#include "semver.hpp"

#include <cctype>
#include <charconv>

namespace anvil {
namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}

bool parse_number(std::string_view text, unsigned& out) {
    if (text.empty() || (text.size() > 1 && text[0] == '0')) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

// "1", "1.2", "1.2.3" (partial allowed for ranges)
std::optional<Version> parse_partial(std::string_view text, int& parts) {
    Version v;
    parts = 0;
    unsigned* fields[] = {&v.major, &v.minor, &v.patch};
    while (!text.empty() && parts < 3) {
        const std::size_t dot = text.find('.');
        const std::string_view piece = text.substr(0, dot);
        if (!parse_number(piece, *fields[parts])) return std::nullopt;
        ++parts;
        if (dot == std::string_view::npos) {
            text = {};
            break;
        }
        text.remove_prefix(dot + 1);
    }
    if (!text.empty() || parts == 0) return std::nullopt;
    return v;
}

int compare_prerelease(const std::string& a, const std::string& b) {
    if (a == b) return 0;
    if (a.empty()) return 1;  // release > prerelease
    if (b.empty()) return -1;
    std::string_view x = a, y = b;
    while (!x.empty() && !y.empty()) {
        const std::size_t dx = x.find('.'), dy = y.find('.');
        const std::string_view px = x.substr(0, dx), py = y.substr(0, dy);
        unsigned nx = 0, ny = 0;
        const bool numx = parse_number(px, nx), numy = parse_number(py, ny);
        int c = 0;
        if (numx && numy) c = nx < ny ? -1 : (nx > ny ? 1 : 0);
        else if (numx) c = -1;
        else if (numy) c = 1;
        else c = px < py ? -1 : (px > py ? 1 : 0);
        if (c != 0) return c;
        x = dx == std::string_view::npos ? std::string_view{} : x.substr(dx + 1);
        y = dy == std::string_view::npos ? std::string_view{} : y.substr(dy + 1);
    }
    return x.empty() ? (y.empty() ? 0 : -1) : 1;
}

}  // namespace

std::optional<Version> Version::parse(std::string_view text) {
    text = trim(text);
    const std::size_t plus = text.find('+');
    if (plus != std::string_view::npos) text = text.substr(0, plus);
    std::string_view pre;
    const std::size_t dash = text.find('-');
    if (dash != std::string_view::npos) {
        pre = text.substr(dash + 1);
        text = text.substr(0, dash);
        if (pre.empty()) return std::nullopt;
    }
    int parts = 0;
    auto v = parse_partial(text, parts);
    if (!v || parts != 3) return std::nullopt;
    v->prerelease = std::string(pre);
    return v;
}

std::string Version::to_string() const {
    std::string s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    if (!prerelease.empty()) s += "-" + prerelease;
    return s;
}

int compare(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return compare_prerelease(a.prerelease, b.prerelease);
}

std::optional<VersionRange> VersionRange::parse(std::string_view text) {
    VersionRange range;
    range.text_ = std::string(trim(text));
    std::string_view rest = range.text_;
    if (rest == "*" || rest.empty()) {
        return range;  // no comparators = any version
    }
    while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        std::string_view item = trim(rest.substr(0, comma));
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        if (item.empty()) return std::nullopt;
        Op op = Op::Eq;
        char sugar = 0;
        if (item.starts_with(">=")) { op = Op::Ge; item.remove_prefix(2); }
        else if (item.starts_with("<=")) { op = Op::Le; item.remove_prefix(2); }
        else if (item.starts_with(">")) { op = Op::Gt; item.remove_prefix(1); }
        else if (item.starts_with("<")) { op = Op::Lt; item.remove_prefix(1); }
        else if (item.starts_with("=")) { op = Op::Eq; item.remove_prefix(1); }
        else if (item.starts_with("^") || item.starts_with("~")) { sugar = item[0]; item.remove_prefix(1); }
        item = trim(item);
        int parts = 0;
        auto base = parse_partial(item, parts);
        if (!base) return std::nullopt;
        if (sugar != 0) {
            Version upper = *base;
            if (sugar == '^') {
                if (base->major > 0) upper = {base->major + 1, 0, 0, {}};
                else if (parts >= 2 && base->minor > 0) upper = {0, base->minor + 1, 0, {}};
                else upper = {0, 0, base->patch + 1, {}};
            } else {
                upper = parts >= 2 ? Version{base->major, base->minor + 1, 0, {}} : Version{base->major + 1, 0, 0, {}};
            }
            range.comparators_.push_back({Op::Ge, *base});
            range.comparators_.push_back({Op::Lt, upper});
        } else if (op == Op::Eq && parts < 3) {
            // "=1.2" means any 1.2.x
            const Version upper = parts == 1 ? Version{base->major + 1, 0, 0, {}} : Version{base->major, base->minor + 1, 0, {}};
            range.comparators_.push_back({Op::Ge, *base});
            range.comparators_.push_back({Op::Lt, upper});
        } else {
            range.comparators_.push_back({op, *base});
        }
    }
    return range;
}

bool VersionRange::matches(const Version& version) const {
    for (const auto& c : comparators_) {
        const int r = compare(version, c.version);
        bool ok = false;
        switch (c.op) {
        case Op::Eq: ok = r == 0; break;
        case Op::Lt: ok = r < 0; break;
        case Op::Le: ok = r <= 0; break;
        case Op::Gt: ok = r > 0; break;
        case Op::Ge: ok = r >= 0; break;
        }
        if (!ok) return false;
    }
    return true;
}

}  // namespace anvil
