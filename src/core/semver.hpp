#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil {

// SemVer 2.0 version (build metadata is accepted and ignored for ordering).
struct Version {
    unsigned major = 0;
    unsigned minor = 0;
    unsigned patch = 0;
    std::string prerelease;  // empty = release

    static std::optional<Version> parse(std::string_view text);
    std::string to_string() const;
};

int compare(const Version& a, const Version& b);
inline bool operator<(const Version& a, const Version& b) { return compare(a, b) < 0; }
inline bool operator==(const Version& a, const Version& b) { return compare(a, b) == 0; }

// Version requirement: comparators joined by ',' (all must hold), e.g.
// ">=1.2.0, <2", "=1.0.0", "1.0.0" (same as "="), "^1.2" (>=1.2.0 <2.0.0),
// "~1.2" (>=1.2.0 <1.3.0), "*" (any). Missing minor/patch count as 0.
class VersionRange {
public:
    static std::optional<VersionRange> parse(std::string_view text);
    bool matches(const Version& version) const;
    const std::string& text() const { return text_; }

private:
    enum class Op { Eq, Lt, Le, Gt, Ge };
    struct Comparator {
        Op op;
        Version version;
    };
    std::vector<Comparator> comparators_;
    std::string text_;
};

}  // namespace anvil
