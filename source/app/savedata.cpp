#include "savedata.hpp"

#include "cia.hpp"
#include "file_ops.hpp"
#include "fs_util.hpp"
#include "saves.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <set>

namespace savedata {

namespace {

// FS results Checkpoint handles (see core/saves.hpp for the source).
constexpr ::Result InUse = static_cast<::Result>(0xC92044E6);
constexpr ::Result SparseRead = static_cast<::Result>(0xD900458B);
constexpr size_t MaxPathUnits = 0x100;
constexpr u32 ChunkSize = 0x50000;

// A UTF-16 FS path inside an archive, such as "/main".
class ArchivePath {
public:
    explicit ArchivePath(const std::string &path)
    {
        units_.assign(path.size() + 1, 0);
        const ssize_t count = utf8_to_utf16(
            units_.data(), reinterpret_cast<const uint8_t *>(path.c_str()),
            path.size());
        units_.resize(count >= 0 ? static_cast<size_t>(count) + 1 : 1);
        units_.back() = 0;
    }
    size_t length() const { return units_.size() - 1; }
    FS_Path get() const
    {
        return {PATH_UTF16, static_cast<u32>(units_.size() * 2), units_.data()};
    }

private:
    std::vector<uint16_t> units_;
};

std::string fromUtf16(const uint16_t *units)
{
    size_t length = 0;
    while (length < 0x106 && units[length] != 0) {
        ++length;
    }
    // The last argument is the room for the UTF-8, up to three bytes a unit.
    std::string out(length * 3, '\0');
    const ssize_t bytes = utf16_to_utf8(reinterpret_cast<uint8_t *>(&out[0]),
                                        units, out.size());
    out.resize(bytes > 0 ? std::min(static_cast<size_t>(bytes), out.size())
                         : 0);
    return out;
}

std::string describe(::Result code)
{
    char text[24];
    std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(code));
    return text;
}

class Archive {
public:
    ~Archive()
    {
        if (open_) {
            FSUSER_CloseArchive(handle_);
        }
    }
    ::Result open(const Target &target)
    {
        const u32 low = static_cast<u32>(target.titleId);
        const u32 high = static_cast<u32>(target.titleId >> 32);
        ::Result code;
        if (target.kind == Kind::ExtData) {
            const u32 path[3] = {MEDIATYPE_SD, target.extDataId, 0};
            code = FSUSER_OpenArchive(&handle_, ARCHIVE_EXTDATA,
                                      {PATH_BINARY, sizeof(path), path});
        } else if (target.media == MEDIATYPE_NAND) {
            const u32 path[2] = {MEDIATYPE_NAND, 0x00020000 | (low >> 8)};
            code = FSUSER_OpenArchive(&handle_, ARCHIVE_SYSTEM_SAVEDATA,
                                      {PATH_BINARY, sizeof(path), path});
        } else {
            const u32 path[3] = {static_cast<u32>(target.media), low, high};
            code = FSUSER_OpenArchive(&handle_, ARCHIVE_USER_SAVEDATA,
                                      {PATH_BINARY, sizeof(path), path});
        }
        open_ = R_SUCCEEDED(code);
        return code;
    }
    FS_Archive get() const { return handle_; }

private:
    FS_Archive handle_ = 0;
    bool open_ = false;
};

struct Context {
    const Target &target;
    FS_Archive archive;
    Progress &progress;
    std::unique_ptr<uint8_t[]> buffer;
    Result result;
};

bool fail(Context &context, const std::string &message)
{
    if (context.result.message.empty()) {
        context.result.message = message;
    }
    return false;
}

// Archive file at path to the SD card file at destination.
bool copyOut(Context &context, const std::string &path,
             const std::string &destination)
{
    Handle file = 0;
    const ArchivePath source(path);
    const ::Result opened = FSUSER_OpenFile(&file, context.archive, source.get(),
                                            FS_OPEN_READ, 0);
    if (opened == InUse && context.target.kind == Kind::ExtData) {
        // The system holds a few extra data files open (HOME Menu's
        // cache); Checkpoint leaves them out too.
        ++context.result.skipped;
        return true;
    }
    if (R_FAILED(opened)) {
        return fail(context, "Could not read " + path + " (" + describe(opened) + ").");
    }
    u64 size = 0;
    FSFILE_GetSize(file, &size);
    FILE *out = std::fopen(destination.c_str(), "wb");
    if (out == nullptr) {
        FSFILE_Close(file);
        return fail(context, "Could not write " + destination + ".");
    }
    u64 offset = 0;
    u32 chunk = ChunkSize;
    bool ok = true;
    while (offset < size && ok) {
        const u32 wanted = static_cast<u32>(std::min<u64>(chunk, size - offset));
        u32 read = 0;
        const ::Result code = FSFILE_Read(file, &read, offset, context.buffer.get(),
                                          wanted);
        if (code == SparseRead && context.target.kind == Kind::ExtData) {
            // Sparse extra data reads fail past the written part: read in
            // smaller pieces, and stop the file where nothing more reads.
            chunk /= 2;
            if (chunk == 0) {
                break;
            }
            continue;
        }
        if (R_FAILED(code) || read == 0) {
            ok = fail(context, "Could not read " + path + " (" + describe(code) + ").");
            break;
        }
        if (std::fwrite(context.buffer.get(), 1, read, out) != read) {
            ok = fail(context, "The SD card could not take " + destination + ".");
            break;
        }
        offset += read;
        context.progress.bytes += read;
    }
    FSFILE_Close(file);
    if (std::fclose(out) != 0) {
        ok = fail(context, "The SD card could not take " + destination + ".");
    }
    if (ok) {
        ++context.result.files;
        ++context.progress.files;
    }
    return ok;
}

// Everything under the archive folder path (ending in '/') into the SD card
// folder destination (ending in '/').
bool copyTreeOut(Context &context, const std::string &path,
                 const std::string &destination)
{
    Handle directory = 0;
    const ArchivePath folder(path);
    const ::Result code =
        FSUSER_OpenDirectory(&directory, context.archive, folder.get());
    if (R_FAILED(code)) {
        return fail(context, "Could not open " + path + " (" + describe(code) + ").");
    }
    std::vector<FS_DirectoryEntry> entries(16);
    bool ok = true;
    while (ok) {
        if (context.progress.cancel) {
            ok = fail(context, "Stopped.");
            break;
        }
        u32 count = 0;
        if (R_FAILED(FSDIR_Read(directory, &count,
                                static_cast<u32>(entries.size()),
                                entries.data())) ||
            count == 0) {
            break;
        }
        for (u32 index = 0; index < count && ok; ++index) {
            const std::string name = fromUtf16(entries[index].name);
            if (name.empty()) {
                continue;
            }
            if (entries[index].attributes & FS_ATTRIBUTE_DIRECTORY) {
                const std::string inner = destination + name + "/";
                ok = (mkdir(inner.c_str(), 0777) == 0 || fs::directoryExists(inner))
                         ? copyTreeOut(context, path + name + "/", inner)
                         : fail(context, "Could not create " + inner + ".");
            } else {
                ok = copyOut(context, path + name, destination + name);
            }
        }
    }
    FSDIR_Close(directory);
    return ok;
}

struct Item {
    std::string path; // inside the archive, e.g. "/data/main"
    bool folder = false;
    uint64_t size = 0;
};

// The backup's folders and files, parents before children, leaving out what
// computers add to folders. False, with a reason, when the backup cannot be
// restored as it is.
bool collect(const std::string &root, const std::string &relative,
             std::vector<Item> &items, std::string &error)
{
    bool readable = false;
    const std::vector<fs::Entry> entries =
        fs::listEntries(root + relative, &readable, true, false);
    if (!readable) {
        error = "Could not read " + root + relative + ".";
        return false;
    }
    for (const fs::Entry &entry : entries) {
        if (saves::isHostMetadata(entry.name)) {
            continue;
        }
        Item item;
        item.path = "/" + relative + entry.name;
        item.folder = entry.folder;
        item.size = entry.size > 0 ? static_cast<uint64_t>(entry.size) : 0;
        if (ArchivePath(item.path).length() > MaxPathUnits) {
            error = item.path + " has a path too long for save data.";
            return false;
        }
        items.push_back(item);
        if (entry.folder &&
            !collect(root, relative + entry.name + "/", items, error)) {
            return false;
        }
    }
    return true;
}

// Files below path in the archive, for clearing extra data.
void listArchive(FS_Archive archive, const std::string &path,
                 std::vector<Item> &items)
{
    Handle directory = 0;
    const ArchivePath folder(path);
    if (R_FAILED(FSUSER_OpenDirectory(&directory, archive, folder.get()))) {
        return;
    }
    std::vector<FS_DirectoryEntry> entries(16);
    while (true) {
        u32 count = 0;
        if (R_FAILED(FSDIR_Read(directory, &count,
                                static_cast<u32>(entries.size()),
                                entries.data())) ||
            count == 0) {
            break;
        }
        for (u32 index = 0; index < count; ++index) {
            const std::string name = fromUtf16(entries[index].name);
            Item item;
            item.path = path + name;
            item.folder = (entries[index].attributes & FS_ATTRIBUTE_DIRECTORY) != 0;
            items.push_back(item);
            if (item.folder) {
                listArchive(archive, item.path + "/", items);
            }
        }
    }
    FSDIR_Close(directory);
}

bool copyIn(Context &context, const std::string &source, const Item &item)
{
    const ArchivePath path(item.path);
    // Extra data files cannot grow, so every file is made at its full size
    // first; an existing save file is simply opened.
    FSUSER_CreateFile(context.archive, path.get(), 0, item.size);
    Handle file = 0;
    const ::Result opened =
        FSUSER_OpenFile(&file, context.archive, path.get(), FS_OPEN_WRITE, 0);
    if (R_FAILED(opened)) {
        return fail(context, "Could not write " + item.path + " (" +
                                 describe(opened) + ").");
    }
    FILE *in = std::fopen(source.c_str(), "rb");
    if (in == nullptr) {
        FSFILE_Close(file);
        return fail(context, "Could not read " + source + ".");
    }
    bool ok = true;
    u64 offset = 0;
    while (ok) {
        const size_t read = std::fread(context.buffer.get(), 1, ChunkSize, in);
        if (read == 0) {
            break;
        }
        u32 written = 0;
        const ::Result code = FSFILE_Write(file, &written, offset,
                                           context.buffer.get(),
                                           static_cast<u32>(read), 0);
        if (R_FAILED(code) || written != read) {
            ok = fail(context, "Could not write " + item.path + " (" +
                                   describe(code) + ").");
            break;
        }
        offset += read;
        context.progress.bytes += read;
    }
    std::fclose(in);
    FSFILE_Flush(file);
    FSFILE_Close(file);
    if (ok) {
        ++context.result.files;
        ++context.progress.files;
    }
    return ok;
}

} // namespace

Target saveOf(const titles::Title &title, FS_MediaType media)
{
    Target target;
    target.kind = Kind::Save;
    target.titleId = title.id;
    target.media = media;
    target.name = title.name;
    target.folder = std::string(saves::SavesRoot) +
                    saves::titleFolder(title.id, title.englishShortName) + "/";
    return target;
}

Target extDataOf(const titles::Title &title)
{
    Target target;
    target.kind = Kind::ExtData;
    target.titleId = title.id;
    target.extDataId = saves::extDataId(title.id);
    target.name = title.name;
    target.folder = std::string(saves::ExtDataRoot) +
                    saves::titleFolder(title.id, title.englishShortName) + "/";
    return target;
}

Target extDataOf(uint64_t extDataId, const std::string &englishShortName,
                 const std::string &name)
{
    Target target;
    target.kind = Kind::ExtData;
    target.titleId = 0x0004000000000000ULL | (extDataId << 8);
    target.extDataId = static_cast<uint32_t>(extDataId);
    target.name = name;
    target.folder = std::string(saves::ExtDataRoot) +
                    saves::titleFolder(target.titleId, englishShortName) + "/";
    return target;
}

bool exists(const Target &target)
{
    Archive archive;
    return R_SUCCEEDED(archive.open(target));
}

std::string unsupportedReason(const titles::Title &title)
{
    if (cia::kind(title.id) == cia::Kind::DsiWare) {
        return "DSiWare saves live in a different place that 3dsDL does not "
               "open yet. Checkpoint can back them up.";
    }
    if (saves::isExcluded(title.id)) {
        return "This title has no save data of its own.";
    }
    return "";
}

std::vector<std::string> backups(const Target &target)
{
    std::vector<std::string> names;
    for (const fs::Entry &entry : fs::listEntries(target.folder, nullptr, true, false)) {
        if (entry.folder) {
            names.push_back(entry.name);
        }
    }
    saves::sortNewestFirst(names);
    return names;
}

Result backup(const Target &target, const std::string &name, Progress &progress)
{
    Context context {target, 0, progress, nullptr, Result()};
    context.buffer.reset(new (std::nothrow) uint8_t[ChunkSize]);
    if (!context.buffer) {
        context.result.message = "Not enough memory.";
        return context.result;
    }
    Archive archive;
    const ::Result code = archive.open(target);
    if (R_FAILED(code)) {
        context.result.message = "The data could not be opened (" +
                                 describe(code) + ").";
        return context.result;
    }
    context.archive = archive.get();

    const std::string destination = target.folder + name;
    if (!fs::makeDirectories(target.folder)) {
        context.result.message = "Could not create " + target.folder + ".";
        return context.result;
    }
    if (fileops::exists(destination)) {
        // Choosing an existing backup replaces it, as in Checkpoint.
        struct Quiet : fileops::Observer {
            bool cancelRequested() override { return false; }
        } quiet;
        uint64_t done = 0;
        fileops::remove(destination, quiet, done, 0);
    }
    if (!fs::makeDirectories(destination + "/")) {
        context.result.message = "Could not create " + destination + ".";
        return context.result;
    }
    if (!copyTreeOut(context, "/", destination + "/")) {
        // Half a backup is worse than none.
        struct Quiet : fileops::Observer {
            bool cancelRequested() override { return false; }
        } quiet;
        uint64_t done = 0;
        fileops::remove(destination, quiet, done, 0);
        return context.result;
    }
    context.result.ok = true;
    return context.result;
}

Result restore(const Target &target, const std::string &name, Progress &progress)
{
    Context context {target, 0, progress, nullptr, Result()};
    const std::string source = target.folder + name + "/";

    // Check the backup first, so a bad one never wipes good data.
    std::vector<Item> items;
    std::string error;
    if (!collect(source, "", items, error)) {
        context.result.message = error;
        return context.result;
    }
    const bool anyFile = std::any_of(items.begin(), items.end(),
                                     [](const Item &item) { return !item.folder; });
    if (!anyFile) {
        context.result.message = "This backup has no files in it.";
        return context.result;
    }
    context.buffer.reset(new (std::nothrow) uint8_t[ChunkSize]);
    if (!context.buffer) {
        context.result.message = "Not enough memory.";
        return context.result;
    }
    Archive archive;
    ::Result code = archive.open(target);
    if (R_FAILED(code)) {
        context.result.message = "The data could not be opened (" +
                                 describe(code) + ").";
        return context.result;
    }
    context.archive = archive.get();
    const ArchivePath root("/");

    std::set<std::string> inUse;
    if (target.kind == Kind::Save) {
        code = FSUSER_DeleteDirectoryRecursively(context.archive, root.get());
        if (R_FAILED(code)) {
            context.result.message = "The old save could not be cleared (" +
                                     describe(code) + ").";
            return context.result;
        }
    } else {
        // Extra data is cleared file by file, leaving what the system holds
        // open; those files are not restored either.
        std::vector<Item> present;
        listArchive(context.archive, "/", present);
        for (const Item &item : present) {
            if (item.folder) {
                continue;
            }
            const ArchivePath path(item.path);
            Handle file = 0;
            const ::Result opened = FSUSER_OpenFile(&file, context.archive,
                                                    path.get(), FS_OPEN_READ, 0);
            if (opened == InUse) {
                inUse.insert(item.path);
                continue;
            }
            if (R_SUCCEEDED(opened)) {
                FSFILE_Close(file);
            }
            FSUSER_DeleteFile(context.archive, path.get());
        }
        for (auto item = present.rbegin(); item != present.rend(); ++item) {
            if (item->folder) {
                FSUSER_DeleteDirectory(context.archive, ArchivePath(item->path).get());
            }
        }
    }

    for (const Item &item : items) {
        if (context.progress.cancel) {
            fail(context, "Stopped.");
            break;
        }
        if (item.folder) {
            FSUSER_CreateDirectory(context.archive, ArchivePath(item.path).get(), 0);
            continue;
        }
        if (inUse.count(item.path) != 0) {
            ++context.result.skipped;
            continue;
        }
        if (!copyIn(context, source + item.path.substr(1), item)) {
            break;
        }
    }
    if (!context.result.message.empty()) {
        return context.result;
    }

    if (target.kind == Kind::Save) {
        code = FSUSER_ControlArchive(context.archive,
                                     ARCHIVE_ACTION_COMMIT_SAVE_DATA, nullptr, 0,
                                     nullptr, 0);
        if (R_FAILED(code)) {
            context.result.message = "The save could not be committed (" +
                                     describe(code) + ").";
            return context.result;
        }
        // Games that check their save against a secure value would call a
        // restored save corrupt; Checkpoint deletes the value, and so does
        // this, with the same value.
        const u32 unique = static_cast<u32>(target.titleId) >> 8;
        u64 secureValue = (static_cast<u64>(SECUREVALUE_SLOT_SD) << 32) |
                          (static_cast<u64>(unique) << 8);
        u8 existed = 0;
        code = FSUSER_ControlSecureSave(SECURESAVE_ACTION_DELETE, &secureValue,
                                        sizeof(secureValue), &existed, 1);
        if (R_FAILED(code)) {
            context.result.message = "The save was restored, but its secure "
                                     "value could not be cleared (" +
                                     describe(code) + "). The game may call "
                                     "it corrupted.";
            return context.result;
        }
    }
    context.result.ok = true;
    return context.result;
}

bool mount(const Target &target, const std::string &device, std::string &error)
{
    const u32 low = static_cast<u32>(target.titleId);
    const u32 high = static_cast<u32>(target.titleId >> 32);
    ::Result code;
    if (target.kind == Kind::ExtData) {
        const u32 path[3] = {MEDIATYPE_SD, target.extDataId, 0};
        code = archiveMount(ARCHIVE_EXTDATA, {PATH_BINARY, sizeof(path), path},
                            device.c_str());
    } else if (target.media == MEDIATYPE_NAND) {
        const u32 path[2] = {MEDIATYPE_NAND, 0x00020000 | (low >> 8)};
        code = archiveMount(ARCHIVE_SYSTEM_SAVEDATA,
                            {PATH_BINARY, sizeof(path), path}, device.c_str());
    } else {
        const u32 path[3] = {static_cast<u32>(target.media), low, high};
        code = archiveMount(ARCHIVE_USER_SAVEDATA,
                            {PATH_BINARY, sizeof(path), path}, device.c_str());
    }
    if (R_FAILED(code)) {
        error = "The data could not be opened (" + describe(code) + ").";
        return false;
    }
    return true;
}

void unmount(const std::string &device)
{
    archiveUnmount(device.c_str());
}

} // namespace savedata
