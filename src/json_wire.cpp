#include "json_wire.h"

#include <cctype>

namespace doomnet {

namespace {

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    bool parse(Json& out, std::string& error) {
        skip();
        if (!value(out, 0, error)) {
            return false;
        }
        skip();
        if (at_ < text_.size()) {
            error = "trailing data after the JSON value";
            return false;
        }
        return true;
    }

private:
    const std::string& text_;
    size_t at_ = 0;

    void skip() {
        while (at_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[at_]))) {
            ++at_;
        }
    }

    bool value(Json& out, int depth, std::string& error) {
        if (depth > 32) {
            error = "JSON nested too deeply";
            return false;
        }
        skip();
        if (at_ >= text_.size()) {
            error = "JSON ended early";
            return false;
        }
        const char c = text_[at_];
        if (c == '{') {
            return object(out, depth, error);
        }
        if (c == '[') {
            return array(out, depth, error);
        }
        if (c == '"') {
            out.type = Json::Type::String;
            return string(out.s, error);
        }
        if (c == 't' || c == 'f') {
            return boolean(out, error);
        }
        if (c == 'n') {
            return null(out, error);
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return number(out, error);
        }
        error = "JSON has an unexpected character";
        return false;
    }

    bool object(Json& out, int depth, std::string& error) {
        ++at_;
        out.type = Json::Type::Object;
        skip();
        if (at_ < text_.size() && text_[at_] == '}') {
            ++at_;
            return true;
        }
        while (at_ < text_.size()) {
            skip();
            std::string key;
            if (!string(key, error)) {
                return false;
            }
            skip();
            if (at_ >= text_.size() || text_[at_] != ':') {
                error = "JSON object is missing a colon";
                return false;
            }
            ++at_;
            Json child;
            if (!value(child, depth + 1, error)) {
                return false;
            }
            out.o.emplace_back(std::move(key), std::move(child));
            skip();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            if (at_ < text_.size() && text_[at_] == '}') {
                ++at_;
                return true;
            }
            error = "JSON object did not end";
            return false;
        }
        error = "JSON object ended early";
        return false;
    }

    bool array(Json& out, int depth, std::string& error) {
        ++at_;
        out.type = Json::Type::Array;
        skip();
        if (at_ < text_.size() && text_[at_] == ']') {
            ++at_;
            return true;
        }
        while (at_ < text_.size()) {
            Json child;
            if (!value(child, depth + 1, error)) {
                return false;
            }
            out.a.push_back(std::move(child));
            skip();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            if (at_ < text_.size() && text_[at_] == ']') {
                ++at_;
                return true;
            }
            error = "JSON array did not end";
            return false;
        }
        error = "JSON array ended early";
        return false;
    }

    bool string(std::string& out, std::string& error) {
        if (at_ >= text_.size() || text_[at_] != '"') {
            error = "JSON expected a string";
            return false;
        }
        ++at_;
        out.clear();
        while (at_ < text_.size()) {
            const char c = text_[at_++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (at_ >= text_.size()) {
                error = "JSON string ended in an escape";
                return false;
            }
            const char e = text_[at_++];
            switch (e) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(e);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u':
                    if (!unicode(out, error)) {
                        return false;
                    }
                    break;
                default:
                    error = "JSON string has an unknown escape";
                    return false;
            }
        }
        error = "JSON string did not end";
        return false;
    }

    bool unicode(std::string& out, std::string& error) {
        uint32_t cp = 0;
        if (!hex4(cp, error)) {
            return false;
        }
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (at_ + 2 <= text_.size() && text_[at_] == '\\' && text_[at_ + 1] == 'u') {
                at_ += 2;
                uint32_t low = 0;
                if (!hex4(low, error)) {
                    return false;
                }
                if (low < 0xDC00 || low > 0xDFFF) {
                    error = "JSON string has a broken surrogate pair";
                    return false;
                }
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            }
        }
        append_utf8(out, cp);
        return true;
    }

    bool hex4(uint32_t& out, std::string& error) {
        if (at_ + 4 > text_.size()) {
            error = "JSON unicode escape is short";
            return false;
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[at_++];
            out <<= 4;
            if (c >= '0' && c <= '9') {
                out += static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                out += static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                out += static_cast<uint32_t>(c - 'A' + 10);
            } else {
                error = "JSON unicode escape is not hex";
                return false;
            }
        }
        return true;
    }

    static void append_utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool boolean(Json& out, std::string& error) {
        if (text_.compare(at_, 4, "true") == 0) {
            at_ += 4;
            out.type = Json::Type::Bool;
            out.b = true;
            return true;
        }
        if (text_.compare(at_, 5, "false") == 0) {
            at_ += 5;
            out.type = Json::Type::Bool;
            out.b = false;
            return true;
        }
        error = "JSON expected true or false";
        return false;
    }

    bool null(Json& out, std::string& error) {
        if (text_.compare(at_, 4, "null") != 0) {
            error = "JSON expected null";
            return false;
        }
        at_ += 4;
        out.type = Json::Type::Null;
        return true;
    }

    bool number(Json& out, std::string& error) {
        const size_t start = at_;
        if (text_[at_] == '-') {
            ++at_;
        }
        if (at_ >= text_.size() || text_[at_] < '0' || text_[at_] > '9') {
            error = "JSON number is incomplete";
            return false;
        }
        while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
            ++at_;
        }
        if (at_ < text_.size() && text_[at_] == '.') {
            ++at_;
            if (at_ >= text_.size() || text_[at_] < '0' || text_[at_] > '9') {
                error = "JSON number is incomplete";
                return false;
            }
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
                ++at_;
            }
        }
        if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) {
            ++at_;
            if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-')) {
                ++at_;
            }
            if (at_ >= text_.size() || text_[at_] < '0' || text_[at_] > '9') {
                error = "JSON number is incomplete";
                return false;
            }
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
                ++at_;
            }
        }
        try {
            out.n = std::stod(text_.substr(start, at_ - start));
        } catch (...) {
            error = "JSON number could not be read";
            return false;
        }
        out.type = Json::Type::Number;
        return true;
    }
};

}  // namespace

const Json* Json::find(const char* key) const {
    for (const auto& entry : o) {
        if (entry.first == key) {
            return &entry.second;
        }
    }
    return nullptr;
}

std::string Json::key_string(const char* key) const {
    const Json* child = find(key);
    return child != nullptr && child->type == Type::String ? child->s : std::string();
}

bool Json::key_bool(const char* key) const {
    const Json* child = find(key);
    return child != nullptr && child->type == Type::Bool && child->b;
}

bool json_parse(const std::string& text, Json& out, std::string& error) {
    out = Json{};
    error.clear();
    return Parser(text).parse(out, error);
}

std::string json_quote(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (unsigned char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    constexpr char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(kHex[c >> 4]);
                    out.push_back(kHex[c & 0xF]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace doomnet
