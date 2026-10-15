#include "manifest.hpp"

#include <toml++/toml.hpp>

#include <fstream>
#include <set>
#include <sstream>

namespace anvil {
namespace {

const std::set<std::string> kKnownKeys = {
    "id", "name", "version", "authors", "description", "api", "entry", "phase",
    "affects_gameplay", "load_after", "dependencies", "game_builds",
};

bool read_string_array(const toml::node* node, std::vector<std::string>& out, std::string& error,
                       std::string_view key) {
    if (node == nullptr) return true;
    const auto* array = node->as_array();
    if (array == nullptr) {
        error = std::string(key) + " must be an array of strings";
        return false;
    }
    for (const auto& item : *array) {
        const auto value = item.value<std::string>();
        if (!value) {
            error = std::string(key) + " must be an array of strings";
            return false;
        }
        out.push_back(*value);
    }
    return true;
}

}  // namespace

bool is_valid_plugin_id(std::string_view id) {
    if (id.empty() || id.size() > 64 || id.front() == '-' || id.back() == '-') return false;
    for (char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

ManifestResult parse_manifest_text(std::string_view text, const std::filesystem::path& folder) {
    ManifestResult result;
    toml::table table;
    try {
        table = toml::parse(text);
    } catch (const toml::parse_error& e) {
        std::ostringstream message;
        message << "plugin.toml: " << e.description() << " (line " << e.source().begin.line << ")";
        result.error = message.str();
        return result;
    }

    Manifest m;
    m.folder = folder;
    auto require_string = [&](std::string_view key, std::string& out) {
        const auto value = table[key].value<std::string>();
        if (!value || value->empty()) {
            result.error = "plugin.toml: missing or empty '" + std::string(key) + "'";
            return false;
        }
        out = *value;
        return true;
    };

    if (!require_string("id", m.id) || !require_string("name", m.name) || !require_string("entry", m.entry)) {
        return result;
    }
    if (!is_valid_plugin_id(m.id)) {
        result.error = "plugin.toml: id '" + m.id + "' must match [a-z0-9-]+ (max 64 characters)";
        return result;
    }
    std::string versionText;
    if (!require_string("version", versionText)) return result;
    const auto version = Version::parse(versionText);
    if (!version) {
        result.error = "plugin.toml: version '" + versionText + "' is not SemVer (MAJOR.MINOR.PATCH)";
        return result;
    }
    m.version = *version;
    const auto api = table["api"].value<int64_t>();
    if (!api || *api <= 0) {
        result.error = "plugin.toml: missing or invalid 'api'";
        return result;
    }
    m.api = static_cast<unsigned>(*api);
    if (m.entry.find_first_of("/\\:") != std::string::npos || !m.entry.ends_with(".dll")) {
        result.error = "plugin.toml: entry must be a .dll file name inside the plugin folder";
        return result;
    }
    const std::string phase = table["phase"].value_or<std::string>("main");
    if (phase == "early") m.phase = Phase::Early;
    else if (phase == "main") m.phase = Phase::Main;
    else {
        result.error = "plugin.toml: phase must be \"early\" or \"main\"";
        return result;
    }
    m.description = table["description"].value_or<std::string>("");
    m.affects_gameplay = table["affects_gameplay"].value_or(false);

    std::string error;
    if (!read_string_array(table.get("authors"), m.authors, error, "authors") ||
        !read_string_array(table.get("load_after"), m.load_after, error, "load_after") ||
        !read_string_array(table.get("game_builds"), m.game_builds, error, "game_builds")) {
        result.error = "plugin.toml: " + error;
        return result;
    }
    if (m.game_builds.empty()) {
        result.error = "plugin.toml: game_builds must list supported builds (or \"*\")";
        return result;
    }
    if (const auto* deps = table.get_as<toml::array>("dependencies")) {
        for (const auto& node : *deps) {
            const auto* dep = node.as_table();
            if (dep == nullptr) {
                result.error = "plugin.toml: dependencies must be an array of tables ([[dependencies]])";
                return result;
            }
            Dependency d;
            d.id = (*dep)["id"].value_or<std::string>("");
            const std::string range = (*dep)["version"].value_or<std::string>("*");
            d.optional = (*dep)["optional"].value_or(false);
            const auto parsed = VersionRange::parse(range);
            if (!is_valid_plugin_id(d.id) || !parsed) {
                result.error = "plugin.toml: invalid dependency '" + d.id + "' version '" + range + "'";
                return result;
            }
            d.range = *parsed;
            m.dependencies.push_back(std::move(d));
        }
    } else if (table.contains("dependencies")) {
        result.error = "plugin.toml: dependencies must be an array of tables ([[dependencies]])";
        return result;
    }
    for (const auto& [key, value] : table) {
        if (!kKnownKeys.contains(std::string(key.str()))) {
            m.notes.push_back("unknown key '" + std::string(key.str()) + "' ignored");
        }
    }
    result.manifest = std::move(m);
    return result;
}

ManifestResult load_manifest(const std::filesystem::path& folder) {
    std::ifstream file(folder / "plugin.toml", std::ios::binary);
    if (!file) {
        return {std::nullopt, "plugin.toml not found"};
    }
    std::ostringstream content;
    content << file.rdbuf();
    std::string text = content.str();
    if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
    return parse_manifest_text(text, folder);
}

}  // namespace anvil
