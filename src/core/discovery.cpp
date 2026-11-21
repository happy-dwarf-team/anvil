#include "discovery.hpp"

#include <toml++/toml.hpp>

#include <algorithm>

namespace anvil {

namespace {

// A key of the wrong type: as a file that does not parse (K3, #537), so a
// list the loader cannot read never turns a plugin back on.
struct WrongType {
    std::string what;
    uint32_t line;
};

uint32_t line_of(const toml::node& node) { return static_cast<uint32_t>(node.source().begin.line); }

void read_ids(const toml::table& t, const char* key, std::vector<std::string>& out) {
    const toml::node* node = t.get(key);
    if (node == nullptr) return;
    const auto* list = node->as_array();
    if (list == nullptr) throw WrongType{std::string(key) + " is not a list of strings", line_of(*node)};
    for (const auto& item : *list) {
        const auto v = item.value<std::string>();
        if (!v) throw WrongType{std::string(key) + " has a value that is not a string", line_of(item)};
        out.push_back(*v);
    }
}

}  // namespace

bool module_enabled(const AnvilSettings& settings, const std::string& key, bool enabled_by_default) {
    const auto listed = [&](const std::vector<std::string>& ids) {
        return std::find(ids.begin(), ids.end(), key) != ids.end();
    };
    if (listed(settings.disabled_modules)) return false;
    if (listed(settings.enabled_modules)) return true;
    return enabled_by_default;
}

AnvilSettings read_anvil_settings(const std::filesystem::path& file) {
    AnvilSettings s;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return s;
    try {
        const toml::table t = toml::parse_file(file.wstring());
        if (const toml::node* enabled = t.get("enabled")) {
            if (!enabled->is_boolean()) throw WrongType{"enabled is not true or false", line_of(*enabled)};
            s.enabled = *enabled->value<bool>();
        }
        read_ids(t, "disabled_plugins", s.disabled_plugins);
        read_ids(t, "allow_ironman", s.allow_ironman);
        read_ids(t, "allow_multiplayer", s.allow_multiplayer);
        read_ids(t, "enabled_modules", s.enabled_modules);
        read_ids(t, "disabled_modules", s.disabled_modules);
        const std::string level = t["log_level"].value_or<std::string>("info");
        if (level == "debug") s.log_level = ANVIL_LOG_DEBUG;
        else if (level == "warning") s.log_level = ANVIL_LOG_WARNING;
        else if (level == "error") s.log_level = ANVIL_LOG_ERROR;
        s.profile = t["profile"].value_or(false);
    } catch (const toml::parse_error& e) {
        s = AnvilSettings{};
        s.error = std::string(e.description());
        s.line = static_cast<uint32_t>(e.source().begin.line);
    } catch (const WrongType& e) {
        s = AnvilSettings{};
        s.error = e.what;
        s.line = e.line;
    }
    return s;
}

std::vector<std::filesystem::path> plugin_folders(const std::filesystem::path& plugins_dir) {
    std::error_code ec;
    std::vector<std::filesystem::path> folders;
    // increment(ec): a range-for's operator++ throws when the listing fails
    // midway (a drive that goes away), which would end the whole start-up.
    for (auto it = std::filesystem::directory_iterator(plugins_dir, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        std::error_code type_ec;
        if (it->is_directory(type_ec)) folders.push_back(it->path());
    }
    std::sort(folders.begin(), folders.end());
    return folders;
}

i18n::Message missing_entry_reason(const Manifest& manifest) {
    std::error_code ec;
    // plugin.toml is UTF-8; `/ std::string` would read the name in the ANSI code page.
    const std::u8string entry(manifest.entry.begin(), manifest.entry.end());
    if (std::filesystem::exists(manifest.folder / entry, ec)) return {};
    return i18n::Message("entry_missing", {manifest.entry});
}

}  // namespace anvil
