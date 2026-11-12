#include "app.hpp"

#include "app_util.hpp"
#include "awake.hpp"
#include "cia.hpp"
#include "crash.hpp"
#include "fetch.hpp"
#include "file_ops.hpp"
#include "finalize.hpp"
#include "fs_util.hpp"
#include "installer.hpp"
#include "logging.hpp"
#include "mdns.hpp"
#include "mdns_responder.hpp"
#include "naming.hpp"
#include "platform.hpp"
#include "qr_code.hpp"
#include "receive_log.hpp"
#include "remote.hpp"
#include "remote_server.hpp"
#include "storage.hpp"
#include "tasks.hpp"
#include "text.hpp"
#include "ui.hpp"
#include "units.hpp"
#include "url.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <deque>
#include <initializer_list>
#include <memory>
#include <optional>
#include <map>
#include <set>

// The download queue, receiving links and files from other devices on the
// network (FBI's remote install protocol and a web page with a file
// manager), and installing a CIA without saving it.

namespace {

using screens::basename;
using screens::plural;

constexpr unsigned short WebPort = 80;
constexpr unsigned short FallbackWebPort = 8080;

std::string nameOf(const std::string &link)
{
    const std::string name = naming::chooseFilename("", link);
    return name == naming::DefaultFilename ? url::host(link) : name;
}

HistoryEntry historyEntry(const std::string &link, const std::string &folder,
                          HistoryStatus status, const std::string &message)
{
    HistoryEntry entry;
    entry.url = link;
    entry.folder = folder;
    entry.status = status;
    entry.message = message;
    return entry;
}

// The console's address on the local network, or "" when it has none.
std::string localAddress()
{
    const long id = gethostid();
    if (id == 0 || id == -1) {
        return "";
    }
    in_addr address {};
    address.s_addr = static_cast<in_addr_t>(id);
    return inet_ntoa(address);
}

// The name the console answers to on the network, as in http://3ds.local/.
constexpr const char *NetworkName = "3ds";

// Listens on a worker thread while the Receive screen is open, and answers
// for 3ds.local on another. What they hear waits here for the screen.
struct ReceiveJob {
    net::Receiver receiver;
    net::MdnsResponder mdns;
    std::atomic<bool> stop {false};
    // Set once mdns has settled on its name; it does not change after.
    std::atomic<bool> named {false};
    LightLock lock;
    std::deque<net::Received> arrived;
    // Devices that asked to use the SD card: those waiting for an answer
    // on the console, and the answers. They last while Receive is open.
    struct Asker {
        std::string from;
        std::string device;
    };
    std::deque<Asker> asking;
    std::set<std::string> allowed;
    std::set<std::string> denied;
    // Devices whose links Don't Allow turned away, a list of their own.
    std::set<std::string> linksDenied;
    // The console's own address: it never sends itself links or asks
    // itself for files, which a link pointing back at it would.
    std::string self;
    // Progress reports and the paths the page changed, in order, until the
    // screen takes them.
    std::deque<net::Transfer> reports;
    std::deque<std::string> changed;
    net::ConsoleStatus status;
    // Requests being served, all but the page's status poll, and how many
    // have started, so one that comes and goes between frames counts too.
    std::atomic<int> requests {0};
    std::atomic<uint32_t> started {0};

    ReceiveJob() { LightLock_Init(&lock); }

    bool take(net::Received &out)
    {
        LightLock_Lock(&lock);
        const bool any = !arrived.empty();
        if (any) {
            out = arrived.front();
            arrived.pop_front();
        }
        LightLock_Unlock(&lock);
        return any;
    }

    bool takeAsking(Asker &out)
    {
        LightLock_Lock(&lock);
        const bool any = !asking.empty();
        if (any) {
            out = asking.front();
            asking.pop_front();
        }
        LightLock_Unlock(&lock);
        return any;
    }

    void answer(const std::string &from, bool allow)
    {
        LightLock_Lock(&lock);
        (allow ? allowed : denied).insert(from);
        (allow ? denied : allowed).erase(from);
        LightLock_Unlock(&lock);
    }

    bool fromConsole(const std::string &from) const
    {
        return from == self || from.rfind("127.", 0) == 0;
    }

    // Runs on the listener's thread.
    bool refusesLinks(const std::string &from)
    {
        LightLock_Lock(&lock);
        const bool refused = fromConsole(from) || linksDenied.count(from) != 0;
        LightLock_Unlock(&lock);
        return refused;
    }

    // Turns a device's links away until Receive closes, those already
    // waiting included.
    void denyLinks(const std::string &from)
    {
        std::deque<net::Received> dropped;
        LightLock_Lock(&lock);
        linksDenied.insert(from);
        for (auto at = arrived.begin(); at != arrived.end();) {
            if (at->from == from && !at->links.empty() && at->replySocket < 0) {
                dropped.push_back(*at);
                at = arrived.erase(at);
            } else {
                ++at;
            }
        }
        LightLock_Unlock(&lock);
        for (net::Received &received : dropped) {
            net::finishReceived(received);
        }
    }

    // Runs on the listener's thread.
    net::Access access(const std::string &from, const std::string &device,
                       bool ask)
    {
        if (fromConsole(from)) {
            return net::Access::Denied;
        }
        LightLock_Lock(&lock);
        net::Access result = net::Access::Pending;
        const bool waiting =
            std::any_of(asking.begin(), asking.end(),
                        [&](const Asker &asker) { return asker.from == from; });
        if (allowed.count(from) != 0) {
            result = net::Access::Allowed;
        } else if (denied.count(from) != 0) {
            result = net::Access::Denied;
        } else if (!waiting && !ask) {
            result = net::Access::Unknown;
        } else if (!waiting) {
            asking.push_back({from, device});
        }
        LightLock_Unlock(&lock);
        return result;
    }

    // What the page shows the console doing; see net::ConsoleStatus.
    // open says the text names nothing private; see net::ConsoleStatus.
    void setState(const std::string &state, const std::string &text = "",
                  bool open = false)
    {
        LightLock_Lock(&lock);
        status.state = state;
        status.text = text;
        status.open = open;
        LightLock_Unlock(&lock);
    }

    void finish(const std::string &result, bool ok, bool open = false)
    {
        LightLock_Lock(&lock);
        status.state = "ready";
        status.text.clear();
        status.result = result;
        status.resultOk = ok;
        status.open = open;
        ++status.resultSeq;
        LightLock_Unlock(&lock);
    }

    // Runs on the listener's thread.
    net::ConsoleStatus snapshot()
    {
        LightLock_Lock(&lock);
        const net::ConsoleStatus copy = status;
        LightLock_Unlock(&lock);
        return copy;
    }

    // Runs on the listener's thread. Only the newest word on a file still on
    // its way is kept, so a long upload while the screen is elsewhere does
    // not pile up reports.
    void progress(const net::Transfer &update)
    {
        LightLock_Lock(&lock);
        if (!reports.empty() && !reports.back().finished &&
            reports.back().path == update.path &&
            reports.back().upload == update.upload) {
            reports.back() = update;
        } else {
            reports.push_back(update);
        }
        LightLock_Unlock(&lock);
    }

    // Runs on the listener's thread.
    void pathChanged(const std::string &path)
    {
        LightLock_Lock(&lock);
        changed.push_back(path);
        LightLock_Unlock(&lock);
    }

    // Everything reported since the last call.
    void takeReports(std::deque<net::Transfer> &newReports,
                     std::deque<std::string> &newChanges)
    {
        LightLock_Lock(&lock);
        newReports.swap(reports);
        newChanges.swap(changed);
        LightLock_Unlock(&lock);
    }
};

void receiveMain(void *argument)
{
    crash::ThreadName name("receive");
    ReceiveJob &job = *static_cast<ReceiveJob *>(argument);
    while (!job.stop.load()) {
        net::Received received;
        if (job.receiver.wait(250, received)) {
            LightLock_Lock(&job.lock);
            job.arrived.push_back(received);
            LightLock_Unlock(&job.lock);
        }
    }
}

void mdnsMain(void *argument)
{
    crash::ThreadName name("mdns");
    ReceiveJob &job = *static_cast<ReceiveJob *>(argument);
    job.mdns.claim();
    job.named.store(true);
    while (!job.stop.load()) {
        job.mdns.serve(250);
    }
}

// Waits for Receive's threads to end once told to stop. They do within a
// quarter of a second; one stuck in the network service must not freeze
// the screen and the HOME button with it, so a longer wait goes on behind
// a busy screen. False when 3dsDL closes first: the threads are then left
// running.
bool waitForThreads(std::initializer_list<Thread> threads)
{
    // 0 only once a thread has ended: a timeout is a status code that
    // R_FAILED does not count as failing.
    const auto ended = [&](u64 timeoutNs) {
        for (const Thread thread : threads) {
            if (thread != nullptr && threadJoin(thread, timeoutNs) != 0) {
                return false;
            }
        }
        return true;
    };
    if (ended(500000000ULL)) {
        return true;
    }
    const uint64_t started = osGetTime();
    bool done = false;
    while (!(done = ended(0)) && ui::keepRunning()) {
        ui::readInput();
        ui::BusyView view;
        view.header = "接收";
        view.title = "正在停止……";
        ui::drawBusy(view);
        gspWaitForVBlank();
    }
    logging::warn("receive.stopping",
                  {{"ms", osGetTime() - started}, {"stopped", done}});
    return done;
}

using Row = net::ReceiveLog::Row;
using RowKind = net::ReceiveLog::Kind;
using RowState = net::ReceiveLog::State;

// A sender in a sentence: "iPhone", or its address when it did not say.
std::string whoIs(const std::string &device, const std::string &address)
{
    return device.empty() ? address : device;
}

// A sender as a fact: "Mac, 192.168.1.24".
std::string senderOf(const std::string &device, const std::string &address)
{
    return device.empty() ? address : device + ", " + address;
}

// The time of day of a moment from osGetTime, which counts from 1900; the
// console clock is already local time.
std::string clockTime(int64_t milliseconds)
{
    const time_t seconds =
        static_cast<time_t>(milliseconds / 1000 - 2208988800LL);
    std::tm parts {};
    gmtime_r(&seconds, &parts);
    char text[8];
    std::snprintf(text, sizeof(text), "%02d:%02d", parts.tm_hour, parts.tm_min);
    return text;
}

int64_t clockNow()
{
    return static_cast<int64_t>(osGetTime());
}

screens::KindLook lookOf(const Row &row)
{
    return screens::lookOf(row.kind == RowKind::Folder
                               ? filetype::Kind::Folder
                               : filetype::fromName(row.name));
}

RowState rowState(HistoryStatus status)
{
    switch (status) {
    case HistoryStatus::Saved:
        return RowState::Received;
    case HistoryStatus::Installed:
        return RowState::Installed;
    case HistoryStatus::Cancelled:
        return RowState::Stopped;
    case HistoryStatus::Failed:
        break;
    }
    return RowState::Failed;
}

ui::ListItem rowItem(const Row &row)
{
    ui::ListItem item;
    item.key = std::to_string(row.id);
    item.label = row.name;
    const screens::KindLook look = lookOf(row);
    item.icon = look.icon;
    item.iconColor = look.color;
    const bool folder = row.kind == RowKind::Folder;
    switch (row.state) {
    case RowState::Active:
        item.progress = net::progress(row);
        item.detail = folder ? std::to_string(row.filesDone) + " / " +
                                   std::to_string(row.files)
                             : std::to_string(static_cast<int>(item.progress * 100)) +
                                   "%";
        break;
    case RowState::Received:
        item.detail = folder ? plural(row.filesDone, "文件") : units::bytes(row.total);
        break;
    case RowState::Sent:
        item.badge = "已发送";
        item.badgeColor = ui::color::Tag;
        break;
    case RowState::Queued:
        item.badge = "已加入下载队列";
        item.badgeColor = ui::color::Badge;
        break;
    case RowState::Installed:
        item.badge = "已安装";
        item.badgeColor = ui::color::Good;
        break;
    case RowState::Failed:
        item.badge = "失败";
        item.badgeColor = ui::color::Bad;
        break;
    case RowState::Stopped:
        item.badge = "已停止";
        item.badgeColor = ui::color::Neutral;
        break;
    }
    return item;
}

// What there is to know about a row, four facts at most.
std::vector<ui::Fact> rowFacts(const Row &row)
{
    std::vector<ui::Fact> facts;
    if (row.state == RowState::Failed && !row.message.empty()) {
        facts.push_back({"失败", row.message, ui::color::Bad});
    }
    if (row.kind == RowKind::Folder) {
        facts.push_back({"文件", row.filesDone >= row.files
                                      ? std::to_string(row.files)
                                      : std::to_string(row.filesDone) + " / " +
                                            std::to_string(row.files)});
    } else if (row.total > 0 && row.done < row.total) {
        facts.push_back({"大小", units::bytes(row.done) + " / " +
                                     units::bytes(row.total)});
    } else if (row.total > 0) {
        facts.push_back({"大小", units::bytes(row.total)});
    }
    facts.push_back({row.upload ? "来源" : "目标", senderOf(row.device, row.from)});
    if (!row.path.empty()) {
        facts.push_back({row.upload ? "已保存到" : "文件夹",
                         row.path.substr(0, row.path.rfind('/') + 1)});
    } else if (!row.url.empty()) {
        facts.push_back({"链接", row.url});
    }
    facts.push_back({"时间", clockTime(row.changed)});
    if (facts.size() > 4) {
        facts.resize(4);
    }
    return facts;
}

// The largest scale up to most at which value fits width, so it is read
// whole rather than scrolled.
float fitScale(const std::string &value, float most, float width)
{
    const float natural = ui::textWidth(value, most);
    return natural <= width ? most : most * width / natural;
}

// The top screen while nothing is on its way: the QR code for the page,
// the address browsers and FBI's tools use beside it, and the console's
// name under that.
void drawConnect(const QrCode &code, const std::string &address,
                 const std::string &named)
{
    ui::drawTopChrome("接收", ui::Icon::Chip, ui::color::IconPink);
    ui::drawPanel();
    const float px = code.size() <= 25 ? 4 : 3;
    const float side = code.size() * px;
    const float qrX = ui::TopPanel.x + 12 + 2 * px;
    code.draw(qrX, ui::TopPanel.y + (ui::TopPanel.h - side) / 2, px);
    const float left = qrX + side + 2 * px + 12;
    const float width = ui::TopPanel.x + ui::TopPanel.w - 12 - left;
    const float middle = left + width / 2;
    // One block, centred beside the code, each line whole.
    const float heroScale = fitScale(address, ui::type::Hero, width);
    const float nameScale = fitScale(named, ui::type::Body, width);
    const float heroHeight = ui::lineHeight(heroScale);
    const float nameHeight = named.empty() ? 0 : 4 + ui::lineHeight(nameScale);
    float y = ui::TopPanel.y + (ui::TopPanel.h - heroHeight - nameHeight) / 2;
    ui::drawMarquee(address, middle, y, heroScale, ui::color::Accent, width,
                    C2D_AlignCenter);
    y += heroHeight + 4;
    if (!named.empty()) {
        ui::drawMarquee(named, middle, y, nameScale, ui::color::Ink, width,
                        C2D_AlignCenter);
    }
}

// How far a transfer has got and how far it goes, in bytes: the whole
// batch's when the page sent several files in one go, else the file's.
struct Span {
    uint64_t done;
    uint64_t total;
};

Span spanOf(const net::Transfer &transfer)
{
    const uint64_t file = transfer.finished && transfer.ok ? transfer.total
                                                           : transfer.done;
    const remote::UploadBatch &batch = transfer.batch;
    if (transfer.upload && batch.count > 1 && batch.bytes > 0) {
        return {std::min(batch.bytes, batch.before + file), batch.bytes};
    }
    return {file, transfer.total};
}

// The top screen while a file is on its way, as the Downloading screen
// shows one, with the address kept at the end of the dotted line. A batch
// reads as one: its bytes, its place and the time left for all of it.
void drawTransfer(const net::Transfer &transfer, const units::RateMeter &meter,
                  const std::string &address)
{
    ui::drawTopChrome("接收", ui::Icon::Chip, ui::color::IconPink, address);
    ui::ProgressView view;
    view.name = transfer.name;
    const screens::KindLook look = screens::lookOf(filetype::fromName(transfer.name));
    view.nameIcon = look.icon;
    view.nameIconColor = look.color;
    view.source = (transfer.upload ? "来自 " : "到 ") +
                  whoIs(transfer.device, transfer.from);
    const Span span = spanOf(transfer);
    view.fraction = span.total > 0
                        ? static_cast<float>(static_cast<double>(span.done) /
                                             static_cast<double>(span.total))
                        : 1.0f;
    view.amounts = units::bytes(span.done) + " / " + units::bytes(span.total);
    if (meter.hasRate() && span.done < span.total) {
        view.speed = units::rate(meter.bytesPerSecond());
        const int64_t left = meter.secondsLeft(span.total - span.done);
        if (left >= 0) {
            view.left = units::timeLeft(static_cast<uint64_t>(left));
        }
    }
    if (transfer.batch.count > 1) {
        view.tag = std::to_string(transfer.batch.position) + " / " +
                   std::to_string(transfer.batch.count);
    }
    ui::drawProgressPanel(view);
}

// A name in a list of what a sender handed over.
struct Named {
    std::string name;
    std::string detail; // beside it: "github.com"
};

// The top screen for a question about what a sender handed over: the names,
// then facts about where they come from and go.
void drawHandedOver(const std::vector<Named> &names,
                    const std::vector<ui::Fact> &facts)
{
    ui::drawTopChrome("接收", ui::Icon::Chip, ui::color::IconPink);
    ui::drawPanel();
    // Four names, or three and how many more.
    const size_t shown = names.size() > 4 ? 3 : names.size();
    const float pitch = 22;
    const float factsHeight = facts.size() * 26.0f - 6;
    const float lines = static_cast<float>(shown + (names.size() > shown ? 1 : 0));
    float y = ui::TopPanel.y +
              (ui::TopPanel.h - lines * pitch - 12 - factsHeight) / 2;
    const float left = ui::TopPanel.x + 18;
    const float right = ui::TopPanel.x + ui::TopPanel.w - 18;
    for (size_t index = 0; index < shown; ++index) {
        const screens::KindLook look =
            screens::lookOf(filetype::fromName(names[index].name));
        ui::drawIcon(look.icon, left, y + 1, 17, look.color);
        const float detailWidth =
            names[index].detail.empty()
                ? 0
                : std::min(110.0f, ui::textWidth(names[index].detail, ui::type::Tiny)) + 10;
        ui::drawMarquee(names[index].name, left + 24,
                        y + (19 - ui::lineHeight(ui::type::Row)) / 2,
                        ui::type::Row, ui::color::InkStrong,
                        right - left - 24 - detailWidth);
        if (detailWidth > 0) {
            ui::drawText(ui::fitEnd(names[index].detail, ui::type::Tiny, 110),
                         right, y + (19 - ui::lineHeight(ui::type::Tiny)) / 2,
                         ui::type::Tiny, ui::color::InkSoft, C2D_AlignRight);
        }
        y += pitch;
    }
    if (names.size() > shown) {
        ui::drawText("另有 " + std::to_string(names.size() - shown) + " 项",
                     left + 24, y + (19 - ui::lineHeight(ui::type::Small)) / 2,
                     ui::type::Small, ui::color::InkSoft);
        y += pitch;
    }
    ui::drawFacts(facts, {ui::TopPanel.x, y + 12, ui::TopPanel.w, factsHeight});
}

} // namespace

bool App::replaceSent(const std::string &newer, const std::string &older,
                      const std::string &host, const std::string &sender)
{
    std::string app;
    if (!remote::sameApp(older, newer, &app)) {
        logging::info("replace.skipped", {{"文件", basename(older)}});
        return false;
    }
    const std::vector<Named> named = {{basename(older), host}};
    const std::vector<ui::Fact> facts = {{"来源", sender},
                                         {"应用", app},
                                         {"已位于", naming::parentFolder(older)},
                                         {"保留为", basename(newer)}};
    ui::Dialog dialog;
    dialog.header = "接收";
    dialog.title = "已存在同名文件。";
    dialog.tone = ui::Tone::Warning;
    dialog.buttons = {"两者都保留", "替换"};
    dialog.dangerous = 1;
    dialog.cancelButton = 0;
    dialog.drawTop = [&]() { drawHandedOver(named, facts); };
    if (ui::choose(dialog) != 1) {
        return false;
    }
    checkSdIndex(); // so only this change counts as explained
    const bool replaced = finalize::replace(newer, older);
    fileIndex_.changed(newer);
    sdChanged(older);
    if (!replaced) {
        logging::warn("replace.failed", {{"文件", basename(older)}});
        ui::message("无法替换", "新文件位于 " + newer + ".",
                    ui::Tone::Error);
        return false;
    }
    logging::info("replace.done", {{"文件", basename(older)}});
    return true;
}

App::SameApp App::sentFolderHoldsSameApp(const std::string &archive,
                                         const SentExtraction &sent,
                                         const archives::Listing &shown)
{
    const std::string folder = std::string(remote::ArchiveTarget) + shown.top + "/";
    if (!shown.topIsFolder || !fs::directoryExists(folder) ||
        fs::listEntries(folder, nullptr, true, false).empty()) {
        return SameApp::Yes;
    }
    // The apps the folder holds, in it or one folder down.
    std::vector<std::string> theirs;
    for (const fs::Entry &entry : fs::listEntries(folder, nullptr, true, false)) {
        if (!entry.folder && filetype::fromName(entry.name) == filetype::Kind::Homebrew) {
            theirs.push_back(folder + entry.name);
        } else if (entry.folder) {
            for (const fs::Entry &inner :
                 fs::listEntries(folder + entry.name + "/", nullptr, true, false)) {
                if (!inner.folder &&
                    filetype::fromName(inner.name) == filetype::Kind::Homebrew) {
                    theirs.push_back(folder + entry.name + "/" + inner.name);
                }
            }
        }
    }
    if (theirs.empty()) {
        return SameApp::No;
    }
    // Each 3dsx in the archive is taken out on its own to compare, as its
    // listing says nothing about what it is; a few are enough.
    const std::string scratch = std::string(storage::DataFolder) + "check/";
    size_t tried = 0;
    for (const archives::Entry &entry : sent.listing.entries) {
        if (entry.directory || entry.path.empty() || tried >= 4) {
            continue;
        }
        const std::string placed = sent.options.place(entry.path, false);
        if (placed.empty() || filetype::fromName(placed) != filetype::Kind::Homebrew) {
            continue;
        }
        ++tried;
        const int64_t free = platform::sdFreeBytes();
        if (free >= 0 && entry.size + (1u << 20) > static_cast<uint64_t>(free)) {
            return SameApp::NoRoom;
        }
        archives::Options one;
        one.only = {entry.path};
        one.existing = archives::Existing::Overwrite;
        one.place = [](const std::string &, bool directory) {
            return directory ? std::string() : std::string("app.3dsx");
        };
        const archives::Result taken =
            tasks::extract(archive, sent.listing, scratch, one);
        bool same = false;
        for (const std::string &app : theirs) {
            same = same || (taken.ok && taken.extracted == 1 &&
                            remote::sameApp(app, scratch + "app.3dsx"));
        }
        fs::removeFolderTree(scratch);
        sdChanged(scratch);
        if (taken.cancelled) {
            return SameApp::Stopped;
        }
        if (same) {
            return SameApp::Yes;
        }
    }
    return SameApp::No;
}

void App::addToQueue(const std::vector<std::string> &links,
                     const std::string &folder, bool announce, bool sent)
{
    size_t added = 0;
    for (const std::string &link : links) {
        const bool already = std::any_of(
            queue_.begin(), queue_.end(), [&](const QueuedLink &item) {
                return item.url == link && item.folder == folder;
            });
        if (already) {
            continue;
        }
        QueuedLink item;
        item.id = nextQueueId_++;
        item.url = link;
        item.folder = folder;
        item.sent = sent;
        queue_.push_back(item);
        ++added;
    }
    if (announce) {
        ui::Notice notice;
        notice.title = added > 0 ? "已加入下载队列" : "已在下载队列中";
        notice.text = std::to_string(queue_.size()) +
                      " 正在主界面的下载队列中等待。";
        notice.icon = ui::Icon::Download;
        notice.iconColor = ui::color::IconGreen;
        ui::notify(notice);
    }
}

void App::runQueue(const std::vector<uint64_t> *ids,
                   std::vector<QueueResult> *results)
{
    const auto report = [results](const std::string &url, HistoryStatus status,
                                  const std::string &name,
                                  const std::string &path, uint64_t bytes,
                                  const std::string &message,
                                  const std::string &filename = "",
                                  const std::string &host = "") {
        if (results != nullptr) {
            results->push_back(
                {url, status, name, path, bytes, message, filename, host});
        }
    };
    std::vector<uint64_t> order;
    for (const QueuedLink &item : queue_) {
        if (ids == nullptr ||
            std::find(ids->begin(), ids->end(), item.id) != ids->end()) {
            order.push_back(item.id);
        }
    }
    if (order.empty()) {
        return;
    }
    if (platform::wifiStatus() == platform::Wifi::Disconnected) {
        ui::message("未连接",
                    "请开启无线通信，然后重新下载队列中的项目。",
                    ui::Tone::Warning);
        return;
    }

    // What to do with CIAs is asked once per run, when the first one lands.
    CiaAction ciaAction = settings_.afterCiaDownload;
    size_t saved = 0;
    std::vector<std::string> savedNames;
    size_t installed = 0;
    size_t failed = 0;
    bool stopped = false;
    for (size_t position = 0; position < order.size() && !stopped; ++position) {
        const auto found = std::find_if(
            queue_.begin(), queue_.end(),
            [&](const QueuedLink &item) { return item.id == order[position]; });
        if (found == queue_.end()) {
            continue;
        }
        QueuedLink item = *found;
        // A sender's link downloads into the Download Folder and moves into
        // the sender's folder only once it is known to belong there, so
        // nothing else is made or left in it.
        const std::string downloadTo = item.sent ? settings_.destination : item.folder;
        if (!fs::makeDirectories(downloadTo)) {
            found->state = QueuedLink::State::Failed;
            found->message = "无法创建：" + downloadTo + ".";
            report(item.url, HistoryStatus::Failed, "", "", 0, found->message);
            ++failed;
            continue;
        }

        net::Request request;
        request.url = item.url;
        request.folder = downloadTo;
        // Never the console itself, and for a link another site sent,
        // nothing on this network.
        request.refuse = [foreign = item.foreign](const std::string &to,
                                                  const std::string &from) {
            return remote::addressRefused(to, from, foreign);
        };
        request.caFile = http::CaBundle;
        request.userAgent = http::UserAgent;
        request.allowResume = settings_.resumeDownloads;
        request.socketBufferSize = http::SocketBufferSize;
        request.connections = settings_.maxConnections;
        request.freeSpace = [](const std::string &) {
            return platform::sdFreeBytes();
        };
        const std::string step = order.size() > 1
                                     ? std::to_string(position + 1) + " / " +
                                           std::to_string(order.size())
                                     : "";
        const net::Result result = tasks::download(request, step);
        if (!ui::keepRunning()) {
            return;
        }
        if (result.outcome == net::Outcome::Cancelled) {
            found->state = QueuedLink::State::Stopped;
            found->message = "已停止。";
            HistoryEntry stoppedEntry = historyEntry(
                item.url, downloadTo, HistoryStatus::Cancelled, result.message);
            stoppedEntry.name = result.filename;
            record(stoppedEntry);
            report(item.url, HistoryStatus::Cancelled, result.filename, "",
                   result.bytes, "");
            stopped = true;
            break;
        }
        if (result.outcome != net::Outcome::Completed) {
            found->state = QueuedLink::State::Failed;
            found->message = result.message;
            HistoryEntry entry = historyEntry(item.url, downloadTo,
                                              HistoryStatus::Failed,
                                              result.message);
            entry.resumable = result.resumable;
            entry.bytes = result.bytes;
            entry.name = result.filename;
            record(entry);
            report(item.url, HistoryStatus::Failed, result.filename, "",
                   result.bytes, result.message);
            ++failed;
            continue;
        }

        // A queue never stops to ask about names: a clash keeps both. A
        // sender's folder takes only apps and archives, and only from where
        // the link was found to lead before the console asked; anything
        // else it sent goes to the Download Folder.
        const std::string came =
            url::host(result.effectiveUrl.empty() ? item.url : result.effectiveUrl);
        const bool fromChecked =
            !item.host.empty() && url::site(came) == url::site(item.host);
        const bool belongs = fromChecked && remote::sentFileAllowed(result.filename);
        std::string folder = item.folder;
        if (item.sent && belongs && fs::makeDirectories(item.folder)) {
            sdChanged(item.folder);
        } else if (item.sent) {
            folder = settings_.destination;
            logging::info("receive.moved", {{"文件", result.filename},
                                            {"why", fromChecked ? "type" : "host"}});
        }
        const finalize::Result placed =
            finalize::commit(result.partPath, folder, result.filename,
                             false, platform::now());
        HistoryEntry entry = historyEntry(
            item.url, folder,
            placed.ok ? HistoryStatus::Saved : HistoryStatus::Failed,
            placed.ok ? "已从下载队列保存" : placed.message);
        entry.bytes = result.bytes;
        entry.path = placed.path;
        entry.name = result.filename;
        record(entry);
        report(item.url, placed.ok ? HistoryStatus::Saved : HistoryStatus::Failed,
               placed.ok ? basename(placed.path) : result.filename, placed.path,
               result.bytes, placed.ok ? "" : placed.message, result.filename,
               came);
        if (!placed.ok) {
            found->state = QueuedLink::State::Failed;
            found->message = placed.message;
            ++failed;
            continue;
        }
        queue_.erase(found);
        ++saved;
        savedNames.push_back(basename(placed.path));

        if (!cia::isCiaFile(placed.path) || !installer::available()) {
            continue;
        }
        if (ciaAction == CiaAction::Ask) {
            ui::Dialog dialog;
            dialog.header = "下载队列";
            dialog.title = "是否安装此下载队列中的 CIA？";
            dialog.tone = ui::Tone::Info;
            dialog.buttons = {"安装", "安装后删除", "保留文件"};
            dialog.cancelButton = 2;
            const int choice = ui::choose(dialog);
            ciaAction = choice == 0   ? CiaAction::Install
                        : choice == 1 ? CiaAction::InstallAndDelete
                                      : CiaAction::Keep;
        }
        if (ciaAction == CiaAction::Keep) {
            continue;
        }
        cia::Info info;
        std::string error;
        if (readCia(placed.path, info, error) &&
            installCia(placed.path, info,
                       ciaAction == CiaAction::InstallAndDelete, item.url,
                       true)) {
            ++installed;
            if (results != nullptr) {
                results->back().status = HistoryStatus::Installed;
            }
        }
    }
    if (results != nullptr) {
        return; // the caller shows how each went
    }

    std::string summary = plural(saved, "文件") + " 已下载";
    if (installed > 0) {
        summary += ", " + std::to_string(installed) + " 已安装";
    }
    if (failed > 0) {
        summary += ", " + std::to_string(failed) + " 失败";
    }
    summary += ".";
    if (stopped) {
        summary += " 下载队列已停止；剩余项目仍在队列中。";
    } else if (failed > 0) {
        summary += " 失败的项目保留在下载队列中，以便重试。";
    }
    // The names go on the top screen, which would otherwise show the last
    // progress frame behind the dialog.
    std::string names;
    for (size_t index = 0; index < savedNames.size() && index < 6; ++index) {
        names += (index > 0 ? ", " : "") + savedNames[index];
    }
    if (savedNames.size() > 6) {
        names += " 和 " + std::to_string(savedNames.size() - 6) + " 项";
    }
    ui::message(failed == 0 && !stopped ? "下载队列处理完成" : "下载队列处理结束",
                summary,
                failed == 0 && !stopped ? ui::Tone::Success
                                        : ui::Tone::Warning,
                names.empty() ? "" : "已保存：" + names + ".");
}

void App::queueScreen()
{
    int cursor = 0;
    while (ui::keepRunning() && !quit_) {
        ui::ListSpec spec;
        spec.title = "下载队列";
        spec.emptyText = "没有等待中的项目。";
        for (const QueuedLink &item : queue_) {
            ui::ListItem row;
            row.label = nameOf(item.url);
            row.icon = ui::Icon::Download;
            row.iconColor = ui::color::IconGreen;
            if (item.state == QueuedLink::State::Failed) {
                row.badge = "失败";
                row.badgeColor = ui::color::Bad;
            } else if (item.state == QueuedLink::State::Stopped) {
                row.badge = "已停止";
                row.badgeColor = ui::color::Neutral;
            } else {
                row.detail = url::host(item.url);
            }
            spec.items.push_back(row);
        }
        if (!queue_.empty()) {
            spec.action = "全部下载";
            spec.actionKey = "X";
            spec.actionKeys = KEY_X;
        }
        spec.drawTop = [&](int highlighted) {
            ui::drawTopChrome("下载队列", ui::Icon::Download, ui::color::IconGreen);
            ui::drawPanel();
            if (highlighted < 0 || highlighted >= static_cast<int>(queue_.size())) {
                ui::drawEmblem(ui::Icon::Download, ui::color::IconGreen);
                return;
            }
            const QueuedLink &item = queue_[highlighted];
            std::vector<ui::Fact> facts = {{"网址", item.url},
                                           {"保存到", item.folder}};
            if (!item.message.empty()) {
                const bool failed = item.state == QueuedLink::State::Failed;
                facts.push_back({failed ? "失败" : "备注", item.message,
                                 failed ? ui::color::Bad : ui::color::Neutral});
            } else {
                facts.push_back({"位置", std::to_string(highlighted + 1) +
                                                 " / " +
                                                 std::to_string(queue_.size())});
            }
            // The name beside its icon, then the three fact rows (20 px,
            // 26 apart), centred in the panel as one block.
            constexpr float Header = 30 + 14;
            constexpr float Rows = 3 * 26 - 6;
            const float top =
                ui::TopPanel.y + (ui::TopPanel.h - Header - Rows) / 2;
            const float left = ui::TopPanel.x + 18;
            ui::drawIcon(ui::Icon::Download, left, top, 30,
                         ui::color::IconGreen);
            ui::drawMarquee(nameOf(item.url), left + 40,
                            top + (30 - ui::lineHeight(0.62f)) / 2, 0.62f,
                            ui::color::InkStrong, 300);
            ui::drawFacts(facts, {ui::TopPanel.x, top + Header,
                                  ui::TopPanel.w, Rows});
        };
        const ui::ListResult result = ui::runList(spec, cursor);
        if (result.key == KEY_X && !queue_.empty()) {
            runQueue();
            continue;
        }
        if (result.key != KEY_A || result.index < 0 ||
            result.index >= static_cast<int>(queue_.size())) {
            return;
        }
        const QueuedLink item = queue_[result.index];
        const int choice = ui::menu(
            {{"立即下载", false, true},
             {"移除", true, true},
             {"全部移除", true, true}},
            [&](int) {
                ui::drawTopChrome("下载队列", ui::Icon::Download,
                                  ui::color::IconGreen);
                ui::drawPanel();
                ui::drawPanelText(nameOf(item.url), ui::TopPanel);
            });
        if (choice == 0) {
            const std::vector<uint64_t> only = {item.id};
            runQueue(&only);
        } else if (choice == 1) {
            queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                        [&](const QueuedLink &link) {
                                            return link.id == item.id;
                                        }),
                         queue_.end());
        } else if (choice == 2) {
            ui::Dialog dialog;
            dialog.header = "下载队列";
            dialog.title = "是否将 " + plural(queue_.size(), "链接") +
                           " 从下载队列中移除？";
            dialog.tone = ui::Tone::Warning;
            dialog.buttons = {"移除", "取消"};
            dialog.dangerous = 0;
            dialog.initial = 1;
            dialog.cancelButton = 1;
            if (ui::choose(dialog) == 0) {
                queue_.clear();
            }
        }
    }
}

void App::receiveScreen()
{
    const std::string address = localAddress();
    if (platform::wifiStatus() == platform::Wifi::Disconnected ||
        address.empty()) {
        ui::message("未连接", "请连接 Wi-Fi 以接收。",
                    ui::Tone::Warning);
        return;
    }

    // On the heap, so a thread that never stops can keep it.
    std::unique_ptr<ReceiveJob> owned(new ReceiveJob());
    ReceiveJob &job = *owned;
    std::string error;
    if (!job.receiver.open(remote::FbiPort, WebPort, FallbackWebPort, error)) {
        ui::message("无法接收", error, ui::Tone::Error);
        return;
    }
    net::FileSharing sharing;
    sharing.root = "sdmc:";
    // The page's sidebar: the Download Folder, the usual homebrew folders
    // and Files' favourites, each once.
    std::set<std::string> listed;
    const auto place = [&](std::vector<net::Place> &into, std::string path) {
        while (path.size() > 6 && path.back() == '/') {
            path.pop_back();
        }
        if (path != "sdmc:/" && listed.insert(text::lower(path)).second) {
            into.push_back({fileops::name(path), path});
        }
    };
    place(sharing.places, settings_.destination);
    if (!sharing.places.empty()) {
        sharing.places.back().name = "下载";
    }
    for (const char *folder : {"sdmc:/3ds", "sdmc:/cias", "sdmc:/3ds/Checkpoint"}) {
        place(sharing.places, folder);
    }
    for (const std::string &favorite : settings_.favorites) {
        place(sharing.favorites, favorite);
    }
    job.self = localAddress();
    sharing.access = [&job](const std::string &from, const std::string &device,
                            bool ask) { return job.access(from, device, ask); };
    sharing.refuseLinks = [&job](const std::string &from) {
        return job.refusesLinks(from);
    };
    sharing.freeSpace = []() { return platform::sdFreeBytes(); };
    sharing.totalSpace = []() { return platform::sdTotalBytes(); };
    sharing.status = [&job]() { return job.snapshot(); };
    sharing.appVersion = APP_VERSION;
    sharing.editorScript = "romfs:/web/editor.js.gz";
    sharing.progress = [&job](const net::Transfer &transfer) {
        job.progress(transfer);
    };
    sharing.changed = [&job](const std::string &path) { job.pathChanged(path); };
    sharing.working = [&job](bool starting) {
        if (starting) {
            ++job.requests;
            ++job.started;
        } else {
            --job.requests;
        }
    };
    // Room for the network to keep filling while an upload's last piece is
    // written to the SD card, so the sender is not told to wait.
    sharing.socketBufferSize = 128 * 1024;
    if (fs::makeDirectories(storage::DataFolder)) {
        sharing.stagingFolder = storage::DataFolder;
    }
    // Leaving Receive stops whatever request is being served, rather than
    // waiting for an upload or a big delete to finish.
    sharing.cancelled = [&job]() { return job.stop.load(); };
    job.receiver.shareFiles(sharing);

    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    const int workerPriority = std::min(0x3f, static_cast<int>(priority) + 1);
    // A New 3DS's third core is free, so receiving a file never waits for
    // drawing; elsewhere the listener shares the app core.
    bool isNew3ds = false;
    Thread thread =
        R_SUCCEEDED(APT_CheckNew3DS(&isNew3ds)) && isNew3ds
            ? threadCreate(receiveMain, &job, 32 * 1024, workerPriority, 2,
                           false)
            : nullptr;
    if (thread == nullptr) {
        thread = threadCreate(receiveMain, &job, 32 * 1024, workerPriority,
                              -2, false);
    }
    if (thread == nullptr) {
        ui::message("无法接收", "无法启动网络监听服务。",
                    ui::Tone::Error);
        return;
    }
    // 3ds.local is a convenience: without multicast the address still works.
    std::string mdnsError;
    Thread mdnsThread =
        job.mdns.open(NetworkName, static_cast<uint32_t>(gethostid()),
                      mdns::Port, true, mdnsError)
            ? threadCreate(mdnsMain, &job, 16 * 1024, workerPriority, -2, false)
            : nullptr;

    // Browsers and FBI's tools reach the console at the same address; the
    // page needs its port too when it could not have port 80.
    const std::string port =
        job.receiver.webPort() == 80
            ? std::string()
            : ":" + std::to_string(job.receiver.webPort());
    const std::string shownAddress = address + port;
    const QrCode code("http://" + shownAddress + "/");
    // Only log.txt says whether FBI's tools could reach the console.
    const bool fbiListening = job.receiver.fbiPort() != 0;
    // Shown from the start: claiming the name takes a second or so, and only
    // another console on Receive moves it on to 3ds-2.
    std::string named = mdnsThread != nullptr
                            ? "http://" + std::string(NetworkName) + ".local" + port
                            : "";
    bool nameSettled = false;
    std::string hostname; // for log.txt, once settled

    std::optional<awake::Hold> keptAwake;
    uint32_t requestsSeen = 0;
    // The newest report, for the top screen, which shows a finished file a
    // moment longer so a batch does not flicker between its files.
    net::Transfer current;
    int64_t heard = 0;
    units::RateMeter meter;
    constexpr int64_t LingerMs = 1500;
    const int64_t opened = clockNow();
    // For log.txt, once Receive closes.
    uint32_t filesIn = 0;
    uint32_t filesOut = 0;
    uint32_t linksIn = 0;
    uint32_t installed = 0;
    uint32_t failed = 0;
    uint64_t bytesIn = 0;

    const auto takeReports = [&]() {
        std::deque<net::Transfer> reports;
        std::deque<std::string> changes;
        job.takeReports(reports, changes);
        const int64_t now = clockNow();
        for (const net::Transfer &report : reports) {
            // A batch is measured as one, so its speed and time left carry
            // on from file to file.
            const bool sameBatch = !report.batch.id.empty() &&
                                   report.batch.id == current.batch.id;
            if (!sameBatch && (report.path != current.path ||
                               report.upload != current.upload)) {
                meter.reset();
            }
            current = report;
            heard = now;
            meter.sample(static_cast<uint64_t>(now), spanOf(report).done);
            if (report.finished && report.ok && report.upload) {
                ++filesIn;
                bytesIn += report.total;
            } else if (report.finished && report.ok) {
                ++filesOut;
            } else if (report.finished) {
                ++failed;
            }
            receiveLog_.transfer(report, now);
        }
        for (const std::string &path : changes) {
            sdChanged(path);
            // A folder the page merged into may hold files written over at
            // the same size.
            if (fileops::isFolder(path)) {
                fileIndex_.rewritten(path);
            }
        }
        receiveLog_.settle(now);
        return changes;
    };
    takePageChanges_ = takeReports;
    struct Unhook {
        App &app;
        ~Unhook() { app.takePageChanges_ = nullptr; }
    } unhook{*this};

    // Someone using the page holds the console awake, and the linger after
    // each request covers the gap before the next, so closing the lid never
    // cuts a batch off between files (issue #13).
    const auto holdAwake = [&]() {
        const uint32_t started = job.started.load();
        const bool inUse = job.requests.load() > 0 || started != requestsSeen;
        requestsSeen = started;
        if (inUse != keptAwake.has_value()) {
            if (keptAwake) {
                keptAwake.reset();
            } else {
                keptAwake.emplace();
            }
        }
    };

    const auto askAbout = [&](const ReceiveJob::Asker &asker) {
        const std::string who = whoIs(asker.device, asker.from);
        ui::Dialog dialog;
        dialog.header = "接收";
        dialog.title = "是否允许 " + who + " 管理 SD 卡上的文件？";
        dialog.tone = ui::Tone::Warning;
        dialog.buttons = {"允许", "不允许"};
        dialog.cancelButton = 1;
        dialog.drawTop = [&]() {
            ui::drawTopChrome("接收", ui::Icon::Chip, ui::color::IconPink);
            ui::drawPanel();
            std::vector<ui::Fact> facts;
            if (!asker.device.empty()) {
                facts.push_back({"设备", asker.device});
            }
            facts.push_back({"地址", asker.from});
            facts.push_back({"有效期至", "离开“接收”界面"});
            ui::drawFacts(facts);
        };
        job.setState("asking", "正在询问是否允许 " + who + " 使用 SD 卡。");
        job.answer(asker.from, ui::choose(dialog) == 0);
        job.setState("ready");
    };

    // Installs CIAs on the SD card that the page sent, telling it how that
    // went; the list on the console shows each.
    const auto installSent = [&](const net::Received &received, bool deleteAfter) {
        const size_t count = received.install.size();
        size_t done = 0;
        // Each one that did not go in, with why, for the page.
        std::vector<std::string> reasons;
        for (size_t index = 0; index < count && ui::keepRunning(); ++index) {
            const std::string &path = received.install[index];
            job.setState("installing",
                         "正在安装 " + basename(path) +
                             (count > 1 ? " (" + std::to_string(index + 1) +
                                              " / " + std::to_string(count) + ")"
                                        : "") +
                             ".");
            cia::Info info;
            std::string why;
            bool already = false;
            const bool ok = readCia(path, info, why) &&
                            installCia(path, info, deleteAfter, "", true,
                                       &already, &why);
            const bool went = ok && !already;
            if (!went) {
                if (why.empty()) {
                    why = "未安装。";
                }
                reasons.push_back(basename(path) + ": " + why);
                logging::info("receive.install", {{"文件", basename(path)},
                                                  {"why", why}});
            }
            receiveLog_.installEnded(path, received, ok, went ? "" : why, clockNow());
            done += went ? 1 : 0;
        }
        // Stopped before the rest: those were never tried.
        for (size_t index = reasons.size() + done; index < count; ++index) {
            reasons.push_back(basename(received.install[index]) +
                              "：3dsDL 在安装前已关闭。");
        }
        installed += static_cast<uint32_t>(done);
        std::string text =
            done == count
                ? (count == 1 ? "已安装 " + basename(received.install[0]) + "."
                              : "已全部安装，共 " + plural(count, "CIA") + ".")
            : count == 1 ? "未安装。"
                         : "已安装 " + std::to_string(done) + " / " +
                               std::to_string(count) + ". ";
        for (size_t index = 0; index < reasons.size(); ++index) {
            text += (index > 0 ? " " : "") + reasons[index];
        }
        job.finish(text, done == count);
    };

    // A link the queue kept beside a file of the same name, in the folder
    // its sender named: now that the server's name for it is known, the
    // console says whether it replaces older. True when it did.
    const auto askToReplace = [&](QueueResult &result, const std::string &older,
                                  const std::string &sender) {
        job.setState("choosing", "请在 3DS 上进行选择。", true);
        if (!replaceSent(result.path, older, result.host, sender)) {
            return false;
        }
        for (HistoryEntry &entry : history_) {
            if (entry.path == result.path) {
                entry.path = older;
            }
        }
        storage::saveHistory(history_);
        result.path = older;
        result.name = basename(older);
        return true;
    };

    // A sender's extract=1, once its archive is saved. A full read, as every
    // entry is checked before anything is asked; only compressed tars and
    // solid RARs cost more that way. agreed: the console said yes before the
    // download.
    const auto offerSentExtraction = [&](const QueueResult &result,
                                         bool agreed, const std::string &sender) {
        SentExtraction sent;
        std::string error;
        job.setState("busy", "正在 3DS 上读取压缩包。", true);
        const bool read = screens::readArchive(result.path, archives::Scan::Full,
                                               sent.listing, error);
        job.setState("choosing", "请在 3DS 上进行选择。", true);
        if (!read) {
            if (!error.empty()) {
                ui::message("无法解压", error, ui::Tone::Error,
                            "已保存为 " + result.path + ".");
            }
            return;
        }
        archives::Listing shown;
        // The server's name, not the one kept beside an older copy, so a
        // second send goes into the same folder.
        if (!remote::sentExtraction(sent.listing, result.filename,
                                    sent.options, shown)) {
            logging::warn("extract.refused", {{"文件", basename(result.path)}});
            ui::message("未解压",
                        "发送的压缩包只能解压到 /3ds/ 下的一个文件夹中。",
                        ui::Tone::Warning, "已保存为 " + result.path + ".");
            return;
        }
        // What it adds to a folder already there, for the question.
        std::string summary = "已保存为 " + result.path + ".";
        const std::string into = std::string(remote::ArchiveTarget) + shown.top + "/";
        std::vector<std::string> clashes;
        if (shown.topIsFolder && fs::directoryExists(into) &&
            archives::existingFiles(shown, remote::ArchiveTarget, clashes)) {
            const std::set<std::string> there(clashes.begin(), clashes.end());
            std::vector<std::string> added;
            for (const archives::Entry &entry : shown.entries) {
                if (!entry.directory && there.count(entry.path) == 0) {
                    added.push_back(basename(entry.path));
                }
            }
            if (!added.empty()) {
                summary += " 将添加 " + plural(added.size(), "新文件") + " 到 " +
                           into + ": " + added[0];
                for (size_t index = 1; index < added.size() && index < 3; ++index) {
                    summary += ", " + added[index];
                }
                summary += added.size() > 3
                               ? " 和 " + std::to_string(added.size() - 3) + " 项。"
                               : ".";
            }
        }
        sent.agreed = agreed;
        sent.host = result.host;
        sent.sender = sender;
        // The page's text names no file, as any device on the network sees it.
        sent.extracting = [&](bool started) {
            if (started) {
                job.setState("busy", "正在 3DS 上解压。", true);
            } else {
                job.setState("choosing", "请在 3DS 上进行选择。",
                             true);
            }
        };
        job.setState("choosing", "请在 3DS 上进行选择。", true);
        offerExtraction(result.path, remote::ArchiveTarget, summary,
                        "是否解压到 /3ds/？", &shown, &sent);
    };

    // Addresses whose links were turned down while Receive is open: their
    // next question offers Don't Allow in place of Add to Queue.
    std::set<std::string> turnedDown;

    // What a sender handed over: CIAs on the SD card to install, or links.
    const auto handle = [&](net::Received &received) {
        logging::info("receive.got",
                      {{"links", received.links.size()},
                       {"cias", received.install.size()},
                       {"fbi", received.replySocket >= 0},
                       {"folder", received.options.folder},
                       {"replace", received.options.replace ? "yes" : ""},
                       {"extract", received.options.extract ? "yes" : ""}});
        // Links from a device turned down since they arrived, or from the
        // console itself, go no further.
        if (!received.links.empty() && received.replySocket < 0 &&
            job.refusesLinks(received.from)) {
            logging::info("receive.refused", {{"from", received.from}});
            net::finishReceived(received);
            return;
        }
        const std::string who = whoIs(received.device, received.from);
        const std::string sender = senderOf(received.device, received.from);
        const std::vector<ui::Fact> from = {
            {"来源", senderOf(received.device, received.from)}};
        if (!received.install.empty()) {
            std::vector<Named> names;
            for (const std::string &path : received.install) {
                names.push_back({basename(path), ""});
            }
            ui::Dialog dialog;
            dialog.header = "接收";
            dialog.title = "是否再次安装 " + plural(received.install.size(), "CIA") +
                           "，来自 " + who + "?";
            dialog.tone = ui::Tone::Info;
            dialog.buttons = {"安装", "安装后删除", "暂不处理"};
            dialog.cancelButton = 2;
            dialog.drawTop = [&]() { drawHandedOver(names, from); };
            job.setState("choosing", "请在 3DS 上进行选择。");
            const int choice = ui::choose(dialog);
            if (choice == 0 || choice == 1) {
                installSent(received, choice == 1);
            } else {
                job.finish("未安装：已在 3DS 上选择暂不处理。", false);
            }
            return;
        }

        // FBI's tools send CIAs to install, as FBI did, without keeping a
        // copy; they wait for the reply, so nothing they send can be queued.
        const bool fromFbiTool = received.replySocket >= 0;
        const bool allCias = std::all_of(
            received.links.begin(), received.links.end(), [](const std::string &link) {
                return filetype::fromName(nameOf(link)) == filetype::Kind::Cia;
            });
        // Where each link leads once its redirects are followed, found before
        // asking: the question names where the files really come from, and
        // what is downloaded is held to it (QueuedLink::host).
        std::map<std::string, std::string> leadsTo;
        // Never the console itself, and for links another site sent,
        // nothing on this network: the check would otherwise reach them
        // before anyone agreed to anything.
        const bool anotherSite = !fromFbiTool && !received.ownPage;
        std::set<std::string> refused;
        job.setState("busy", "正在 3DS 上检查链接。", true);
        const uint64_t checkStarted = osGetTime();
        const bool checked = tasks::busy(
            "接收", "正在检查 " + plural(received.links.size(), "链接") + "...",
            [&](tasks::Busy &busy) {
                busy.total = received.links.size();
                for (const std::string &link : received.links) {
                    if (busy.cancel.load()) {
                        break;
                    }
                    net::FetchRequest request;
                    request.url = link;
                    request.caFile = http::CaBundle;
                    request.userAgent = http::UserAgent;
                    request.connectTimeoutSeconds = 10;
                    request.totalTimeoutSeconds = 15;
                    request.cancel = &busy.cancel;
                    request.refuse = [anotherSite](const std::string &to,
                                                   const std::string &from) {
                        return remote::addressRefused(to, from, anotherSite);
                    };
                    const net::Destination destination = net::whereTo(request);
                    if (destination.ok) {
                        leadsTo[link] = url::host(destination.url);
                    } else if (destination.refused) {
                        refused.insert(link);
                    }
                    ++busy.done;
                }
            },
            true);
        size_t elsewhere = 0;
        for (const auto &found : leadsTo) {
            elsewhere += found.second != url::host(found.first) ? 1 : 0;
        }
        logging::info("receive.checked",
                      {{"links", received.links.size()},
                       {"elsewhere", elsewhere},
                       {"refused", refused.size()},
                       {"unknown", received.links.size() - leadsTo.size()},
                       {"ms", osGetTime() - checkStarted}});
        if (!checked) {
            job.finish("未下载：已在 3DS 上停止检查。", false, true);
            net::finishReceived(received);
            return;
        }
        received.links.erase(
            std::remove_if(received.links.begin(), received.links.end(),
                           [&](const std::string &link) {
                               return refused.count(link) != 0;
                           }),
            received.links.end());
        if (received.links.empty()) {
            job.finish("未下载：链接指向此局域网中的地址。",
                       false, true);
            net::finishReceived(received);
            return;
        }
        // Comes from the site the check found its link to lead to.
        const auto fromChecked = [&](const QueueResult &result) {
            const auto found = leadsTo.find(result.url);
            return found != leadsTo.end() &&
                   url::site(found->second) == url::site(result.host);
        };

        enum class Choice { Install, Download, Queue, Cancel, DontAllow };
        // Once a device's links were turned down, Don't Allow takes Add to
        // Queue's place, to stop a page that keeps sending.
        const bool turnedDownBefore = turnedDown.count(received.from) > 0;
        const std::vector<Choice> choices =
            fromFbiTool && allCias && installer::available()
                ? std::vector<Choice>{Choice::Install, Choice::Download, Choice::Cancel}
            : fromFbiTool ? std::vector<Choice>{Choice::Download, Choice::Cancel}
            : turnedDownBefore
                ? std::vector<Choice>{Choice::Download, Choice::Cancel,
                                      Choice::DontAllow}
                : std::vector<Choice>{Choice::Download, Choice::Queue, Choice::Cancel};
        const bool offerInstall = choices.front() == Choice::Install;
        const size_t count = received.links.size();
        std::vector<Named> names;
        for (const std::string &link : received.links) {
            const auto found = leadsTo.find(link);
            names.push_back(
                {nameOf(link), found != leadsTo.end() ? found->second : url::host(link)});
        }
        const remote::SendOptions &options = received.options;
        const std::string folder =
            options.folder.empty() ? settings_.destination : options.folder;
        std::vector<ui::Fact> facts = from;
        if (!offerInstall) {
            facts.push_back({"保存到", folder});
        }
        // Holds the links just queued to where the check found them to lead,
        // or to nowhere when it could not tell.
        const auto holdToChecked = [&]() {
            for (QueuedLink &item : queue_) {
                if (item.folder != folder ||
                    std::find(received.links.begin(), received.links.end(),
                              item.url) == received.links.end()) {
                    continue;
                }
                const auto found = leadsTo.find(item.url);
                item.host = found != leadsTo.end() ? found->second : "";
                item.foreign = anotherSite;
            }
        };
        ui::Dialog dialog;
        dialog.header = "接收";
        dialog.title = offerInstall
                           ? "是否再次安装 " + plural(count, "CIA") + "，来自 " + who + "?"
                           : "下载 " + plural(count, "文件") + "，来自 " + who + "?";
        dialog.tone = ui::Tone::Info;
        for (const Choice choice : choices) {
            dialog.buttons.push_back(choice == Choice::Install     ? "安装"
                                     : choice == Choice::Download  ? "下载"
                                     : choice == Choice::Queue     ? "加入下载队列"
                                     : choice == Choice::DontAllow ? "不允许"
                                                                   : "取消");
        }
        dialog.cancelButton = static_cast<int>(
            std::find(choices.begin(), choices.end(), Choice::Cancel) - choices.begin());
        // Another site may have sent these without the person asking, so
        // its question starts on Cancel.
        if (!fromFbiTool && !received.ownPage) {
            dialog.initial = dialog.cancelButton;
        }
        dialog.drawTop = [&]() { drawHandedOver(names, facts); };
        job.setState("choosing", "请在 3DS 上进行选择。", true);
        const int picked = ui::choose(dialog);
        const Choice choice = picked >= 0 && picked < static_cast<int>(choices.size())
                                  ? choices[picked]
                                  : Choice::Cancel;
        const std::string links = plural(count, "链接");

        if (choice == Choice::Install) {
            receiveLog_.addLinks(received, RowState::Active, clockNow());
            job.setState("installing", "正在安装 " + links + ".", true);
            size_t done = 0;
            for (const std::string &link : received.links) {
                std::string why;
                const HistoryStatus status = installDirectly(link, true, &why);
                receiveLog_.linkEnded(link, rowState(status), "", "", 0, why,
                                      clockNow());
                done += status == HistoryStatus::Installed ? 1 : 0;
                if (status == HistoryStatus::Cancelled || !ui::keepRunning()) {
                    break;
                }
            }
            receiveLog_.settleLinks(RowState::Stopped, clockNow());
            linksIn += static_cast<uint32_t>(count);
            installed += static_cast<uint32_t>(done);
            job.finish(done == count
                           ? "已安装 " + plural(count, "CIA") + "（在 3DS 上）。"
                           : "已安装 " + std::to_string(done) + " / " +
                                 plural(count, "CIA") + "（在 3DS 上）。",
                       done == count, true);
        } else if (choice == Choice::Download) {
            // With Ask to Extract off, a sender's extract=1 on links to
            // archives is put to the console now, before anything downloads,
            // starting on Not Now. With it on, archives are offered after.
            bool agreed = false;
            if (options.extract && !settings_.offerZipExtraction) {
                const size_t archiveLinks = static_cast<size_t>(std::count_if(
                    received.links.begin(), received.links.end(),
                    [](const std::string &link) {
                        return filetype::fromName(nameOf(link)) ==
                               filetype::Kind::Archive;
                    }));
                if (archiveLinks > 0) {
                    ui::Dialog question;
                    question.header = "接收";
                    question.title =
                        archiveLinks == 1
                            ? "是否在下载完成后解压此压缩包？"
                            : "是否在下载完成后解压这些压缩包？";
                    question.tone = ui::Tone::Info;
                    question.buttons = {"解压", "暂不处理"};
                    question.initial = 1;
                    question.cancelButton = 1;
                    // Two facts, as the top screen fits beside four names.
                    std::vector<ui::Fact> where = from;
                    where.push_back({"解压到", remote::ArchiveTarget});
                    question.drawTop = [&]() { drawHandedOver(names, where); };
                    agreed = ui::choose(question) == 0;
                }
            }
            const bool extract =
                options.extract && (agreed || settings_.offerZipExtraction);
            turnedDown.erase(received.from);
            const uint64_t first = nextQueueId_;
            addToQueue(received.links, folder, false, !options.folder.empty());
            holdToChecked();
            std::vector<uint64_t> ids;
            for (const QueuedLink &item : queue_) {
                // When the sender named a folder, the same link queued for
                // another folder stays in the Queue.
                if (item.id >= first ||
                    ((options.folder.empty() || item.folder == folder) &&
                     std::find(received.links.begin(), received.links.end(),
                               item.url) != received.links.end())) {
                    ids.push_back(item.id);
                }
            }
            receiveLog_.addLinks(received, RowState::Active, clockNow());
            job.setState("downloading", "正在下载 " + links + ".", true);
            std::vector<QueueResult> results;
            runQueue(&ids, &results);
            // Questions come once the downloads are done, as the queue never
            // stops to ask in between, and not at all after a stop.
            const bool stopped = std::any_of(
                results.begin(), results.end(), [](const QueueResult &result) {
                    return result.status == HistoryStatus::Cancelled;
                });
            if (options.replace && !options.folder.empty() && !stopped) {
                // Never "older" than a file this same send saved, in any
                // case, as FAT ignores it.
                std::set<std::string> saved;
                for (const QueueResult &result : results) {
                    saved.insert(text::lower(result.path));
                }
                for (QueueResult &result : results) {
                    if (!ui::keepRunning()) {
                        break;
                    }
                    const bool kept =
                        fromChecked(result) &&
                        (result.status == HistoryStatus::Saved ||
                         (result.status == HistoryStatus::Installed &&
                          fs::fileExists(result.path)));
                    const std::string older =
                        kept ? remote::replaceCandidate(result.path,
                                                        result.filename, folder,
                                                        saved)
                             : "";
                    if (!older.empty() && askToReplace(result, older, sender)) {
                        saved.insert(text::lower(older));
                    }
                }
            }
            for (const QueueResult &result : results) {
                receiveLog_.linkEnded(result.url, rowState(result.status),
                                      result.name, result.path, result.bytes,
                                      result.message, clockNow());
                installed += result.status == HistoryStatus::Installed ? 1 : 0;
            }
            for (const QueueResult &result : results) {
                if (!extract || stopped || !ui::keepRunning()) {
                    break;
                }
                // A yes covers the links that looked like archives when it
                // was asked; any other archive is offered starting on Not Now.
                if (result.status == HistoryStatus::Saved && fromChecked(result) &&
                    filetype::fromName(basename(result.path)) ==
                        filetype::Kind::Archive) {
                    offerSentExtraction(
                        result,
                        agreed && filetype::fromName(nameOf(result.url)) ==
                                      filetype::Kind::Archive,
                        sender);
                }
            }
            // A file that came from somewhere the check did not find its link
            // to lead got none of what the sender asked for.
            const auto strayed = std::find_if(
                results.begin(), results.end(), [&](const QueueResult &result) {
                    return result.status == HistoryStatus::Saved &&
                           !fromChecked(result);
                });
            if ((!options.folder.empty() || options.extract) &&
                strayed != results.end() && ui::keepRunning()) {
                job.setState("choosing", "请在 3DS 上进行选择。", true);
                const auto found = leadsTo.find(strayed->url);
                ui::message("已保存到下载文件夹",
                            strayed->name + " 来自 " + strayed->host +
                                (found != leadsTo.end()
                                     ? "，而非 " + found->second + "."
                                     : "，且无法预先检查其链接。"),
                            ui::Tone::Warning, strayed->path);
            }
            // What a stop left behind is still in the Queue.
            receiveLog_.settleLinks(RowState::Queued, clockNow());
            linksIn += static_cast<uint32_t>(count);
            job.finish("已处理完 " + links + "（在 3DS 上）。", true, true);
        } else if (choice == Choice::Queue) {
            // The Queue keeps only the folder: Download All keeps both on a
            // clash, as it always has.
            turnedDown.erase(received.from);
            addToQueue(received.links, folder, false, !options.folder.empty());
            holdToChecked();
            receiveLog_.addLinks(received, RowState::Queued, clockNow());
            linksIn += static_cast<uint32_t>(count);
            job.finish("已添加 " + links + " 到 3DS 的下载队列中。", true, true);
        } else if (choice == Choice::DontAllow) {
            // Until Receive closes, its links and the file page are refused
            // before they reach the console.
            job.denyLinks(received.from);
            logging::info("receive.denied", {{"from", received.from}});
            job.finish("未下载：3DS 拒绝了此设备。", false, true);
        } else {
            if (!fromFbiTool) {
                turnedDown.insert(received.from);
            }
            job.finish("未下载：已在 3DS 上选择取消。", false, true);
        }
        net::finishReceived(received);
    };

    // The list takes what has happened each frame, and runs the questions
    // and the work senders ask for over itself.
    ui::ListSpec spec;
    spec.title = "接收";
    spec.emptyText = "尚未接收任何内容";
    uint32_t shown = 0;
    const auto fillRows = [&]() {
        spec.items.clear();
        for (const Row &row : receiveLog_.rows()) {
            spec.items.push_back(rowItem(row));
        }
        // Clear empties the list of what is done; the files stay.
        const bool clearable = receiveLog_.anyFinished();
        spec.action = clearable ? "清空" : "";
        spec.actionKey = "X";
        spec.actionKeys = clearable ? static_cast<u32>(KEY_X) : 0u;
        shown = receiveLog_.generation();
    };
    spec.update = [&]() {
        if (!nameSettled && job.named.load()) {
            hostname = job.mdns.hostname();
            named = "http://" + hostname + port;
            nameSettled = true;
        }
        holdAwake();
        takeReports();
        ReceiveJob::Asker asker;
        net::Received received;
        if (job.takeAsking(asker)) {
            askAbout(asker);
            ui::lockTouch();
        } else if (job.take(received)) {
            handle(received);
            ui::lockTouch();
        }
        if (receiveLog_.generation() == shown) {
            return false;
        }
        fillRows();
        return true;
    };
    spec.drawTop = [&](int) {
        const bool moving = !current.path.empty() &&
                            (!current.finished ||
                             (current.ok && clockNow() - heard < LingerMs));
        if (moving) {
            drawTransfer(current, meter, shownAddress);
        } else {
            drawConnect(code, shownAddress, named);
        }
    };

    int cursor = 0;
    while (ui::keepRunning() && !quit_) {
        fillRows();
        const ui::ListResult result = ui::runList(spec, cursor);
        if (result.key == KEY_X) {
            receiveLog_.clearFinished();
            cursor = 0;
            continue;
        }
        if (result.key != KEY_A || result.index < 0 ||
            result.index >= static_cast<int>(receiveLog_.rows().size())) {
            break;
        }
        // A copy: the list goes on changing while the row is open.
        receivedOptions(receiveLog_.rows()[result.index], shownAddress);
    }

    job.stop.store(true);
    if (!waitForThreads({thread, mdnsThread})) {
        owned.release(); // 3dsDL is closing; the threads still use it
        return;
    }
    threadFree(thread);
    if (mdnsThread != nullptr) {
        threadFree(mdnsThread);
    }
    // The last reports, such as an upload given up as the listener stopped.
    takeReports();
    receiveLog_.stopAll(clockNow());
    // Senders still waiting are told the links are done, so they exit.
    net::Received pending;
    while (job.take(pending)) {
        net::finishReceived(pending);
    }
    job.receiver.close();
    job.mdns.close();
    logging::info("receive.done", {{"files", filesIn},
                                   {"bytes", bytesIn},
                                   {"sent", filesOut},
                                   {"links", linksIn},
                                   {"installed", installed},
                                   {"failed", failed},
                                   {"fbi", fbiListening},
                                   {"name", hostname},
                                   {"ms", clockNow() - opened}});
}

void App::receivedOptions(net::ReceiveLog::Row row, const std::string &address)
{
    const bool there = !row.path.empty() && fileops::exists(row.path);
    const bool installable = there && row.kind != RowKind::Folder &&
                             row.state != RowState::Installed &&
                             filetype::fromName(row.path) == filetype::Kind::Cia &&
                             installer::available();
    enum class Option { Install, InstallAndDelete, Show };
    std::vector<Option> options;
    std::vector<ui::MenuItem> items;
    if (installable) {
        options = {Option::Install, Option::InstallAndDelete};
        items = {{"安装"}, {"安装后删除文件"}};
    }
    if (there) {
        options.push_back(Option::Show);
        items.push_back({"在“文件管理”中显示"});
    }
    const std::vector<ui::Fact> facts = rowFacts(row);
    const int choice = ui::menu(items, [&](int) {
        ui::drawTopChrome("接收", ui::Icon::Chip, ui::color::IconPink, address);
        ui::drawPanel();
        // The name beside its icon, then the fact rows (20 px, 26 apart),
        // centred in the panel as one block.
        constexpr float Header = 30 + 14;
        const float rows = facts.size() * 26.0f - 6;
        const float top = ui::TopPanel.y + (ui::TopPanel.h - Header - rows) / 2;
        const float left = ui::TopPanel.x + 18;
        const screens::KindLook look = lookOf(row);
        ui::drawIcon(look.icon, left, top, 30, look.color);
        ui::drawMarquee(row.name, left + 40, top + (30 - ui::lineHeight(0.62f)) / 2,
                        0.62f, ui::color::InkStrong, 300);
        ui::drawFacts(facts, {ui::TopPanel.x, top + Header, ui::TopPanel.w, rows});
    });
    if (choice < 0) {
        return;
    }
    if (options[choice] == Option::Show) {
        filesScreen(row.path);
        return;
    }
    cia::Info info;
    std::string error;
    if (!readCia(row.path, info, error)) {
        if (!error.empty()) {
            ui::message("无法安装", error, ui::Tone::Error, row.name);
        }
        return;
    }
    net::Received sender;
    sender.from = row.from;
    sender.device = row.device;
    if (installCia(row.path, info, options[choice] == Option::InstallAndDelete,
                   "")) {
        receiveLog_.installEnded(row.path, sender, true, "", clockNow());
    }
}

HistoryStatus App::installDirectly(const std::string &address, bool quiet,
                                   std::string *message)
{
    const auto failed = [message](const std::string &why) {
        if (message != nullptr) {
            *message = why;
        }
        return HistoryStatus::Failed;
    };
    const std::string folder = storage::DataFolder;
    if (!fs::makeDirectories(folder)) {
        if (!quiet) {
            ui::message("未安装", "无法创建：" + folder + ".",
                        ui::Tone::Error);
        }
        return failed("无法创建：" + folder + ".");
    }
    installer::StreamInstall sink;
    net::Request request;
    request.url = address;
    request.folder = folder;
    request.caFile = http::CaBundle;
    request.userAgent = http::UserAgent;
    request.allowResume = false;
    request.socketBufferSize = http::SocketBufferSize;
    request.connections = 1; // the install has to receive the data in order
    request.write = [&sink](const void *data, size_t size, FILE *file) {
        const off_t at = ftello(file);
        return at >= 0 && sink.write(static_cast<uint64_t>(at), data, size)
                   ? size
                   : static_cast<size_t>(0);
    };
    const net::Result result =
        tasks::download(request, "", true,
                        [&sink]() { return sink.started(); });
    if (!result.partPath.empty()) {
        fs::removeFile(result.partPath); // empty: the data went to the install
    }

    installer::Result installed;
    if (result.outcome == net::Outcome::Completed) {
        installed = sink.finish();
    } else {
        sink.cancel();
        installed = sink.result();
        if (installed.message.empty()) {
            installed.message = result.message;
        }
        if (result.outcome == net::Outcome::Cancelled) {
            installed.outcome = installer::Outcome::Cancelled;
        }
    }
    if (!ui::keepRunning()) {
        return HistoryStatus::Cancelled;
    }

    const cia::Info &info = sink.info();
    const std::string name = info.hasSmdh ? screens::ciaName(info)
                                          : nameOf(address);
    const auto recordResult = [&](HistoryStatus status,
                                  const std::string &message) {
        HistoryEntry entry = historyEntry(address, folder, status, message);
        entry.name = result.filename;
        entry.bytes = installed.bytes;
        record(entry);
    };
    if (installed.outcome == installer::Outcome::Cancelled) {
        recordResult(HistoryStatus::Cancelled, "直接安装已停止");
        if (!quiet) {
            ui::message("安装已停止", "未作任何更改。");
        }
        return HistoryStatus::Cancelled;
    }
    if (installed.outcome != installer::Outcome::Installed) {
        recordResult(HistoryStatus::Failed,
 "直接安装失败：" + installed.message);
 if (!quiet) {
 ui::message("安装失败", installed.message, ui::Tone::Error,
 name);
 }
 return failed(installed.message);
 }
 recordResult(HistoryStatus::Installed, "已直接安装，未保存文件");

 std::string seedNote;
 if (info.seed == cia::Seed::Needed) {
 tasks::busy("正在安装", "正在获取 " + name + " 所需的 seed……",
                    [&](tasks::Busy &busy) {
                        installer::installSeed(info.titleId, seedNote,
                                               &busy.cancel);
                    },
                    true);
    }
    if (quiet) {
        return HistoryStatus::Installed;
    }
    const uint64_t launchId = cia::baseTitle(info.titleId);
    const cia::Kind kind = cia::kind(launchId);
    const bool canLaunch = launchId == info.titleId &&
                           (kind == cia::Kind::Application ||
                            kind == cia::Kind::Demo);
    ui::Dialog dialog;
    dialog.header = "安装完成";
    dialog.title = name + " 已安装。";
    dialog.tone = ui::Tone::Success;
    dialog.icon = info.hasSmdh ? ui::showIcon(info.smdh.icon) : nullptr;
    dialog.body.push_back({name, ui::color::Text, 0.5f, 1});
    dialog.body.push_back({std::string(cia::kindName(cia::kind(info.titleId))) +
                               ", " + screens::titleVersion(info.titleId, info.version) + ", " +
                               units::bytes(installed.bytes),
                           ui::color::Muted, 0.46f, 3});
    if (!seedNote.empty()) {
        dialog.body.push_back({seedNote, ui::color::Muted, 0.44f, 2});
    }
    dialog.buttons = canLaunch ? std::vector<std::string>{"关闭", "启动"}
                               : std::vector<std::string>{"关闭"};
    dialog.initial = canLaunch ? 1 : 0;
    dialog.cancelButton = 0;
    if (ui::choose(dialog) == 1 && canLaunch) {
        std::string error;
        if (!installer::launch(launchId, error)) {
            ui::message("无法启动", error, ui::Tone::Error);
        }
    }
    return HistoryStatus::Installed;
}
