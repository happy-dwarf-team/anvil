#include "release_check.hpp"

#include "core/semver.hpp"

namespace anvil::manager {
namespace {

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

    // A string; \uXXXX outside ASCII is kept as '?' (only URLs and tags are read).
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
                if (at_ + 4 > text_.size()) return std::nullopt;
                unsigned code = 0;
                for (int i = 0; i < 4; ++i) {
                    const char h = text_[at_++];
                    code <<= 4;
                    if (h >= '0' && h <= '9') code |= h - '0';
                    else if (h >= 'a' && h <= 'f') code |= h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') code |= h - 'A' + 10;
                    else return std::nullopt;
                }
                out += code < 0x80 ? static_cast<char>(code) : '?';
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
    std::string_view text_;
    size_t at_ = 0;
};

}  // namespace

std::optional<Release> parse_release(std::string_view json) {
    JsonScanner scan(json);
    if (!scan.eat('{')) return std::nullopt;
    Release release;
    if (!scan.peek('}')) {
        do {
            const auto key = scan.string();
            if (!key || !scan.eat(':')) return std::nullopt;
            if ((*key == "tag_name" || *key == "html_url") && scan.peek('"')) {
                const auto value = scan.string();
                if (!value) return std::nullopt;
                (*key == "tag_name" ? release.tag : release.url) = *value;
            } else if (!scan.skip_value()) {
                return std::nullopt;
            }
        } while (scan.eat(','));
    }
    if (!scan.eat('}') || !scan.at_end()) return std::nullopt;
    if (release.tag.empty() || !release.url.starts_with(std::string(kReleasesUrl) + "/")) return std::nullopt;
    for (const char c : release.url) {
        if (static_cast<unsigned char>(c) <= 0x20 || c == '"' || c == '<' || c == '>' || c == '\\' || c == '`') {
            return std::nullopt;
        }
    }
    return release;
}

bool newer_release(std::string_view tag, std::string_view current) {
    if (!tag.empty() && tag.front() == 'v') tag.remove_prefix(1);
    const auto latest = Version::parse(tag);
    const auto own = Version::parse(current);
    return latest && own && *own < *latest;
}

}  // namespace anvil::manager
