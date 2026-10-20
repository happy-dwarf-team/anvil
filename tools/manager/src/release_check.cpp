#include "release_check.hpp"

#include "core/json_scan.hpp"
#include "core/semver.hpp"

namespace anvil::manager {

using anvil::JsonScanner;


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
