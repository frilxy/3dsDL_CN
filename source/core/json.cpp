#include "json.hpp"

#include <cmath>
#include <cstdlib>

namespace json {

namespace {

class Parser {
public:
    Parser(const std::string &text, int maxDepth)
        : text_(text), maxDepth_(maxDepth)
    {
    }

    bool document(Value &out, std::string &error)
    {
        skipSpace();
        if (!value(out, 0)) {
            error = error_;
            return false;
        }
        skipSpace();
        if (position_ != text_.size()) {
            error = "Unexpected text after the value at " + at() + ".";
            return false;
        }
        return true;
    }

private:
    const std::string &text_;
    const int maxDepth_;
    size_t position_ = 0;
    std::string error_;

    std::string at() const { return "offset " + std::to_string(position_); }

    bool fail(const std::string &what)
    {
        if (error_.empty()) {
            error_ = what + " at " + at() + ".";
        }
        return false;
    }

    bool atEnd() const { return position_ >= text_.size(); }
    char peek() const { return atEnd() ? '\0' : text_[position_]; }

    void skipSpace()
    {
        while (!atEnd()) {
            const char c = text_[position_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++position_;
        }
    }

    bool literal(const char *word)
    {
        for (size_t index = 0; word[index] != '\0'; ++index) {
            if (peek() != word[index]) {
                return fail("Unknown word");
            }
            ++position_;
        }
        return true;
    }

    bool value(Value &out, int depth)
    {
        if (depth > maxDepth_) {
            return fail("Nested too deeply");
        }
        switch (peek()) {
        case '{':
            return object(out, depth);
        case '[':
            return array(out, depth);
        case '"':
            out.type = Type::String;
            return string(out.string);
        case 't':
            out.type = Type::Bool;
            out.boolean = true;
            return literal("true");
        case 'f':
            out.type = Type::Bool;
            out.boolean = false;
            return literal("false");
        case 'n':
            out.type = Type::Null;
            return literal("null");
        default:
            return number(out);
        }
    }

    bool object(Value &out, int depth)
    {
        out.type = Type::Object;
        ++position_; // {
        skipSpace();
        if (peek() == '}') {
            ++position_;
            return true;
        }
        while (true) {
            skipSpace();
            if (peek() != '"') {
                return fail("Expected a field name");
            }
            std::string key;
            if (!string(key)) {
                return false;
            }
            skipSpace();
            if (peek() != ':') {
                return fail("Expected ':'");
            }
            ++position_;
            skipSpace();
            out.fields.emplace_back(std::move(key), Value());
            if (!value(out.fields.back().second, depth + 1)) {
                return false;
            }
            skipSpace();
            if (peek() == ',') {
                ++position_;
                continue;
            }
            if (peek() == '}') {
                ++position_;
                return true;
            }
            return fail("Expected ',' or '}'");
        }
    }

    bool array(Value &out, int depth)
    {
        out.type = Type::Array;
        ++position_; // [
        skipSpace();
        if (peek() == ']') {
            ++position_;
            return true;
        }
        while (true) {
            skipSpace();
            out.items.emplace_back();
            if (!value(out.items.back(), depth + 1)) {
                return false;
            }
            skipSpace();
            if (peek() == ',') {
                ++position_;
                continue;
            }
            if (peek() == ']') {
                ++position_;
                return true;
            }
            return fail("Expected ',' or ']'");
        }
    }

    bool hex4(uint32_t &out)
    {
        if (position_ + 4 > text_.size()) {
            return fail("Short \\u escape");
        }
        out = 0;
        for (int index = 0; index < 4; ++index) {
            const char c = text_[position_++];
            out <<= 4;
            if (c >= '0' && c <= '9') {
                out |= static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                out |= static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                out |= static_cast<uint32_t>(c - 'A' + 10);
            } else {
                return fail("Bad \\u escape");
            }
        }
        return true;
    }

    static void appendUtf8(std::string &out, uint32_t code)
    {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    bool string(std::string &out)
    {
        ++position_; // opening quote
        while (true) {
            if (atEnd()) {
                return fail("Unterminated string");
            }
            const char c = text_[position_++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail("Control character in a string");
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (atEnd()) {
                return fail("Unterminated escape");
            }
            const char escape = text_[position_++];
            switch (escape) {
            case '"':
            case '\\':
            case '/':
                out += escape;
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                uint32_t code = 0;
                if (!hex4(code)) {
                    return false;
                }
                // A surrogate pair makes one character beyond U+FFFF; a lone
                // half becomes U+FFFD.
                if (code >= 0xD800 && code <= 0xDBFF &&
                    text_.compare(position_, 2, "\\u") == 0) {
                    const size_t mark = position_;
                    position_ += 2;
                    uint32_t low = 0;
                    if (!hex4(low)) {
                        return false;
                    }
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    } else {
                        position_ = mark;
                        code = 0xFFFD;
                    }
                } else if (code >= 0xD800 && code <= 0xDFFF) {
                    code = 0xFFFD;
                }
                appendUtf8(out, code);
                break;
            }
            default:
                return fail("Unknown escape");
            }
        }
    }

    bool number(Value &out)
    {
        const size_t start = position_;
        if (peek() == '-') {
            ++position_;
        }
        if (!(peek() >= '0' && peek() <= '9')) {
            return fail("Unexpected character");
        }
        while ((peek() >= '0' && peek() <= '9') || peek() == '.' ||
               peek() == 'e' || peek() == 'E' || peek() == '+' ||
               peek() == '-') {
            ++position_;
        }
        const std::string digits = text_.substr(start, position_ - start);
        char *end = nullptr;
        const double parsed = std::strtod(digits.c_str(), &end);
        if (end == nullptr || *end != '\0' || !std::isfinite(parsed)) {
            position_ = start;
            return fail("Bad number");
        }
        out.type = Type::Number;
        out.number = parsed;
        return true;
    }
};

const Value &nullValue()
{
    static const Value value;
    return value;
}

} // namespace

const Value &Value::operator[](const std::string &key) const
{
    if (type == Type::Object) {
        for (const auto &field : fields) {
            if (field.first == key) {
                return field.second;
            }
        }
    }
    return nullValue();
}

std::string Value::text(const std::string &fallback) const
{
    return type == Type::String ? string : fallback;
}

int64_t Value::integer(int64_t fallback) const
{
    if (type != Type::Number || number < -9.2e18 || number > 9.2e18) {
        return fallback;
    }
    return static_cast<int64_t>(number);
}

bool parse(const std::string &text, Value &out, std::string &error,
           int maxDepth)
{
    out = Value();
    Parser parser(text, maxDepth);
    return parser.document(out, error);
}

} // namespace json
