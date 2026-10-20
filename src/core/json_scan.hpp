#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace anvil {

// A small reader for the JSON files Anvil looks into (the game's
// dlc_load.json, a GitHub release): strings in full, every other value only
// skipped. Not a validator; the callers check the shape they need.
class JsonScanner {
public:
    explicit JsonScanner(std::string_view text) : text_(text) {}

    void skip_space() {
        while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\r' || text_[at_] == '\n')) ++at_;
    }
    bool eat(char c) {
        skip_space();
        if (at_ < text_.size() && text_[at_] == c) {
            ++at_;
            return true;
        }
        return false;
    }
    bool peek(char c) {
        skip_space();
        return at_ < text_.size() && text_[at_] == c;
    }

    // A string as UTF-8; \uXXXX escapes (with surrogate pairs) are encoded,
    // a lone surrogate is an error.
    std::optional<std::string> string() {
        if (!eat('"')) return std::nullopt;
        std::string out;
        while (at_ < text_.size()) {
            const char c = text_[at_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) return std::nullopt;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_ >= text_.size()) return std::nullopt;
            const char e = text_[at_++];
            switch (e) {
            case '"': case '\\': case '/': out += e; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                auto code = hex4();
                if (!code) return std::nullopt;
                if (*code >= 0xDC00 && *code <= 0xDFFF) return std::nullopt;
                if (*code >= 0xD800 && *code <= 0xDBFF) {
                    if (at_ + 2 > text_.size() || text_[at_] != '\\' || text_[at_ + 1] != 'u') return std::nullopt;
                    at_ += 2;
                    const auto low = hex4();
                    if (!low || *low < 0xDC00 || *low > 0xDFFF) return std::nullopt;
                    *code = 0x10000 + ((*code - 0xD800) << 10) + (*low - 0xDC00);
                }
                append_utf8(out, *code);
                break;
            }
            default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    // Any value, skipped.
    bool skip_value(int depth = 0) {
        skip_space();
        if (at_ >= text_.size() || depth > 64) return false;
        const char c = text_[at_];
        if (c == '"') return string().has_value();
        if (c == '{' || c == '[') {
            const char close = c == '{' ? '}' : ']';
            ++at_;
            if (eat(close)) return true;
            do {
                if (c == '{' && (!string() || !eat(':'))) return false;
                if (!skip_value(depth + 1)) return false;
            } while (eat(','));
            return eat(close);
        }
        const size_t start = at_;
        while (at_ < text_.size() && std::string_view(",}] \t\r\n").find(text_[at_]) == std::string_view::npos) ++at_;
        return at_ > start;
    }

    bool at_end() {
        skip_space();
        return at_ == text_.size();
    }

private:
    std::optional<uint32_t> hex4() {
        if (at_ + 4 > text_.size()) return std::nullopt;
        uint32_t code = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = text_[at_++];
            code <<= 4;
            if (h >= '0' && h <= '9') code |= h - '0';
            else if (h >= 'a' && h <= 'f') code |= h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') code |= h - 'A' + 10;
            else return std::nullopt;
        }
        return code;
    }

    static void append_utf8(std::string& out, uint32_t code) {
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

    std::string_view text_;
    size_t at_ = 0;
};

}  // namespace anvil
