#pragma once

#include "semver.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace anvil {

enum class Phase { Early, Main };

struct Dependency {
    std::string id;
    VersionRange range;
    bool optional = false;
};

struct Manifest {
    std::string id;
    std::string name;
    Version version;
    std::vector<std::string> authors;
    std::string description;
    unsigned api = 0;
    std::string entry;  // DLL file name inside the plugin folder
    Phase phase = Phase::Main;
    bool affects_gameplay = false;
    std::vector<std::string> load_after;
    std::vector<Dependency> dependencies;
    std::vector<std::string> game_builds;  // build ids or "*"

    std::filesystem::path folder;    // plugin folder
    std::vector<std::string> notes;  // non-fatal remarks (unknown keys ...)
};

struct ManifestResult {
    std::optional<Manifest> manifest;
    std::string error;  // set when manifest is empty
};

ManifestResult parse_manifest_text(std::string_view text, const std::filesystem::path& folder);
ManifestResult load_manifest(const std::filesystem::path& folder);  // reads folder/plugin.toml

bool is_valid_plugin_id(std::string_view id);

}  // namespace anvil
