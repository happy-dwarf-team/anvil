#include "files.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

namespace model_reload {

namespace fs = std::filesystem;

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

namespace {

bool starts_with(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }
bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

std::string utf8(const fs::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

fs::path from_utf8(std::string_view s) { return fs::path(std::u8string(s.begin(), s.end())); }

bool read_file(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// The strings of the "enabled_mods" array of dlc_load.json.
std::vector<std::string> enabled_entries(const std::string& json) {
    std::vector<std::string> out;
    std::size_t at = json.find("\"enabled_mods\"");
    if (at == std::string::npos) return out;
    at = json.find('[', at);
    if (at == std::string::npos) return out;
    for (++at; at < json.size() && json[at] != ']'; ++at) {
        if (json[at] != '"') continue;
        std::string s;
        for (++at; at < json.size() && json[at] != '"'; ++at) {
            if (json[at] == '\\' && at + 1 < json.size()) ++at;
            s += json[at];
        }
        out.push_back(std::move(s));
    }
    return out;
}

// `key="value"` at the top level of a .mod descriptor.
std::string descriptor_value(const std::string& text, std::string_view key) {
    std::istringstream in(text);
    std::string line;
    int depth = 0;
    while (std::getline(in, line)) {
        std::size_t i = 0;
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (depth == 0 && line.compare(i, key.size(), key) == 0) {
            std::size_t j = i + key.size();
            while (j < line.size() && std::isspace(static_cast<unsigned char>(line[j]))) ++j;
            if (j < line.size() && line[j] == '=') {
                ++j;
                while (j < line.size() && std::isspace(static_cast<unsigned char>(line[j]))) ++j;
                if (j < line.size() && line[j] == '"') {
                    const std::size_t end = line.find('"', j + 1);
                    if (end != std::string::npos) return line.substr(j + 1, end - j - 1);
                }
            }
        }
        for (const char c : line) {
            if (c == '#') break;
            if (c == '{') ++depth;
            if (c == '}') --depth;
        }
    }
    return {};
}

}  // namespace

std::optional<Kind> classify(std::string_view relative) {
    const std::string p = lower(relative);
    const bool texture = ends_with(p, ".dds") || ends_with(p, ".tga");
    if (starts_with(p, "interface/")) {
        if (ends_with(p, ".gfx")) return Kind::gfx;
        return std::nullopt;
    }
    if (!starts_with(p, "gfx/")) return std::nullopt;
    if (ends_with(p, ".mesh")) return Kind::mesh;
    if (ends_with(p, ".anim")) return Kind::animation;
    if (ends_with(p, ".asset")) return Kind::asset;
    if (texture && starts_with(p, "gfx/models/")) return Kind::model_texture;
    if (texture && starts_with(p, "gfx/particles/")) return Kind::particle_texture;
    return std::nullopt;
}

Mods enabled_mods(const fs::path& user_dir) {
    Mods out;
    std::string json;
    if (!read_file(user_dir / L"dlc_load.json", json)) {
        out.problems.push_back("no dlc_load.json in the user folder: no mods to watch");
        return out;
    }
    for (const std::string& entry : enabled_entries(json)) {
        const fs::path descriptor = user_dir / from_utf8(entry);
        std::string text;
        if (!read_file(descriptor, text)) {
            out.problems.push_back("the descriptor " + entry + " cannot be read");
            continue;
        }
        std::string name = descriptor_value(text, "name");
        if (name.empty()) name = entry;
        std::string path = descriptor_value(text, "path");
        if (path.empty()) {
            out.problems.push_back(name + " is an archive (" + entry + "): it cannot be watched");
            continue;
        }
        fs::path root = from_utf8(path);
        if (root.is_relative()) root = user_dir / root;
        std::error_code ec;
        if (!fs::is_directory(root, ec)) {
            out.problems.push_back("the folder of " + name + " does not exist: " + utf8(root));
            continue;
        }
        out.folders.push_back(ModFolder{std::move(name), std::move(root)});
    }
    return out;
}

Snapshot scan(const std::vector<ModFolder>& mods, std::size_t limit, bool* cut) {
    Snapshot out;
    if (cut != nullptr) *cut = false;
    static const wchar_t* const kFolders[] = {L"gfx/models", L"gfx/entities", L"gfx/particles", L"interface"};
    for (std::size_t m = 0; m < mods.size(); ++m) {
        for (const wchar_t* sub : kFolders) {
            const fs::path base = mods[m].root / sub;
            std::error_code ec;
            if (!fs::is_directory(base, ec)) continue;
            fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec);
            for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                std::error_code fe;
                if (!it->is_regular_file(fe)) continue;
                const std::string relative = utf8(fs::relative(it->path(), mods[m].root, fe));
                if (fe || !classify(relative)) continue;
                if (out.size() >= limit) {
                    if (cut != nullptr) *cut = true;
                    return out;
                }
                FileStamp stamp;
                stamp.size = it->file_size(fe);
                stamp.time = it->last_write_time(fe).time_since_epoch().count();
                out.emplace(FileKey{m, relative}, stamp);
            }
        }
    }
    return out;
}

std::vector<Change> changes(const Snapshot& before, const Snapshot& now) {
    std::vector<Change> out;
    for (const auto& [key, stamp] : now) {
        const auto old = before.find(key);
        if (old != before.end() && old->second == stamp) continue;
        if (const auto kind = classify(key.relative)) out.push_back(Change{key, *kind});
    }
    std::stable_sort(out.begin(), out.end(), [](const Change& a, const Change& b) {
        if (a.kind != b.kind) return a.kind < b.kind;
        return lower(a.key.relative) < lower(b.key.relative);
    });
    return out;
}

std::vector<Step> plan(const std::vector<Change>& list, const Snapshot& all) {
    std::vector<Step> out;
    bool anim = false, particle = false, assets = false;
    for (const Change& c : list) {
        anim |= c.kind == Kind::animation;
        particle |= c.kind == Kind::particle_texture;
        assets |= c.kind == Kind::asset;
    }
    auto add_each = [&](Kind kind, Step::What what) {
        std::vector<std::string> seen;  // one step per path, also when two mods have it
        for (const Change& c : list) {
            if (c.kind != kind) continue;
            const std::string key = lower(c.key.relative);
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
            seen.push_back(key);
            out.push_back(Step{what, c.key.relative, c.key.mod});
        }
    };
    add_each(Kind::model_texture, Step::What::reload_texture);
    add_each(Kind::mesh, Step::What::reload_mesh);
    if (anim) out.push_back(Step{Step::What::reload_anim, {}, 0});
    if (particle) out.push_back(Step{Step::What::reload_particle, {}, 0});
    if (assets) out.push_back(Step{Step::What::reload_assets, {}, 0});
    // A .gfx of one path in two mods: the game reads only the copy of the
    // later mod in the launcher's order, so an earlier mod's copy is skipped.
    for (const Change& c : list) {
        if (c.kind != Kind::gfx) continue;
        bool later = false;
        for (const auto& [key, stamp] : all) {
            if (key.mod > c.key.mod && iequals(key.relative, c.key.relative)) later = true;
        }
        if (!later) out.push_back(Step{Step::What::reload_gfx, c.key.relative, c.key.mod});
    }
    return out;
}

}  // namespace model_reload
