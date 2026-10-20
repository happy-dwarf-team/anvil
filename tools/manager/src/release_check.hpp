#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace anvil::manager {

// The opt-in release check (design §15): the Manager asks GitHub for the
// latest release and, when it is newer, shows a link. Nothing is downloaded;
// updating Anvil means running a newer Manager.
inline constexpr const wchar_t* kReleaseApiHost = L"api.github.com";
inline constexpr const wchar_t* kReleaseApiPath = L"/repos/happy-dwarf-team/anvil/releases/latest";
inline constexpr const char* kReleasesUrl = "https://github.com/happy-dwarf-team/anvil/releases";

struct Release {
    std::string tag;  // "v1.2.0"
    std::string url;  // its page on GitHub
};

// tag_name and html_url of a GitHub release object (top-level keys only).
// nullopt when the text is not such an object, or the URL is not a page of
// the anvil repository on github.com (the Manager opens only those).
std::optional<Release> parse_release(std::string_view json);

// True when `tag` ("v1.2.0" or "1.2.0") is a newer version than `current`.
// A tag that is not a version is never newer.
bool newer_release(std::string_view tag, std::string_view current);

}  // namespace anvil::manager
