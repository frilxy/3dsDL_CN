#include "app.hpp"

#include "app_util.hpp"
#include "fs_util.hpp"
#include "installer.hpp"
#include "library.hpp"
#include "saves.hpp"
#include "storage.hpp"
#include "tasks.hpp"
#include "ui.hpp"
#include "text.hpp"
#include "units.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

// Titles: installed software, updates and DLC, extra data, tickets, seeds
// and cleaning up after failed installs, in the spirit of System Settings,
// Data Management.

namespace {

using screens::plural;

// Where a seeddb.bin is usually left: by hand, by GodMode9, or for 3dsDL.
const char *const SeedDbPlaces[] = {
    "sdmc:/seeddb.bin",
    "sdmc:/3ds/seeddb.bin",
    "sdmc:/3ds/3dsDL/seeddb.bin",
    "sdmc:/gm9/out/seeddb.bin",
};

const char *placeName(titles::Place place)
{
    switch (place) {
    case titles::Place::SystemMemory:
        return "系统存储";
    case titles::Place::GameCard:
        return "游戏卡带";
    case titles::Place::SdCard:
        break;
    }
    return "SD 卡";
}

ui::Icon iconFor(uint64_t titleId)
{
    switch (cia::kind(titleId)) {
    case cia::Kind::Update:
        return ui::Icon::Download;
    case cia::Kind::Dlc:
        return ui::Icon::Package;
    case cia::Kind::DsiWare:
        return ui::Icon::App;
    default:
        return ui::Icon::GameCard;
    }
}

// The title's icon, name and publisher over its Title ID, version and size;
// order is how the list it is in is sorted, if it is in one.
void drawTitleTop(const std::string &header, ui::Icon headerIcon,
                  const titles::Title &title, const titles::Index &index,
                  const std::string &order = "")
{
    ui::drawTopChrome(header, headerIcon, ui::color::IconBlue, order);
    ui::drawPanel();
    const float left = ui::TopPanel.x + 18;
    const float top = ui::TopPanel.y + 12;
    const titles::Title *game = &title;
    if (title.icon.empty()) {
        const auto found = index.find(cia::baseTitle(title.id));
        if (found != index.end()) {
            game = found->second;
        }
    }
    ui::drawTitleIcon(game->icon, left, top, iconFor(title.id));
    ui::drawMarquee(titles::displayName(title, index), left + 60, top + 2,
                    0.62f, ui::color::InkStrong, 280);
    std::string second = game->publisher.empty()
                             ? cia::kindName(cia::kind(title.id))
                             : game->publisher;
    if (!title.productCode.empty()) {
        second += "   " + title.productCode;
    }
    ui::drawMarquee(second, left + 60, top + 30, ui::type::Small,
                    ui::color::InkSoft, 280);
    float y = top + 60;
    y = ui::drawTagRow(left, y, 76, "软件 ID", cia::titleIdText(title.id));
    y = ui::drawTagRow(left, y, 76, "版本", screens::titleVersion(title.id, title.version));
    ui::drawTagRow(left, y, 76, "大小",
                   units::bytes(title.size) + ", " + placeName(title.place));
}

// No body or icon, so the top screen keeps showing what is being deleted.
bool confirmDanger(const std::string &header, const std::string &title,
                   const std::string &button)
{
    ui::Dialog dialog;
    dialog.header = header;
    dialog.title = title;
    dialog.tone = ui::Tone::Warning;
    dialog.buttons = {button, "取消"};
    dialog.dangerous = 0;
    dialog.initial = 1;
    dialog.cancelButton = 1;
    return ui::choose(dialog) == 0;
}

const library::ExtData *extDataFor(const titles::Title &title,
                                   const library::Snapshot &snapshot)
{
    for (const library::ExtData &extData : snapshot.extData) {
        if (extData.id == titles::uniqueId(title.id)) {
            return &extData;
        }
    }
    return nullptr;
}

} // namespace

void App::startLibrary()
{
    libraryLoader_.stop();
    library_ = library::Snapshot();
    libraryLoader_.start();
    ++libraryGeneration_;
}

void App::pollLibrary()
{
    if (libraryLoader_.take(library_)) {
        ++libraryGeneration_;
        // Kept for when the software is gone and only a ticket or extra
        // data is left to name.
        if (library_.complete && titles::rememberNames(knownNames_, library_.titles) &&
            storage::saveNames(knownNames_)) {
            sdChanged(storage::DataFolder);
        }
    }
}

bool App::reloadLibrary()
{
    startLibrary();
    std::string error;
    while (ui::keepRunning() && !library_.complete &&
           !libraryLoader_.failed(error)) {
        ui::readInput();
        ui::BusyView view;
        view.header = "软件管理";
        view.title = "正在读取已安装的软件……";
        const uint64_t total = libraryLoader_.total();
        if (total > 0) {
            view.progress = true;
            view.fraction = static_cast<float>(libraryLoader_.done()) / total;
            view.amount = std::to_string(libraryLoader_.done()) + " / " +
                          std::to_string(total);
        }
        ui::drawBusy(view); // its frame hook takes what arrived
        gspWaitForVBlank();
    }
    if (!library_.complete && libraryLoader_.failed(error)) {
        ui::message("软件列表不可用", error, ui::Tone::Error);
    }
    return library_.complete;
}

void App::titlesScreen()
{
    // Opens at once while the titles are read in the background; the title
    // lists and Save Data fill in as they arrive. Everything that needs the
    // whole picture (tickets, seeds, extra data, clean up) waits for it.
    startLibrary();
    struct StopReading {
        library::Loader &loader;
        ~StopReading() { loader.stop(); }
    } stopReading {libraryLoader_};

    int focus = 3;
    while (ui::keepRunning() && !quit_) {
        size_t software = 0;
        size_t addOns = 0;
        uint64_t sdBytes = 0;
        size_t unused = 0;
        size_t cleanUp = 0;
        std::string error;
        bool failed = false;
        const auto count = [&]() {
            software = addOns = 0;
            sdBytes = 0;
            for (const titles::Title &title : library_.titles) {
                (titles::isSoftware(title.id) ? software : addOns) += 1;
                if (title.place == titles::Place::SdCard) {
                    sdBytes += title.size;
                }
            }
            unused = library::unusedTickets(library_).size();
            cleanUp = unused + library_.pending.size();
            failed = libraryLoader_.failed(error);
        };
        count();

        ui::ScreenSpec spec;
        spec.back = "返回";
        const auto add = [&](ui::Rect rect, const char *label, ui::Icon icon,
                             u32 iconColor, bool tile) {
            ui::Control control;
            control.rect = rect;
            control.label = label;
            control.icon = icon;
            control.iconColor = iconColor;
            control.tile = tile;
            control.scale = ui::type::Small + 0.06f;
            spec.controls.push_back(control);
        };
        // Three narrow bars: words only, so they fit without scrolling.
        add({16, 6, 92, 26}, "清理", ui::Icon::None, 0, false);
        spec.controls.back().scale = ui::type::Small;
        add({114, 6, 92, 26}, "票据", ui::Icon::None, 0, false);
        spec.controls.back().scale = ui::type::Small;
        add({212, 6, 92, 26}, "种子", ui::Icon::None, 0, false);
        spec.controls.back().scale = ui::type::Small;
        add({16, 42, 140, 78}, "软件", ui::Icon::GameCard,
            ui::color::IconBlue, true);
        add({164, 42, 140, 78}, "更新与 DLC", ui::Icon::Download,
            ui::color::IconGreen, true);
        add({16, 127, 140, 76}, "存档", ui::Icon::Chip,
            ui::color::IconTeal, true);
        add({164, 127, 140, 76}, "额外数据", ui::Icon::Folder,
            ui::color::IconOrange, true);
        // The title lists and Save Data fill in while the titles are read;
        // the rest needs all of them first.
        uint32_t seen = libraryGeneration_ - 1;
        spec.update = [&]() {
            if (seen == libraryGeneration_) {
                return false;
            }
            seen = libraryGeneration_;
            count();
            for (const int needsAll : {0, 1, 2, 6}) {
                spec.controls[needsAll].enabled = library_.complete;
            }
            spec.controls[0].badge =
                library_.complete && cleanUp > 0 ? std::to_string(cleanUp) : "";
            return true;
        };
        spec.update();
        spec.drawTop = [&]() {
            ui::drawTopChrome("软件管理", ui::Icon::GameCard, ui::color::IconBlue);
            ui::drawPanel();
            std::vector<ui::Fact> facts;
            if (failed) {
                facts.push_back({"无法读取", error, ui::color::Bad});
            } else if (!library_.complete) {
                facts.push_back({"正在读取",
                                 std::to_string(libraryLoader_.done()) + " / " +
                                     std::to_string(libraryLoader_.total()),
                                 ui::color::Badge});
            }
            switch (focus) {
            case 0:
                facts.push_back({"未完成",
                                 plural(library_.pending.size(), "install")});
                facts.push_back({"未使用", plural(unused, "ticket")});
                break;
            case 1:
                facts.push_back(
                    {"票据", std::to_string(library_.tickets.size())});
                facts.push_back({"未使用", std::to_string(unused)});
                break;
            case 2:
                facts.push_back({"查找位置", "sdmc:/seeddb.bin"});
                break;
            case 4:
                facts.push_back({"已安装", plural(addOns, "item")});
                break;
            case 5:
                facts.push_back({"备份位置", "sdmc:/3ds/Checkpoint/saves/"});
                break;
            case 6:
                facts.push_back({"位于 SD 卡",
                                 plural(library_.extData.size(), "item")});
                facts.push_back(
                    {"备份位置", "sdmc:/3ds/Checkpoint/extdata/"});
                break;
            default:
                facts.push_back({"已安装", plural(software, "title")});
                facts.push_back(
                    {"位于 SD 卡",
                     !library_.complete || library_.sdListed
                         ? units::bytes(sdBytes)
                         : "无法读取",
                     !library_.complete || library_.sdListed
                         ? ui::color::Tag
                         : ui::color::Badge});
                break;
            }
            ui::drawFacts(facts);
        };
        const ui::ScreenResult result = ui::runScreen(spec, focus);
        switch (result.choice) {
        case 0:
            cleanUpScreen();
            break;
        case 1:
            ticketsScreen();
            break;
        case 2:
            importSeeds();
            break;
        case 3:
            titleList(true);
            break;
        case 4:
            titleList(false);
            break;
        case 5:
            saveTitlesScreen();
            break;
        case 6:
            extraDataScreen();
            break;
        default:
            return;
        }
    }
}

namespace {

struct ShowChoice {
    titles::Show show;
    const char *label; // on the sort screen
    const char *title; // the list's, while it shows only these
    const char *empty;
};

const ShowChoice SoftwareShows[] = {
    {titles::Show::All, "全部软件", "软件", "未安装软件。"},
    {titles::Show::SdCard, "SD 卡", "SD 卡上的软件",
     "SD 卡上没有软件。"},
    {titles::Show::SystemMemory, "系统内存", "系统内存中的软件",
     "系统内存中没有软件。"},
};
const ShowChoice AddOnShows[] = {
    {titles::Show::All, "更新与 DLC", "更新与 DLC",
     "未安装更新或 DLC。"},
    {titles::Show::Updates, "更新", "更新", "未安装更新。"},
    {titles::Show::Dlc, "DLC", "DLC", "未安装 DLC。"},
};

const ShowChoice &showChoice(bool software, titles::Show show)
{
    const ShowChoice *choices = software ? SoftwareShows : AddOnShows;
    for (int index = 0; index < 3; ++index) {
        if (choices[index].show == show) {
            return choices[index];
        }
    }
    return choices[0];
}

} // namespace

void App::titleList(bool software)
{
    int cursor = 0;
    uint64_t follow = 0; // a title to land on after the order changes
    while (ui::keepRunning() && !quit_) {
        // Rebuilt whenever more titles arrive, so the list fills in while
        // it is open; rows are keyed by Title ID, so the highlight stays.
        titles::Index index;
        std::vector<const titles::Title *> shown;
        const titles::Show show = software ? softwareShow_ : addOnShow_;
        ui::ListSpec spec;
        spec.title = showChoice(software, show).title;
        spec.emptyText = showChoice(software, show).empty;
        spec.action = "排序";
        spec.actionKey = "X";
        spec.actionKeys = KEY_X;
        uint32_t seen = libraryGeneration_ - 1;
        spec.update = [&]() {
            if (seen == libraryGeneration_) {
                return false;
            }
            seen = libraryGeneration_;
            index = titles::indexOf(library_.titles);
            shown.clear();
            for (const titles::Title &title : library_.titles) {
                if (titles::isSoftware(title.id) == software &&
                    titles::shows(show, title)) {
                    shown.push_back(&title);
                }
            }
            titles::sortTitles(shown, index, settings_.titleSort,
                               settings_.titleSortReverse);
            spec.items.clear();
            for (const titles::Title *title : shown) {
                ui::ListItem item;
                item.key = cia::titleIdText(title->id);
                item.label = titles::displayName(*title, index);
                item.icon = iconFor(title->id);
                item.iconColor =
                    software ? ui::color::IconBlue : ui::color::IconGreen;
                const cia::Kind kind = cia::kind(title->id);
                if (kind == cia::Kind::Update || kind == cia::Kind::Dlc) {
                    item.badge = kind == cia::Kind::Update ? "更新" : "DLC";
                    item.badgeColor = kind == cia::Kind::Update
                                          ? ui::color::Tag
                                          : ui::color::Good;
                } else {
                    item.detail = units::bytes(title->size);
                }
                spec.items.push_back(item);
            }
            return true;
        };
        spec.update();
        for (size_t row = 0; follow != 0 && row < shown.size(); ++row) {
            if (shown[row]->id == follow) {
                cursor = static_cast<int>(row);
            }
        }
        follow = 0;
        spec.loading = [&]() {
            std::string ignored;
            return !library_.complete && !libraryLoader_.failed(ignored);
        };
        spec.drawTop = [&](int highlighted) {
            const std::string order = screens::sortDescription(
                settings_.titleSort, settings_.titleSortReverse);
            if (highlighted < 0 || highlighted >= static_cast<int>(shown.size())) {
                ui::drawTopChrome(spec.title, ui::Icon::GameCard,
                                  ui::color::IconBlue, order);
                ui::drawPanel();
                ui::drawEmblem(ui::Icon::GameCard, ui::color::IconBlue);
                return;
            }
            drawTitleTop(spec.title, software ? ui::Icon::GameCard
                                              : ui::Icon::Download,
                         *shown[highlighted], index, order);
        };
        const ui::ListResult result = ui::runList(spec, cursor);
        const bool onRow =
            result.index >= 0 && result.index < static_cast<int>(shown.size());
        if (result.key == KEY_X) {
            follow = onRow ? shown[result.index]->id : 0;
            cursor = 0;
            titleSortScreen(software);
            continue;
        }
        if (result.key != KEY_A || !onRow) {
            return;
        }
        titleOptions(*shown[result.index]);
    }
}

void App::titleSortScreen(bool software)
{
    titles::Show &show = software ? softwareShow_ : addOnShow_;
    const ShowChoice *shows = software ? SoftwareShows : AddOnShows;
    int focus = settings_.titleSort == fs::SortOrder::Size ? 1 : 0;
    // Picks apply at once and the screen stays, like the files' Sort.
    while (ui::keepRunning() && !quit_) {
        ui::ScreenSpec spec;
        spec.back = "返回";
        const fs::SortOrder orders[] = {fs::SortOrder::Name, fs::SortOrder::Size};
        for (int index = 0; index < 2; ++index) {
            ui::Control control;
            control.rect = {index == 0 ? 26.0f : 164.0f, 29, 130, 40};
            control.label = index == 0 ? "名称" : "大小";
            control.style = settings_.titleSort == orders[index]
                                ? ui::Style::Chosen
                                : ui::Style::Normal;
            spec.controls.push_back(control);
        }
        ui::Control reverse;
        reverse.rect = {26, 81, 268, 40};
        reverse.label = "反向排序";
        reverse.style = settings_.titleSortReverse ? ui::Style::Chosen
                                                   : ui::Style::Normal;
        spec.controls.push_back(reverse);
        for (int index = 0; index < 3; ++index) {
            ui::Control control;
            control.rect = {26.0f + index * 92.0f, 139, 84, 40};
            control.label = shows[index].label;
            control.style =
                shows[index].show == show ? ui::Style::Chosen : ui::Style::Normal;
            spec.controls.push_back(control);
        }
        spec.drawTop = [&]() {
            ui::drawTopChrome("排序", software ? ui::Icon::GameCard
                                               : ui::Icon::Download,
                              software ? ui::color::IconBlue : ui::color::IconGreen);
            ui::drawPanel();
            ui::drawFacts(
                {{"排序", screens::sortDescription(settings_.titleSort,
                                           settings_.titleSortReverse)},
                 {"正在显示", showChoice(software, show).label}});
        };
        const ui::ScreenResult result = ui::runScreen(spec, focus);
        if (result.choice == 0 || result.choice == 1) {
            settings_.titleSort = orders[result.choice];
            persistSettings();
        } else if (result.choice == 2) {
            settings_.titleSortReverse = !settings_.titleSortReverse;
            persistSettings();
        } else if (result.choice >= 3 && result.choice < 6) {
            show = shows[result.choice - 3].show;
        } else {
            return;
        }
    }
}

void App::titleOptions(titles::Title title)
{
    const titles::Index index = titles::indexOf(library_.titles);
    const std::string name = titles::displayName(title, index);
    const cia::Kind kind = cia::kind(title.id);
    const bool launchable = kind == cia::Kind::Application ||
                            kind == cia::Kind::Demo ||
                            kind == cia::Kind::DsiWare;
    const library::ExtData *extData = extDataFor(title, library_);
    const bool self = title.id == library::runningTitle();

    enum Choice { Launch, Saves, Extra, Seed, ExtraData, Delete, DeleteWithTicket };
    std::vector<Choice> choices;
    std::vector<ui::MenuItem> items;
    const auto offer = [&](Choice choice, const std::string &label,
                           bool danger = false, bool enabled = true) {
        choices.push_back(choice);
        items.push_back({label, danger, enabled});
    };
    if (launchable) {
        offer(Launch, "启动", false, !self);
    }
    const savedata::Target saveTarget =
        savedata::saveOf(title, library::mediaOf(title));
    const bool canSave = titles::isSoftware(title.id) &&
                         savedata::unsupportedReason(title).empty() &&
                         savedata::exists(saveTarget);
    if (canSave) {
        offer(Saves, "存档");
    }
    if (extData != nullptr) {
        offer(Extra, "额外数据");
    }
    if (kind == cia::Kind::Application) {
        offer(Seed, "获取种子");
    }
    if (extData != nullptr) {
        offer(ExtraData, "删除额外数据", true);
    }
    const bool removable = !self && title.place != titles::Place::GameCard;
    offer(Delete, "删除", true, removable);
    offer(DeleteWithTicket, "连同票据删除", true, removable);

    const int picked = ui::menu(items, [&](int) {
        drawTitleTop(name, iconFor(title.id), title, index);
    });
    if (picked < 0) {
        return;
    }

    std::string error;
    switch (choices[picked]) {
    case Launch:
        if (!installer::launch(title.id, error)) {
            ui::message("无法启动", error, ui::Tone::Error);
        }
        return;
    case Saves:
        saveDataScreen(saveTarget, title.icon);
        return;
    case Extra:
        saveDataScreen(savedata::extDataOf(title), title.icon);
        return;
    case Seed: {
        std::string message;
        bool added = false;
        if (!tasks::busy("种子", "正在获取 " + name + " 所需的种子……",
                         [&](tasks::Busy &busy) {
                             added = installer::installSeed(title.id, message,
                                                            &busy.cancel);
                         },
                         true)) {
            return;
        }
        ui::message(added ? "已添加种子" : "未添加种子", message,
                    added ? ui::Tone::Success : ui::Tone::Warning);
        return;
    }
    case ExtraData:
        if (confirmDanger("删除", "删除“" + name + "”的额外数据？",
                          "删除")) {
            // The reader stops first, so nothing is read while it changes;
            // the list it came from fills in again by itself.
            libraryLoader_.stop();
            if (!library::deleteExtData(extData->id, error)) {
                ui::message("未删除", error, ui::Tone::Error);
            }
            startLibrary();
        }
        return;
    case Delete:
    case DeleteWithTicket: {
        const bool withTicket = choices[picked] == DeleteWithTicket;
        // An update or DLC goes by its game's name, so the question says
        // which; a game takes its save with it.
        std::string question;
        if (kind == cia::Kind::Update || kind == cia::Kind::Dlc) {
            question = std::string("删除此") +
                       (kind == cia::Kind::Update ? "更新" : "追加内容") +
                       (withTicket ? "“" + name + "”及其票据？"
                                   : "“" + name + "”？");
        } else {
            question = "删除“" + name + "”" +
                       (withTicket ? "、其存档及票据？" : "及其存档？");
        }
        if (!confirmDanger("删除", question, "删除")) {
            return;
        }
        bool deleted = false;
        libraryLoader_.stop();
        tasks::busy("删除", "正在删除 " + name + "……", [&]() {
            deleted = library::deleteTitle(title, withTicket, error);
        });
        if (!deleted) {
            ui::message("未删除", error, ui::Tone::Error);
        }
        if (deleted && library_.complete) {
            library::forgetTitle(library_, title, withTicket);
            ++libraryGeneration_;
        } else {
            startLibrary();
        }
        return;
    }
    }
}

void App::extraDataScreen()
{
    int cursor = 0;
    while (ui::keepRunning() && !quit_) {
        const titles::Index index = titles::indexOf(library_.titles);
        const auto nameOf = [&](const library::ExtData &extData) {
            if (!extData.name.empty()) {
                return extData.name;
            }
            const titles::Title *owner =
                titles::ownerOfExtData(extData.id, library_.titles);
            if (owner != nullptr) {
                return titles::displayName(*owner, index);
            }
            char text[24];
            std::snprintf(text, sizeof(text), "额外数据 %08llX",
                          static_cast<unsigned long long>(extData.id));
            return std::string(text);
        };
        std::vector<const library::ExtData *> shown;
        for (const library::ExtData &extData : library_.extData) {
            shown.push_back(&extData);
        }
        std::stable_sort(shown.begin(), shown.end(),
                         [&](const library::ExtData *a, const library::ExtData *b) {
                             return text::lower(nameOf(*a)) < text::lower(nameOf(*b));
                         });

        ui::ListSpec spec;
        spec.title = "额外数据";
        spec.emptyText = "没有额外数据。";
        for (const library::ExtData *extData : shown) {
            ui::ListItem item;
            item.label = nameOf(*extData);
            item.icon = ui::Icon::Folder;
            item.iconColor = ui::color::IconOrange;
            if (!library::extDataInUse(library_, extData->id)) {
                item.badge = "未使用";
                item.badgeColor = ui::color::Neutral;
            }
            spec.items.push_back(item);
        }
        // The list and the menu for one item show the same facts.
        const auto drawDetails = [&](const library::ExtData &extData) {
            ui::drawTopChrome("额外数据", ui::Icon::Folder,
                              ui::color::IconOrange);
            ui::drawPanel();
            const titles::Title *owner =
                titles::ownerOfExtData(extData.id, library_.titles);
            const float left = ui::TopPanel.x + 18;
            const float top = ui::TopPanel.y + 12;
            ui::drawTitleIcon(!extData.icon.empty() || owner == nullptr
                                  ? extData.icon
                                  : owner->icon,
                              left, top, ui::Icon::Folder);
            ui::drawMarquee(nameOf(extData), left + 60, top + 2, 0.62f,
                            ui::color::InkStrong, 280);
            ui::drawMarquee(extData.publisher, left + 60, top + 30,
                            ui::type::Small, ui::color::InkSoft, 280);
            char id[16];
            std::snprintf(id, sizeof(id), "%08llX",
                          static_cast<unsigned long long>(extData.id));
            const float y = ui::drawTagRow(left, top + 60, 96, "ID", id);
            const bool inUse = library::extDataInUse(library_, extData.id);
            ui::drawTagRow(left, y, 96, "软件",
                           inUse ? "已安装" : "未安装",
                           inUse ? ui::color::Tag : ui::color::Neutral);
        };
        spec.drawTop = [&](int highlighted) {
            if (highlighted < 0 || highlighted >= static_cast<int>(shown.size())) {
                ui::drawTopChrome("额外数据", ui::Icon::Folder,
                                  ui::color::IconOrange);
                ui::drawPanel();
                ui::drawEmblem(ui::Icon::Folder, ui::color::IconOrange);
                return;
            }
            drawDetails(*shown[highlighted]);
        };
        if (!shown.empty()) {
            spec.action = "删除";
            spec.actionKey = "X";
            spec.actionKeys = KEY_X;
        }
        const ui::ListResult result = ui::runList(spec, cursor);
        if ((result.key != KEY_A && result.key != KEY_X) || result.index < 0 ||
            result.index >= static_cast<int>(shown.size())) {
            return;
        }
        const library::ExtData extData = *shown[result.index];
        const std::string name = nameOf(extData);
        if (result.key == KEY_A) {
            const int choice = ui::menu(
                {{"备份与恢复", false, true}, {"删除", true, true}},
                [&](int) { drawDetails(extData); });
            if (choice == 0) {
                // Named after the game when it is installed, as Checkpoint
                // names it; otherwise after the extra data's own icon.
                const titles::Title *owner =
                    titles::ownerOfExtData(extData.id, library_.titles);
                saveDataScreen(owner != nullptr
                                   ? savedata::extDataOf(*owner)
                                   : savedata::extDataOf(extData.id,
                                                         extData.englishShortName,
                                                         name),
                               extData.icon.empty() && owner != nullptr
                                   ? owner->icon
                                   : extData.icon);
                continue;
            }
            if (choice != 1) {
                continue;
            }
        }
        if (!confirmDanger("删除", "Delete the extra data of " + name + "?",
                           "删除")) {
            continue;
        }
        std::string error;
        if (!library::deleteExtData(extData.id, error)) {
            ui::message("未删除", error, ui::Tone::Error);
        }
        reloadLibrary();
    }
}

namespace {

// Checkpoint's backup folder names, which name a title by its unique ID.
std::vector<std::string> checkpointFolders()
{
    std::vector<std::string> folders;
    for (const char *root : {saves::SavesRoot, saves::ExtDataRoot}) {
        for (const fs::Entry &entry : fs::listEntries(root, nullptr, true, false)) {
            if (entry.folder) {
                folders.push_back(entry.name);
            }
        }
    }
    return folders;
}

// A CIA on the SD card that would bring a title's ticket back, where its
// files are usually kept: its path and the name inside it.
struct FoundCia {
    std::string path;
    std::string name;
};

std::map<uint64_t, FoundCia> ciasOnCard(const std::string &downloadFolder)
{
    std::map<uint64_t, FoundCia> found;
    std::set<std::string> seen;
    for (const std::string &folder : {std::string("sdmc:/cias/"), downloadFolder}) {
        if (!seen.insert(text::lower(folder)).second) {
            continue;
        }
        for (const fs::Entry &entry : fs::listEntries(folder, nullptr, false, false)) {
            if (entry.folder || !text::endsWithNoCase(entry.name, ".cia")) {
                continue;
            }
            cia::Info info;
            std::string error;
            const std::string path = folder + entry.name;
            if (cia::readInfo(path, info, error, true) && found.count(info.titleId) == 0) {
                found[info.titleId] = {path, info.smdh.shortName};
            }
        }
    }
    return found;
}

} // namespace

std::string App::softwareName(uint64_t titleId, const titles::Index &index,
                              const std::vector<std::string> &folders) const
{
    const std::string shown = titles::displayName(titleId, index);
    if (shown != cia::titleIdText(titleId)) {
        return shown;
    }
    const std::string remembered = titles::rememberedName(titleId, knownNames_);
    if (!remembered.empty()) {
        return remembered;
    }
    for (const std::string &folder : folders) {
        const std::string name = saves::nameFromFolder(cia::baseTitle(titleId), folder);
        if (!name.empty()) {
            return name;
        }
    }
    return "";
}

bool App::confirmUnsavedTickets(const std::vector<uint64_t> &tickets,
                                const std::string &why)
{
    struct Known {
        uint64_t id = 0;
        std::string name;
        library::TicketFacts facts;
        std::string cia; // a CIA that brings it back, or ""
    };
    std::vector<Known> known;
    tasks::busy("票据", "正在检查票据……", [&]() {
        const titles::Index index = titles::indexOf(library_.titles);
        const std::vector<std::string> folders = checkpointFolders();
        const std::map<uint64_t, FoundCia> cias = ciasOnCard(settings_.destination);
        for (const uint64_t id : tickets) {
            Known ticket;
            ticket.id = id;
            ticket.name = softwareName(id, index, folders);
            ticket.facts = library::ticketFacts(id);
            const auto cia = cias.find(id);
            if (cia != cias.end()) {
                ticket.cia = cia->second.path;
                if (ticket.name.empty()) {
                    ticket.name = cia->second.name;
                }
            }
            known.push_back(ticket);
        }
    });
    size_t bought = 0;
    for (const Known &ticket : known) {
        bought += ticket.facts.personal ? 1 : 0;
    }
    const auto nameOf = [](const Known &ticket) {
        return ticket.name.empty() ? cia::titleIdText(ticket.id) : ticket.name;
    };
    // The console's own reason, without the sentence around it.
    const size_t code = why.find("Error ");
    const std::string reason = code == std::string::npos ? why : why.substr(code);

    ui::Dialog dialog;
    dialog.header = "票据";
    dialog.title =
        known.size() == 1
            ? "删除“" + nameOf(known[0]) + "”的票据且不备份？"
            : "删除 " + plural(known.size(), "票据") + "且不备份？" +
                  (bought > 0 ? "其中 " + std::to_string(bought) +
                                    " 张是在此 3DS 上购买的。"
                              : "");
    dialog.tone = ui::Tone::Warning;
    dialog.buttons = {"仍要删除", "保留"};
    dialog.dangerous = 0;
    dialog.initial = 1;
    dialog.cancelButton = 1;
    dialog.drawTop = [&]() {
        ui::drawTopChrome("票据", ui::Icon::Lock, ui::color::IconGrey);
        ui::drawPanel();
        std::vector<ui::Fact> facts;
        if (known.size() == 1) {
            const Known &ticket = known[0];
            facts.push_back({"软件", ticket.name.empty() ? "未知" : ticket.name});
            facts.push_back({"软件 ID", cia::titleIdText(ticket.id)});
            facts.push_back({"在此主机购买", ticket.facts.personal ? "是" : "否",
                             ticket.facts.personal ? ui::color::Bad : ui::color::Tag});
            if (ticket.facts.known) {
                facts.push_back({"适用版本", screens::titleVersion(ticket.id,
                                                                     ticket.facts.version)});
            }
            facts.push_back({"恢复", ticket.cia.empty() ? "未找到对应的 CIA"
                                                           : "安装 " + ticket.cia});
            facts.push_back({"备份", "无法备份：" + reason, ui::color::Neutral});
        } else {
            for (size_t index = 0; index < known.size() && index < 5; ++index) {
                const Known &ticket = known[index];
                facts.push_back({ticket.facts.personal ? "在此主机购买"
                                 : ticket.cia.empty()  ? "无 CIA"
                                                       : "已找到 CIA",
                                 nameOf(ticket),
                                 ticket.facts.personal ? ui::color::Bad : ui::color::Tag});
            }
            if (known.size() > 5) {
                facts.push_back({"另有", std::to_string(known.size() - 5) + " 个",
                                 ui::color::Neutral});
            }
        }
        ui::drawFacts(facts);
    };
    return ui::choose(dialog) == 0;
}

void App::ticketsScreen()
{
    int cursor = 0;
    const std::vector<std::string> folders = checkpointFolders();
    while (ui::keepRunning() && !quit_) {
        const titles::Index index = titles::indexOf(library_.titles);
        // The name of software that is gone, when 3dsDL can tell.
        const auto nameFor = [&](uint64_t ticket) {
            const std::string name = softwareName(ticket, index, folders);
            return name.empty() ? cia::titleIdText(ticket) : name;
        };
        const std::vector<uint64_t> unusedList =
            library::unusedTickets(library_);
        const std::set<uint64_t> unused(unusedList.begin(), unusedList.end());
        std::vector<uint64_t> shown = library_.tickets;
        std::stable_sort(shown.begin(), shown.end(), [&](uint64_t a, uint64_t b) {
            const std::string left = text::lower(nameFor(a));
            const std::string right = text::lower(nameFor(b));
            return left != right ? left < right : a < b;
        });

        ui::ListSpec spec;
        spec.title = "票据";
        spec.emptyText = "没有票据。";
        for (const uint64_t ticket : shown) {
            ui::ListItem item;
            item.label = nameFor(ticket);
            item.icon = ui::Icon::Lock;
            item.iconColor = ui::color::IconGrey;
            if (unused.count(ticket) != 0) {
                item.badge = "未使用";
                item.badgeColor = ui::color::Neutral;
            } else {
                item.detail = cia::kindName(cia::kind(ticket));
            }
            spec.items.push_back(item);
        }
        // The list and the menu for one ticket show the same facts.
        const auto drawDetails = [&](uint64_t ticket) {
            ui::drawTopChrome("票据", ui::Icon::Lock, ui::color::IconGrey);
            ui::drawPanel();
            const auto own = index.find(ticket);
            const auto game = index.find(cia::baseTitle(ticket));
            const std::vector<uint16_t> noIcon;
            const std::vector<uint16_t> &icon =
                own != index.end() && !own->second->icon.empty() ? own->second->icon
                : game != index.end() ? game->second->icon
                                      : noIcon;
            const float left = ui::TopPanel.x + 18;
            const float top = ui::TopPanel.y + 12;
            ui::drawTitleIcon(icon, left, top, ui::Icon::Lock);
            // The name alone, level with the icon's middle; the list says
            // the kind.
            ui::drawMarquee(nameFor(ticket), left + 60,
                            top + (48 - ui::lineHeight(0.62f)) / 2, 0.62f,
                            ui::color::InkStrong, 280);
            const float y = ui::drawTagRow(left, top + 60, 96, "软件 ID",
                                           cia::titleIdText(ticket));
            const bool isUnused = unused.count(ticket) != 0;
            // Without the SD card's list, whether it is installed is unknown.
            ui::drawTagRow(left, y, 96, "软件",
                           !library_.sdListed ? "未知"
                           : isUnused         ? "未安装"
                                              : "已安装",
                           isUnused ? ui::color::Neutral : ui::color::Tag);
        };
        spec.drawTop = [&](int highlighted) {
            if (highlighted < 0 || highlighted >= static_cast<int>(shown.size())) {
                ui::drawTopChrome("票据", ui::Icon::Lock, ui::color::IconGrey);
                ui::drawPanel();
                ui::drawEmblem(ui::Icon::Lock, ui::color::IconGrey);
                return;
            }
            drawDetails(shown[highlighted]);
        };
        if (!unusedList.empty()) {
            spec.action = "删除未使用项";
            spec.actionKey = "X";
            spec.actionKeys = KEY_X;
        }
        const ui::ListResult result = ui::runList(spec, cursor);
        if (result.key == KEY_X && !unusedList.empty()) {
            deleteUnusedTickets();
            continue;
        }
        if (result.key != KEY_A || result.index < 0 ||
            result.index >= static_cast<int>(shown.size())) {
            return;
        }
        const uint64_t ticket = shown[result.index];
        const std::string name = nameFor(ticket);
        const int choice = ui::menu(
            {{"导出到 SD 卡", false, true}, {"删除", true, true}},
            [&](int) { drawDetails(ticket); });
        if (choice == 0) {
            std::string path;
            std::string error;
            bool exported = false;
            tasks::busy("票据", "正在导出票据……", [&]() {
                exported = library::exportTicket(
                    ticket, "sdmc:/3ds/3dsDL/tickets/", path, error);
            });
            if (exported) {
                ui::message("票据已导出", "已保存为 " + path + "。",
                            ui::Tone::Success);
            } else {
                ui::message("未导出", error, ui::Tone::Error);
            }
            continue;
        }
        if (choice != 1) {
            continue;
        }
        // Installed software does not start without its ticket. Asked
        // fresh, so a misread list never words it as harmless.
        const bool needed = library::installedAnywhere(ticket) ||
                            library_.onSdCard.count(ticket) != 0 ||
                            unused.count(ticket) == 0;
        if (!confirmDanger("删除",
                           needed ? "删除 " + name +
                                        " 启动所需的票据？"
                                  : "删除“" + name + "”的票据？",
                           "删除")) {
            continue;
        }
        std::string error;
        bool unsaved = false;
        if (!library::deleteTicket(ticket, error, &unsaved)) {
            // One the console will not export can still go, once the
            // person has seen what it is.
            if (!unsaved) {
                ui::message("未删除", error, ui::Tone::Error);
            } else if (confirmUnsavedTickets({ticket}, error) &&
                       !library::deleteTicket(ticket, error, nullptr, true)) {
                ui::message("未删除", error, ui::Tone::Error);
            }
        }
        reloadLibrary();
    }
}

void App::deleteUnusedTickets()
{
    // Decided again from scratch, never from what the screens showed: a
    // misread list of installed software makes the tickets games need to
    // start look unused (a user lost 256 that way).
    std::vector<uint64_t> unused;
    std::string error;
    bool checked = false;
    tasks::busy("票据", "正在检查未使用的票据……",
                [&]() { checked = library::checkUnusedTickets(unused, error); });
    if (!checked) {
        ui::message("未删除任何内容", error, ui::Tone::Error);
        reloadLibrary();
        return;
    }
    if (unused.empty()) {
        ui::message("没有可删除的内容", "所有票据都属于已安装的软件。");
        reloadLibrary();
        return;
    }
    if (!confirmDanger("删除",
                       "删除 " + plural(unused.size(), "个未使用的票据") + "？",
                       "删除")) {
        return;
    }
    size_t deleted = 0;
    // Those kept only because they could not be saved first, and why.
    std::vector<uint64_t> unsaved;
    std::string unsavedWhy;
    tasks::busy(
        "删除", "正在删除未使用的票据……",
        [&](tasks::Busy &busy) {
            busy.total = unused.size();
            for (const uint64_t ticket : unused) {
                // Once more just before it goes, in case something was
                // installed meanwhile.
                bool couldNotSave = false;
                if (!library::installedAnywhere(ticket) &&
                    library::deleteTicket(ticket, error, &couldNotSave)) {
                    ++deleted;
                } else if (couldNotSave) {
                    unsaved.push_back(ticket);
                    unsavedWhy = error;
                }
                busy.done.fetch_add(1);
            }
        },
        false);
    // Asked once for all of them, with what each was.
    if (!unsaved.empty() && confirmUnsavedTickets(unsaved, unsavedWhy)) {
        tasks::busy(
            "删除", "正在删除未备份的票据……",
            [&](tasks::Busy &busy) {
                busy.total = unsaved.size();
                for (const uint64_t ticket : unsaved) {
                    if (!library::installedAnywhere(ticket) &&
                        library::deleteTicket(ticket, error, nullptr, true)) {
                        ++deleted;
                    }
                    busy.done.fetch_add(1);
                }
            },
            false);
    }
    if (deleted == unused.size()) {
        ui::message("票据已删除", plural(deleted, "个未使用的票据") +
                                           " deleted.",
                    ui::Tone::Success);
    } else {
        ui::message("仍有部分票据",
                    plural(deleted, "票据") + "，共 " +
                        std::to_string(unused.size()) + " 个，已删除。",
                    ui::Tone::Warning, error);
    }
    reloadLibrary();
}

void App::cleanUpScreen()
{
    int focus = 0;
    while (ui::keepRunning() && !quit_) {
        const size_t pending = library_.pending.size();
        const size_t unused =
            library::unusedTickets(library_).size();
        const size_t saved = library::deletedTicketCount();
        ui::ScreenSpec spec;
        spec.back = "返回";
        const auto add = [&](ui::Rect rect, const std::string &label,
                             bool enabled) {
            ui::Control control;
            control.rect = rect;
            control.label = label;
            control.icon = ui::Icon::Trash;
            control.iconColor = ui::color::IconRed;
            control.enabled = enabled;
            control.scale = ui::type::Small + 0.06f;
            spec.controls.push_back(control);
        };
        // Centred between the top and the tabs.
        add({22, 26, 276, 44}, "移除未完成的安装", pending > 0);
        add({22, 82, 276, 44}, "删除未使用的票据", unused > 0);
        add({22, 138, 276, 44}, "恢复已删除的票据", saved > 0);
        spec.controls.back().icon = ui::Icon::History;
        spec.controls.back().iconColor = ui::color::IconGreen;
        spec.drawTop = [&]() {
            ui::drawTopChrome("清理", ui::Icon::Trash, ui::color::IconRed);
            ui::drawPanel();
            ui::drawFacts({{"未完成", plural(pending, "install"),
                            pending > 0 ? ui::color::Badge : ui::color::Tag},
                           {"未使用", plural(unused, "ticket")},
                           {"已保存", plural(saved, "deleted ticket")}});
        };
        const ui::ScreenResult result = ui::runScreen(spec, focus);
        if (result.choice == 0) {
            if (!confirmDanger("清理",
                               "Remove " + plural(pending, "unfinished install") +
                                   "?",
                               "移除")) {
                continue;
            }
            size_t removed = 0;
            std::string error;
            tasks::busy(
                "清理", "正在移除未完成的安装……",
                [&](tasks::Busy &busy) {
                    busy.total = library_.pending.size();
                    for (const library::PendingTitle &title : library_.pending) {
                        if (library::deletePending(title, error)) {
                            ++removed;
                        }
                        busy.done.fetch_add(1);
                    }
                },
                false);
            ui::message(removed == pending ? "清理完成" : "仍有剩余",
                        plural(removed, "未完成的安装") + "，已移除。",
                        removed == pending ? ui::Tone::Success
                                           : ui::Tone::Warning,
                        error);
            reloadLibrary();
        } else if (result.choice == 1) {
            deleteUnusedTickets();
        } else if (result.choice == 2) {
            std::string error;
            size_t restored = 0;
            tasks::busy("票据", "正在恢复已删除的票据……", [&]() {
                restored = library::restoreDeletedTickets(error);
            });
            ui::message(restored == saved ? "票据已恢复"
                                          : "部分票据未恢复",
                        "已恢复 " + plural(restored, "票据") + "，共 " +
                            std::to_string(saved) + " 个。",
                        restored == saved ? ui::Tone::Success
                                          : ui::Tone::Warning,
                        error);
            reloadLibrary();
        } else {
            return;
        }
    }
}

void App::importSeeds()
{
    std::string path;
    for (const char *place : SeedDbPlaces) {
        if (fs::fileExists(place)) {
            path = place;
            break;
        }
    }
    if (path.empty()) {
        ui::message("未找到 seeddb.bin",
                    "请将 seeddb.bin 放入 sdmc:/、sdmc:/3ds/ 或 "
                    "sdmc:/3ds/3dsDL/ 后重试。",
                    ui::Tone::Info);
        return;
    }
    std::string data;
    std::vector<titles::SeedEntry> entries;
    std::string error;
    if (!fs::readFile(path, data) || !titles::parseSeedDb(data, entries, error)) {
        ui::message("未导入种子",
                    error.empty() ? "无法读取 " + path + "。" : error,
                    ui::Tone::Error, path);
        return;
    }
    const std::vector<titles::SeedEntry> wanted =
        titles::seedsFor(entries, library_.titles);
    if (wanted.empty()) {
        ui::message("无需种子",
                    "此主机安装的游戏中没有一个在此文件里：" + path + "（" +
                        plural(entries.size(), "种子") + "）。");
        return;
    }
    ui::Dialog dialog;
    dialog.header = "种子";
    dialog.title = "从 " + path + " 为 " +
                   plural(wanted.size(), "已安装的游戏") + " 添加种子？";
    dialog.tone = ui::Tone::Info;
    dialog.buttons = {"添加", "取消"};
    dialog.cancelButton = 1;
    if (ui::choose(dialog) != 0) {
        return;
    }
    size_t added = 0;
    tasks::busy(
        "种子", "正在添加种子……",
        [&](tasks::Busy &busy) {
            busy.total = wanted.size();
            for (const titles::SeedEntry &entry : wanted) {
                if (installer::addSeed(entry.titleId, entry.seed.data(), error)) {
                    ++added;
                }
                busy.done.fetch_add(1);
            }
        },
        false);
    ui::message(added == wanted.size() ? "种子已添加" : "部分种子未添加",
                plural(added, "seed") + " added.",
                added == wanted.size() ? ui::Tone::Success : ui::Tone::Warning,
                error);
}
