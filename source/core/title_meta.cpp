#include "title_meta.hpp"

#include "smdh.hpp"
#include "text.hpp"

#include <cstdio>
#include <cstring>

namespace titlemeta {
namespace {

constexpr uint64_t MediaUnit = 0x200;
// The colour behind a DS icon's transparent pixels: the panel colour of the
// top screen, so the icon looks cut out.
constexpr uint16_t IconBackground = 0x1127;

uint16_t le16(const uint8_t *data)
{
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

uint32_t le32(const uint8_t *data)
{
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t le64(const uint8_t *data)
{
    return le32(data) | (static_cast<uint64_t>(le32(data + 4)) << 32);
}

uint64_t be64(const uint8_t *data)
{
    uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value = (value << 8) | data[index];
    }
    return value;
}

// Printable ASCII from a fixed-size field, stopping at the first NUL.
std::string ascii(const uint8_t *data, size_t size)
{
    std::string value;
    for (size_t index = 0; index < size && data[index] != 0; ++index) {
        value += data[index] >= 0x20 && data[index] < 0x7f
            ? static_cast<char>(data[index])
            : '?';
    }
    return text::trim(value);
}

void takeSmdh(const smdh::Info &info, Meta &meta)
{
    meta.name = info.shortName;
    meta.publisher = info.publisher;
    meta.icon = info.icon;
}

bool readSmdhAt(const cia::Reader &read, uint64_t offset, Meta &meta)
{
    std::vector<uint8_t> block(smdh::Size);
    smdh::Info info;
    if (!read(offset, block.data(), block.size()) ||
        !smdh::parse(block.data(), block.size(), info)) {
        return false;
    }
    takeSmdh(info, meta);
    return true;
}

bool readHomebrew(const cia::Reader &read, Meta &meta, std::string &error)
{
    uint8_t header[0x2c];
    if (!read(0, header, sizeof(header)) ||
        std::memcmp(header, "3DSX", 4) != 0) {
        error = "This is not a 3DSX file.";
        return false;
    }
    // Only the extended header has the SMDH's place.
    if (le16(header + 4) >= sizeof(header) && le32(header + 0x24) > 0) {
        readSmdhAt(read, le32(header + 0x20), meta);
    }
    if (meta.name.empty()) {
        meta.note = "It has no built-in name or icon.";
    }
    return true;
}

bool readCartridge(const cia::Reader &read, Meta &meta, std::string &error)
{
    uint8_t ncsd[0x200];
    if (!read(0, ncsd, sizeof(ncsd)) || std::memcmp(ncsd + 0x100, "NCSD", 4)) {
        error = "This is not a 3DS game image.";
        return false;
    }
    meta.titleId = le64(ncsd + 0x108);
    meta.hasTitleId = true;

    const uint64_t ncchOffset = le32(ncsd + 0x120) * MediaUnit;
    uint8_t ncch[0x200];
    if (!read(ncchOffset, ncch, sizeof(ncch)) ||
        std::memcmp(ncch + 0x100, "NCCH", 4) != 0) {
        meta.note = "The game partition could not be read.";
        return true;
    }
    meta.productCode = ascii(ncch + 0x150, 16);
    const uint8_t flags = ncch[0x18f];
    if ((flags & 0x04) == 0) {
        meta.note = "It is encrypted, so its name and icon are hidden.";
        return true;
    }

    // The icon is the "icon" file in the ExeFS.
    const uint64_t exefs = ncchOffset + le32(ncch + 0x1a0) * MediaUnit;
    uint8_t files[0xa0];
    if (le32(ncch + 0x1a0) == 0 || !read(exefs, files, sizeof(files))) {
        return true;
    }
    for (int index = 0; index < 10; ++index) {
        const uint8_t *entry = files + index * 16;
        if (std::memcmp(entry, "icon", 5) == 0) {
            readSmdhAt(read, exefs + 0x200 + le32(entry + 8), meta);
            break;
        }
    }
    return true;
}

// DS icons are 32x32, 4 bits a pixel in 8x8 tiles, with a 16-colour palette
// of BGR555 values. Shown at 48x48 like the others.
std::vector<uint16_t> dsIcon(const uint8_t *bitmap, const uint8_t *palette)
{
    uint16_t colours[16];
    for (int index = 0; index < 16; ++index) {
        const uint16_t value = le16(palette + index * 2);
        const uint16_t red = value & 0x1f;
        const uint16_t green = (value >> 5) & 0x1f;
        const uint16_t blue = (value >> 10) & 0x1f;
        colours[index] = static_cast<uint16_t>((red << 11) | (green << 6) |
                                               ((green >> 4) << 5) | blue);
    }
    uint8_t pixels[32 * 32];
    for (int tile = 0; tile < 16; ++tile) {
        for (int index = 0; index < 32; ++index) {
            const uint8_t pair = bitmap[tile * 32 + index];
            const int x = (tile % 4) * 8 + (index % 4) * 2;
            const int y = (tile / 4) * 8 + index / 4;
            pixels[y * 32 + x] = pair & 0x0f;
            pixels[y * 32 + x + 1] = pair >> 4;
        }
    }
    std::vector<uint16_t> icon(48 * 48);
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 48; ++x) {
            const uint8_t index = pixels[(y * 2 / 3) * 32 + x * 2 / 3];
            icon[y * 48 + x] = index == 0 ? IconBackground : colours[index];
        }
    }
    return icon;
}

bool readNds(const cia::Reader &read, Meta &meta, std::string &error)
{
    uint8_t header[0x70];
    if (!read(0, header, sizeof(header))) {
        error = "The file could not be read.";
        return false;
    }
    meta.name = ascii(header, 12);
    meta.productCode = ascii(header + 0x0c, 4);
    const uint32_t banner = le32(header + 0x68);
    uint8_t data[0x440];
    if (banner == 0 || !read(banner, data, sizeof(data))) {
        if (meta.name.empty()) {
            error = "This is not a DS game.";
            return false;
        }
        return true;
    }
    meta.icon = dsIcon(data + 0x20, data + 0x220);

    // The English title has up to three lines; the last one usually names
    // the publisher.
    const uint8_t *title = data + 0x340;
    constexpr size_t TitleUnits = 0x80;
    std::vector<std::string> lines;
    size_t lineStart = 0;
    for (size_t index = 0; index <= TitleUnits; ++index) {
        const bool end = index == TitleUnits || le16(title + index * 2) == 0;
        if (end || le16(title + index * 2) == '\n') {
            lines.push_back(smdh::utf16ToUtf8(title + lineStart * 2,
                                              index - lineStart));
            lineStart = index + 1;
        }
        if (end) {
            break;
        }
    }
    if (lines.size() > 1) {
        meta.publisher = lines.back();
        lines.pop_back();
    }
    std::string joined;
    for (const std::string &line : lines) {
        if (!line.empty()) {
            joined += (joined.empty() ? "" : " ") + line;
        }
    }
    if (!joined.empty()) {
        meta.name = joined;
    }
    return true;
}

bool readTicket(const cia::Reader &read, uint64_t size, Meta &meta,
                std::string &error)
{
    uint8_t type[4];
    if (!read(0, type, sizeof(type))) {
        error = "The file could not be read.";
        return false;
    }
    const uint32_t signature = (static_cast<uint32_t>(type[0]) << 24) |
                               (type[1] << 16) | (type[2] << 8) | type[3];
    size_t offset = 0;
    switch (signature) {
    case 0x10000:
    case 0x10003:
        offset = 4 + 0x200 + 0x3c;
        break;
    case 0x10001:
    case 0x10004:
        offset = 4 + 0x100 + 0x3c;
        break;
    case 0x10002:
    case 0x10005:
        offset = 4 + 0x3c + 0x40;
        break;
    default:
        error = "This is not a ticket.";
        return false;
    }
    uint8_t id[8];
    if ((size > 0 && offset + 0x9c + 8 > size) ||
        !read(offset + 0x9c, id, sizeof(id))) {
        error = "The ticket is incomplete.";
        return false;
    }
    meta.titleId = be64(id);
    meta.hasTitleId = true;
    return true;
}

} // namespace

bool read(const cia::Reader &read, uint64_t size, filetype::Kind kind,
          Meta &meta, std::string &error)
{
    meta = Meta();
    switch (kind) {
    case filetype::Kind::Homebrew:
        return readHomebrew(read, meta, error);
    case filetype::Kind::Smdh:
        if (!readSmdhAt(read, 0, meta)) {
            error = "This is not an SMDH file.";
            return false;
        }
        return true;
    case filetype::Kind::CartridgeImage:
        return readCartridge(read, meta, error);
    case filetype::Kind::Nds:
        return readNds(read, meta, error);
    case filetype::Kind::Ticket:
        return readTicket(read, size, meta, error);
    default:
        error = "There is nothing to show for this kind of file.";
        return false;
    }
}

bool read(const std::string &path, filetype::Kind kind, Meta &meta,
          std::string &error)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        meta = Meta();
        error = "The file could not be opened.";
        return false;
    }
    uint64_t size = 0;
    if (fseeko(file, 0, SEEK_END) == 0) {
        const off_t end = ftello(file);
        size = end > 0 ? static_cast<uint64_t>(end) : 0;
    }
    const cia::Reader reader = [file](uint64_t offset, void *buffer,
                                      size_t count) {
        return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0 &&
               std::fread(buffer, 1, count, file) == count;
    };
    const bool ok = read(reader, size, kind, meta, error);
    std::fclose(file);
    return ok;
}

} // namespace titlemeta
