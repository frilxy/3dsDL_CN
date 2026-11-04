#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Small string helpers shared by the app, the download engine and the tests.
// Nothing in core/ depends on libctru, so it all builds on the host as well.
namespace text {

std::string trim(const std::string &value);
std::string lower(std::string value);
bool startsWithNoCase(const std::string &value, const std::string &prefix);
bool endsWithNoCase(const std::string &value, const std::string &suffix);

// Length of the UTF-8 sequence starting at value[index], or 0 when the bytes
// there are not a valid, shortest-form encoding of a Unicode scalar value.
size_t utf8SequenceLength(const std::string &value, size_t index,
                          uint32_t *codePoint = nullptr);
bool isValidUtf8(const std::string &value);

// Replaces every byte that is not part of a valid UTF-8 sequence.
std::string sanitizeUtf8(const std::string &value, char replacement = '_');

// Cuts value to at most maxBytes without splitting a UTF-8 sequence.
std::string truncateUtf8(const std::string &value, size_t maxBytes);

std::string latin1ToUtf8(const std::string &value);

// Replaces tabs, newlines and other control characters with spaces so a value
// can be stored as one field of a line-based file.
std::string singleLine(const std::string &value);

} // namespace text
