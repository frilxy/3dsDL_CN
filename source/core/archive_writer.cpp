#include "archive_writer.hpp"

#include "fs_util.hpp"
#include "naming.hpp"
#include "text.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace archives {
namespace {

constexpr size_t PieceSize = 256 * 1024;
// FAT32's limit, less one byte.
constexpr uint64_t MaxFileSize = 0xffffffffull;

std::string writeError(const std::string &path)
{
    if (errno == ENOSPC) {
        return "The SD card is full.";
    }
    return "Could not write " + path + " (" + std::strerror(errno) + ").";
}

bool writeAll(int fd, const unsigned char *data, size_t size)
{
    while (size > 0) {
        const ssize_t wrote = ::write(fd, data, size);
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        if (wrote <= 0) {
            if (wrote == 0) {
                errno = EIO;
            }
            return false;
        }
        data += wrote;
        size -= static_cast<size_t>(wrote);
    }
    return true;
}

std::string parentOf(const std::string &path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? "" : path.substr(0, slash);
}

std::string nameOf(const std::string &path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

std::string safeRelativePath(const std::string &name)
{
    std::string normalised = name;
    for (char &character : normalised) {
        if (character == '\\') {
            character = '/';
        }
    }
    if (normalised.empty() || normalised[0] == '/') {
        return "";
    }

    std::string output;
    size_t start = 0;
    bool first = true;
    while (start <= normalised.size()) {
        const size_t slash = normalised.find('/', start);
        const size_t end = slash == std::string::npos ? normalised.size()
                                                      : slash;
        const std::string component = normalised.substr(start, end - start);

        if (component == "..") {
            return "";
        }
        if (first && component.find(':') != std::string::npos) {
            return "";
        }
        if (!component.empty() && component != ".") {
            if (!output.empty()) {
                output += '/';
            }
            output += naming::safeFilename(component);
            first = false;
        }

        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return output;
}

bool macMetadata(const std::string &path)
{
    const std::string name = nameOf(path);
    return path == "__MACOSX" || path.compare(0, 9, "__MACOSX/") == 0 ||
           name.compare(0, 2, "._") == 0 || name == ".DS_Store";
}

bool FolderNames::contains(const std::string &path)
{
    return namesIn(parentOf(path)).count(text::lower(nameOf(path))) > 0;
}

void FolderNames::madeEmpty(const std::string &folder)
{
    folders_[folder].clear();
}

const std::unordered_set<std::string> &
FolderNames::namesIn(const std::string &folder)
{
    const auto found = folders_.find(folder);
    if (found != folders_.end()) {
        return found->second;
    }
    std::unordered_set<std::string> &names = folders_[folder];
    const std::string full = folder.empty() ? root_ : root_ + folder + "/";
    if (DIR *directory = opendir(full.c_str())) {
        while (const dirent *entry = readdir(directory)) {
            const std::string name = entry->d_name;
            if (name != "." && name != "..") {
                names.insert(text::lower(name));
            }
        }
        closedir(directory);
    }
    return names;
}

Writer::Writer(std::string folder, Existing existing)
    : folder_(std::move(folder)), existing_(existing), names_(folder_),
      buffer_(new unsigned char[PieceSize])
{
}

Writer::~Writer()
{
    abandon();
}

bool Writer::makeRoot()
{
    if (fs::directoryExists(folder_)) {
        return true;
    }
    if (!fs::makeDirectories(folder_)) {
        return fail("Could not create " + folder_ + ".");
    }
    names_.madeEmpty("");
    return true;
}

bool Writer::makeFolder(const std::string &path)
{
    if (path.empty() || made_.count(path) > 0) {
        return true;
    }
    if (!makeFolder(parentOf(path))) {
        return false;
    }
    const std::string full = folder_ + path;
    if (mkdir(full.c_str(), 0777) == 0) {
        names_.madeEmpty(path);
    } else if (errno != EEXIST && !fs::directoryExists(full)) {
        return fail("Could not create the folder " + path + ".");
    }
    made_.insert(path);
    return true;
}

Writer::Start Writer::start(const std::string &path, int64_t size)
{
    abandon();
    error_.clear();
    path_ = path;
    size_ = size;
    written_ = 0;
    buffered_ = 0;
    replacing_ = false;
    if (size > static_cast<int64_t>(MaxFileSize)) {
        fail(path + " is over 4 GiB, more than the SD card can hold in one "
                    "file.");
        return Start::Failed;
    }
    if (!makeFolder(parentOf(path))) {
        return Start::Failed;
    }

    target_ = folder_ + path;
    temporary_ = target_ + ".3dsdl-tmp";
    // Written earlier in this run: the archive has the name twice. The
    // later copy replaces it like any file, so a damaged one cannot cost
    // the first.
    const bool again = done_.count(text::lower(path)) > 0;
    // Large files are written under another name and renamed once whole,
    // so a crash never leaves one that looks complete. That skips the
    // check after opening below, so they are looked up instead; they are
    // few.
    const bool large = size < 0 || size > static_cast<int64_t>(PieceSize);
    struct stat info {};
    const bool there =
        again || names_.contains(path) ||
        (large && stat(target_.c_str(), &info) == 0);
    if (there) {
        if (!again && existing_ == Existing::Skip) {
            return Start::Skip;
        }
        // A folder with the name is never moved aside.
        if (stat(target_.c_str(), &info) == 0 && !S_ISREG(info.st_mode)) {
            return Start::Skip;
        }
        replacing_ = true;
    }
    writing_ = replacing_ || large ? temporary_ : target_;
    if (!open(writing_ != target_)) {
        // A folder the listing missed has the name.
        if (writing_ == target_ && stat(target_.c_str(), &info) == 0 &&
            !S_ISREG(info.st_mode)) {
            error_.clear();
            return Start::Skip;
        }
        return Start::Failed;
    }

    // A new file is empty. One with data was missed by the listing (a
    // name FAT folds differently, or written since), so it is left alone
    // or replaced like any other.
    if (writing_ == target_ && fstat(fd_, &info) == 0 && info.st_size > 0) {
        close(fd_);
        fd_ = -1;
        if (existing_ == Existing::Skip) {
            return Start::Skip;
        }
        replacing_ = true;
        writing_ = temporary_;
        if (!open(true)) {
            return Start::Failed;
        }
    }

    // Taking the whole size at once finds a full SD card before any
    // writing and spares FAT growing the file piece by piece. A file that
    // fits in one write gets its size from that write.
    if (size > static_cast<int64_t>(PieceSize) &&
        ftruncate(fd_, static_cast<off_t>(size)) != 0) {
        fail(writeError(path));
        abandon();
        return Start::Failed;
    }
    return Start::Write;
}

bool Writer::open(bool truncate)
{
    fd_ = ::open(writing_.c_str(),
                 O_WRONLY | O_CREAT | (truncate ? O_TRUNC : 0), 0666);
    return fd_ >= 0 || fail(writeError(path_));
}

bool Writer::write(const void *data, size_t size)
{
    const unsigned char *bytes = static_cast<const unsigned char *>(data);
    if (written_ + size > MaxFileSize) {
        return fail(path_ + " is over 4 GiB, more than the SD card can hold "
                            "in one file.");
    }
    written_ += size;
    if (buffered_ == 0 && size >= PieceSize) {
        return writeAll(fd_, bytes, size) || fail(writeError(path_));
    }
    while (size > 0) {
        const size_t take = std::min(size, PieceSize - buffered_);
        std::memcpy(buffer_.get() + buffered_, bytes, take);
        buffered_ += take;
        bytes += take;
        size -= take;
        if (buffered_ == PieceSize && !flush()) {
            return false;
        }
    }
    return true;
}

bool Writer::writeZeros(uint64_t count)
{
    if (written_ + count > MaxFileSize) {
        return fail(path_ + " is over 4 GiB, more than the SD card can hold "
                            "in one file.");
    }
    written_ += count;
    while (count > 0) {
        const size_t take = static_cast<size_t>(
            std::min<uint64_t>(count, PieceSize - buffered_));
        std::memset(buffer_.get() + buffered_, 0, take);
        buffered_ += take;
        count -= take;
        if (buffered_ == PieceSize && !flush()) {
            return false;
        }
    }
    return true;
}

bool Writer::flush()
{
    if (buffered_ > 0 && !writeAll(fd_, buffer_.get(), buffered_)) {
        return fail(writeError(path_));
    }
    buffered_ = 0;
    return true;
}

bool Writer::finish()
{
    if (!flush()) {
        return false;
    }
    if (size_ >= 0 && written_ != static_cast<uint64_t>(size_)) {
        return fail(path_ + " is not the size the archive says.");
    }
    const int fd = fd_;
    fd_ = -1;
    if (close(fd) != 0) {
        const std::string message = writeError(path_);
        std::remove(writing_.c_str());
        return fail(message);
    }
    done_.insert(text::lower(path_));
    if (writing_ == target_) {
        return true;
    }
    // FAT renames fail when the target exists, so the old file steps aside
    // first and comes back if the new one cannot take its place.
    const std::string old = target_ + ".3dsdl-old";
    if (replacing_ && std::rename(target_.c_str(), old.c_str()) != 0 &&
        // An old copy left by a replace that was cut short after the new
        // file had its name: the file at target_ is the whole one.
        !(std::remove(old.c_str()) == 0 &&
          std::rename(target_.c_str(), old.c_str()) == 0)) {
        std::remove(writing_.c_str());
        return fail("Could not replace " + path_ + ".");
    }
    if (std::rename(writing_.c_str(), target_.c_str()) != 0) {
        std::remove(writing_.c_str());
        if (replacing_) {
            std::rename(old.c_str(), target_.c_str());
        }
        return fail("Could not save " + path_ + ".");
    }
    if (replacing_) {
        std::remove(old.c_str());
    }
    return true;
}

void Writer::abandon()
{
    if (fd_ < 0) {
        return;
    }
    close(fd_);
    fd_ = -1;
    buffered_ = 0;
    std::remove(writing_.c_str());
}

bool Writer::fail(const std::string &message)
{
    error_ = message;
    return false;
}

} // namespace archives
