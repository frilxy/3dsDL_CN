#include "text.hpp"

#include <algorithm>
#include <cctype>

namespace text {

std::string trim(const std::string &value)
{
    const char *whitespace = " \t\r\n";
    const size_t first = value.find_first_not_of(whitespace);
    if (first == std::string::npos) {
        return "";
    }
    const size_t last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

bool startsWithNoCase(const std::string &value, const std::string &prefix)
{
    return value.size() >= prefix.size() &&
           lower(value.substr(0, prefix.size())) == lower(prefix);
}

bool endsWithNoCase(const std::string &value, const std::string &suffix)
{
    return value.size() >= suffix.size() &&
           lower(value.substr(value.size() - suffix.size())) == lower(suffix);
}

size_t utf8SequenceLength(const std::string &value, size_t index,
                          uint32_t *codePoint)
{
    if (index >= value.size()) {
        return 0;
    }

    const auto byte = [&](size_t offset) {
        return static_cast<unsigned char>(value[index + offset]);
    };
    const unsigned char lead = byte(0);

    size_t length = 0;
    uint32_t code = 0;
    uint32_t minimum = 0;
    if (lead < 0x80) {
        if (codePoint != nullptr) {
            *codePoint = lead;
        }
        return 1;
    } else if ((lead & 0xe0) == 0xc0) {
        length = 2;
        code = lead & 0x1f;
        minimum = 0x80;
    } else if ((lead & 0xf0) == 0xe0) {
        length = 3;
        code = lead & 0x0f;
        minimum = 0x800;
    } else if ((lead & 0xf8) == 0xf0) {
        length = 4;
        code = lead & 0x07;
        minimum = 0x10000;
    } else {
        return 0;
    }

    if (index + length > value.size()) {
        return 0;
    }
    for (size_t offset = 1; offset < length; ++offset) {
        const unsigned char continuation = byte(offset);
        if ((continuation & 0xc0) != 0x80) {
            return 0;
        }
        code = (code << 6) | (continuation & 0x3f);
    }

    // Reject overlong forms, UTF-16 surrogates and values past U+10FFFF.
    if (code < minimum || (code >= 0xd800 && code <= 0xdfff) ||
        code > 0x10ffff) {
        return 0;
    }
    if (codePoint != nullptr) {
        *codePoint = code;
    }
    return length;
}

bool isValidUtf8(const std::string &value)
{
    for (size_t index = 0; index < value.size();) {
        const size_t length = utf8SequenceLength(value, index);
        if (length == 0) {
            return false;
        }
        index += length;
    }
    return true;
}

std::string sanitizeUtf8(const std::string &value, char replacement)
{
    std::string output;
    output.reserve(value.size());
    for (size_t index = 0; index < value.size();) {
        const size_t length = utf8SequenceLength(value, index);
        if (length == 0) {
            output += replacement;
            ++index;
        } else {
            output.append(value, index, length);
            index += length;
        }
    }
    return output;
}

std::string truncateUtf8(const std::string &value, size_t maxBytes)
{
    if (value.size() <= maxBytes) {
        return value;
    }
    size_t end = 0;
    for (size_t index = 0; index < value.size();) {
        size_t length = utf8SequenceLength(value, index);
        if (length == 0) {
            length = 1;
        }
        if (index + length > maxBytes) {
            break;
        }
        index += length;
        end = index;
    }
    return value.substr(0, end);
}

std::string latin1ToUtf8(const std::string &value)
{
    std::string output;
    output.reserve(value.size());
    for (const char character : value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte < 0x80) {
            output += character;
        } else {
            output += static_cast<char>(0xc0 | (byte >> 6));
            output += static_cast<char>(0x80 | (byte & 0x3f));
        }
    }
    return output;
}

std::string singleLine(const std::string &value)
{
    std::string output = value;
    for (char &character : output) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte < 0x20 || byte == 0x7f) {
            character = ' ';
        }
    }
    return output;
}

} // namespace text
