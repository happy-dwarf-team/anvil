#include "json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace anvil::manager {
namespace {

const Json kNull;

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    std::optional<Json> document(std::string& error) {
        auto value = parse(0);
        if (value) {
            space();
            if (at_ != text_.size()) fail("unexpected text after the value");
        }
        if (!error_.empty()) {
            error = error_ + " at byte " + std::to_string(at_);
            return std::nullopt;
        }
        return value;
    }

private:
    void fail(const char* why) {
        if (error_.empty()) error_ = why;
    }

    void space() {
        while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\r' || text_[at_] == '\n')) ++at_;
    }

    bool literal(std::string_view word) {
        if (text_.substr(at_, word.size()) != word) return false;
        at_ += word.size();
        return true;
    }

    std::optional<Json> parse(int depth) {
        space();
        if (depth > 64) {
            fail("nested too deeply");
            return std::nullopt;
        }
        if (at_ >= text_.size()) {
            fail("unexpected end");
            return std::nullopt;
        }
        const char c = text_[at_];
        if (c == '{') return object(depth);
        if (c == '[') return array(depth);
        if (c == '"') {
            auto s = string();
            if (!s) return std::nullopt;
            return Json(std::move(*s));
        }
        if (literal("true")) return Json(true);
        if (literal("false")) return Json(false);
        if (literal("null")) return Json();
        return number();
    }

    std::optional<Json> object(int depth) {
        ++at_;
        Json out = Json::object();
        space();
        if (at_ < text_.size() && text_[at_] == '}') {
            ++at_;
            return out;
        }
        while (true) {
            space();
            auto key = string();
            if (!key) return std::nullopt;
            space();
            if (at_ >= text_.size() || text_[at_] != ':') {
                fail("expected ':'");
                return std::nullopt;
            }
            ++at_;
            auto value = parse(depth + 1);
            if (!value) return std::nullopt;
            if (out.find(*key) != nullptr) {
                fail("a key appears twice");
                return std::nullopt;
            }
            out.set(std::move(*key), std::move(*value));
            space();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            if (at_ < text_.size() && text_[at_] == '}') {
                ++at_;
                return out;
            }
            fail("expected ',' or '}'");
            return std::nullopt;
        }
    }

    std::optional<Json> array(int depth) {
        ++at_;
        Json out = Json::array();
        space();
        if (at_ < text_.size() && text_[at_] == ']') {
            ++at_;
            return out;
        }
        while (true) {
            auto value = parse(depth + 1);
            if (!value) return std::nullopt;
            out.push(std::move(*value));
            space();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            if (at_ < text_.size() && text_[at_] == ']') {
                ++at_;
                return out;
            }
            fail("expected ',' or ']'");
            return std::nullopt;
        }
    }

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

    std::optional<std::string> string() {
        if (at_ >= text_.size() || text_[at_] != '"') {
            fail("expected a string");
            return std::nullopt;
        }
        ++at_;
        std::string out;
        while (at_ < text_.size()) {
            const char c = text_[at_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) break;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_ >= text_.size()) break;
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
                if (!code || (*code >= 0xDC00 && *code <= 0xDFFF)) {
                    fail("a bad \\u escape");
                    return std::nullopt;
                }
                if (*code >= 0xD800 && *code <= 0xDBFF) {
                    if (at_ + 2 > text_.size() || text_[at_] != '\\' || text_[at_ + 1] != 'u') {
                        fail("a lone surrogate");
                        return std::nullopt;
                    }
                    at_ += 2;
                    const auto low = hex4();
                    if (!low || *low < 0xDC00 || *low > 0xDFFF) {
                        fail("a lone surrogate");
                        return std::nullopt;
                    }
                    *code = 0x10000 + ((*code - 0xD800) << 10) + (*low - 0xDC00);
                }
                append_utf8(out, *code);
                break;
            }
            default: fail("a bad escape"); return std::nullopt;
            }
        }
        fail("a string that does not end");
        return std::nullopt;
    }

    std::optional<Json> number() {
        const size_t start = at_;
        if (at_ < text_.size() && text_[at_] == '-') ++at_;
        const auto digits = [&] {
            const size_t from = at_;
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
            return at_ - from;
        };
        const size_t int_start = at_;
        const size_t int_digits = digits();
        if (int_digits == 0 || (int_digits > 1 && text_[int_start] == '0')) {
            fail("expected a value");
            return std::nullopt;
        }
        bool integral = true;
        if (at_ < text_.size() && text_[at_] == '.') {
            ++at_;
            integral = false;
            if (digits() == 0) {
                fail("a bad number");
                return std::nullopt;
            }
        }
        if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) {
            ++at_;
            integral = false;
            if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-')) ++at_;
            if (digits() == 0) {
                fail("a bad number");
                return std::nullopt;
            }
        }
        const char* first = text_.data() + start;
        const char* last = text_.data() + at_;
        if (integral) {
            int64_t value = 0;
            if (std::from_chars(first, last, value).ec == std::errc()) return Json(value);
        }
        double value = 0;
        if (std::from_chars(first, last, value).ec != std::errc() || !std::isfinite(value)) {
            fail("a number out of range");
            return std::nullopt;
        }
        return Json(value);
    }

    std::string_view text_;
    size_t at_ = 0;
    std::string error_;
};

void put_string(std::string& out, const std::string& text) {
    out += '"';
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (u < 0x20 || u == 0x7F) {
            char escape[8];
            std::snprintf(escape, sizeof(escape), "\\u%04x", u);
            out += escape;
        } else {
            out += c;
        }
    }
    out += '"';
}

bool is_scalar(const Json& v) { return !v.is_array() && !v.is_object(); }

void put(std::string& out, const Json& v, bool pretty, int depth);

// An array or object of scalars on one line, when it fits.
bool put_inline(std::string& out, const Json& v, int depth) {
    std::string line;
    if (v.is_array()) {
        for (const Json& item : v.items()) {
            if (!is_scalar(item)) return false;
        }
        line += '[';
        for (size_t i = 0; i < v.items().size(); ++i) {
            if (i) line += ", ";
            put(line, v.items()[i], false, 0);
        }
        line += ']';
    } else {
        for (const auto& [key, value] : v.members()) {
            if (!is_scalar(value)) return false;
        }
        line += "{ ";
        for (size_t i = 0; i < v.members().size(); ++i) {
            if (i) line += ", ";
            put_string(line, v.members()[i].first);
            line += ": ";
            put(line, v.members()[i].second, false, 0);
        }
        line += " }";
    }
    if (static_cast<size_t>(depth) * 2 + line.size() > 80) return false;
    out += line;
    return true;
}

void newline(std::string& out, int depth) {
    out += '\n';
    out.append(static_cast<size_t>(depth) * 2, ' ');
}

void put(std::string& out, const Json& v, bool pretty, int depth) {
    switch (v.type()) {
    case Json::Type::Null: out += "null"; return;
    case Json::Type::Bool: out += v.as_bool() ? "true" : "false"; return;
    case Json::Type::Number: {
        if (v.is_integer()) {
            out += std::to_string(v.as_integer());
        } else if (!std::isfinite(v.as_number())) {
            out += "null";
        } else {
            char text[32];
            const auto r = std::to_chars(text, text + sizeof(text), v.as_number());
            out.append(text, r.ptr);
        }
        return;
    }
    case Json::Type::String: put_string(out, v.as_string()); return;
    case Json::Type::Array:
        if (v.items().empty()) {
            out += "[]";
        } else if (!pretty || !put_inline(out, v, depth)) {
            out += '[';
            for (size_t i = 0; i < v.items().size(); ++i) {
                if (i) out += ',';
                if (pretty) newline(out, depth + 1);
                put(out, v.items()[i], pretty, depth + 1);
            }
            if (pretty) newline(out, depth);
            out += ']';
        }
        return;
    case Json::Type::Object:
        if (v.members().empty()) {
            out += "{}";
        } else if (!pretty || !put_inline(out, v, depth)) {
            out += '{';
            for (size_t i = 0; i < v.members().size(); ++i) {
                if (i) out += ',';
                if (pretty) newline(out, depth + 1);
                put_string(out, v.members()[i].first);
                out += pretty ? ": " : ":";
                put(out, v.members()[i].second, pretty, depth + 1);
            }
            if (pretty) newline(out, depth);
            out += '}';
        }
        return;
    }
}

}  // namespace

const Json* Json::find(std::string_view key) const {
    for (const Member& m : members_) {
        if (m.first == key) return &m.second;
    }
    return nullptr;
}

Json* Json::find(std::string_view key) {
    for (Member& m : members_) {
        if (m.first == key) return &m.second;
    }
    return nullptr;
}

const Json& Json::operator[](std::string_view key) const {
    const Json* found = find(key);
    return found != nullptr ? *found : kNull;
}

Json& Json::push(Json value) {
    items_.push_back(std::move(value));
    return items_.back();
}

Json& Json::set(std::string key, Json value) {
    if (Json* found = find(key)) {
        *found = std::move(value);
        return *found;
    }
    members_.emplace_back(std::move(key), std::move(value));
    return members_.back().second;
}

bool Json::operator==(const Json& other) const {
    if (type_ != other.type_) return false;
    switch (type_) {
    case Type::Null: return true;
    case Type::Bool: return bool_ == other.bool_;
    case Type::Number:
        return integral_ && other.integral_ ? integer_ == other.integer_ : number_ == other.number_;
    case Type::String: return string_ == other.string_;
    case Type::Array: return items_ == other.items_;
    case Type::Object: return members_ == other.members_;
    }
    return false;
}

std::optional<Json> parse_json(std::string_view text, std::string& error) { return Parser(text).document(error); }

std::string write_json(const Json& value, bool pretty) {
    std::string out;
    put(out, value, pretty, 0);
    if (pretty) out += '\n';
    return out;
}

}  // namespace anvil::manager
