#include "manifest.hpp"

#include <anvil/anvil_plugin.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <cstdio>

#include <fstream>
#include <set>
#include <sstream>

namespace anvil {
namespace {

const std::set<std::string> kKnownKeys = {
    "id", "name", "version", "authors", "description", "api", "entry", "phase",
    "affects_gameplay", "load_after", "dependencies", "game_builds", "expect",
};

const std::set<std::string> kKnownExpectKeys = {"build", "rva", "bytes", "name"};

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

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

bool parse_byte_pattern(std::string_view text, std::vector<uint8_t>& bytes, std::vector<uint8_t>& mask,
                        std::string& error) {
    bytes.clear();
    mask.clear();
    std::string digits;
    for (char c : text) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') digits.push_back(c);
    }
    if (digits.empty() || digits.size() % 2 != 0) {
        error = "bytes must be pairs of hex digits or \"??\"";
        return false;
    }
    if (digits.size() / 2 > ANVIL_MAX_EXPECT_BYTES) {
        error = "bytes longer than " + std::to_string(ANVIL_MAX_EXPECT_BYTES) + " bytes";
        return false;
    }
    bool any = false;
    for (size_t i = 0; i < digits.size(); i += 2) {
        if (digits[i] == '?' && digits[i + 1] == '?') {
            bytes.push_back(0);
            mask.push_back(0);
            continue;
        }
        const int high = hex_digit(digits[i]), low = hex_digit(digits[i + 1]);
        if (high < 0 || low < 0) {
            error = "'" + digits.substr(i, 2) + "' is not a hex byte or \"??\"";
            return false;
        }
        bytes.push_back(static_cast<uint8_t>(high * 16 + low));
        mask.push_back(0xFF);
        any = true;
    }
    if (!any) {
        error = "bytes must contain at least one byte that is not \"??\"";
        return false;
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
    if (const auto* list = table.get_as<toml::array>("expect")) {
        const bool anyBuild = std::find(m.game_builds.begin(), m.game_builds.end(), "*") != m.game_builds.end();
        for (const auto& node : *list) {
            const auto* entry = node.as_table();
            if (entry == nullptr) {
                result.error = "plugin.toml: expect must be an array of tables ([[expect]])";
                return result;
            }
            Expectation e;
            e.name = (*entry)["name"].value_or<std::string>("");
            const auto rva = (*entry)["rva"].value<int64_t>();
            char where[96];
            std::snprintf(where, sizeof(where), "[[expect]] #%zu", m.expectations.size() + 1);
            const std::string label = std::string(where) + (e.name.empty() ? "" : " (" + e.name + ")");
            e.build = (*entry)["build"].value_or<std::string>("");
            if (e.build.empty()) {
                result.error = "plugin.toml: " + label + " needs 'build' (a build id from game_builds, or \"*\")";
                return result;
            }
            if (e.build != "*" && !anyBuild &&
                std::find(m.game_builds.begin(), m.game_builds.end(), e.build) == m.game_builds.end()) {
                result.error = "plugin.toml: " + label + " build '" + e.build + "' is not listed in game_builds";
                return result;
            }
            if (!rva || *rva < 0 || *rva > 0xFFFFFFFFll) {
                result.error = "plugin.toml: " + label + " needs 'rva' (an integer, 0 to 0xFFFFFFFF)";
                return result;
            }
            e.rva = static_cast<uint32_t>(*rva);
            const auto pattern = (*entry)["bytes"].value<std::string>();
            if (!pattern) {
                result.error = "plugin.toml: " + label + " needs 'bytes' (e.g. \"48 8B ?? 24\")";
                return result;
            }
            if (!parse_byte_pattern(*pattern, e.bytes, e.mask, error)) {
                result.error = "plugin.toml: " + label + ": " + error;
                return result;
            }
            if (static_cast<uint64_t>(e.rva) + e.bytes.size() > 0x100000000ull) {
                result.error = "plugin.toml: " + label + " extends past RVA 0xFFFFFFFF";
                return result;
            }
            for (const auto& [key, value] : *entry) {
                if (!kKnownExpectKeys.contains(std::string(key.str()))) {
                    m.notes.push_back(label + ": unknown key '" + std::string(key.str()) + "' ignored");
                }
            }
            m.expectations.push_back(std::move(e));
        }
    } else if (table.contains("expect")) {
        result.error = "plugin.toml: expect must be an array of tables ([[expect]])";
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
