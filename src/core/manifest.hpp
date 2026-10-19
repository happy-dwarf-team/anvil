#pragma once

#include "semver.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace anvil {

enum class Phase { Early, Main };

// The manifest's `api` this host accepts. Until the plugin API 2 header lands,
// AnvilHost is still the API 1 table (ANVIL_API_VERSION 1); only the manifest
// already declares API 2.
inline constexpr unsigned kManifestApi = 2;

// allow_ironman / allow_multiplayer: false, or "opt-in" (the player may allow
// the plugin in that mode).
enum class Allow { No, OptIn };

struct Dependency {
    std::string id;
    VersionRange range;
    bool optional = false;
};

// [[conflicts]]: the declaring plugin is skipped while a matching one is enabled.
struct Conflict {
    std::string id;
    VersionRange range;
};

struct Manifest {
    std::string id;
    std::string name;
    Version version;
    std::vector<std::string> authors;
    std::string description;
    unsigned api = 0;
    std::optional<VersionRange> anvil;  // Anvil versions the plugin needs ("anvil" key); empty = any
    std::string entry;  // DLL file name inside the plugin folder
    Phase phase = Phase::Main;
    std::string symbols;   // symbols.toml path relative to the folder; empty = none (runs on every build)
    std::string settings;  // settings.toml path relative to the folder; empty = none
    bool affects_gameplay = false;
    Allow allow_ironman = Allow::No;
    Allow allow_multiplayer = Allow::No;
    std::vector<std::string> load_after;
    std::string issues;      // URL, optional
    std::string repository;  // URL, optional
    std::vector<Dependency> dependencies;
    std::vector<Conflict> conflicts;

    std::filesystem::path folder;  // plugin folder
};

struct ManifestResult {
    std::optional<Manifest> manifest;
    std::string error;  // set when manifest is empty
};

// Parses plugin.toml v2 (design §5.1). Any unknown key or a value of the wrong
// type is an error; so is `api = 1`. Does not touch the file system.
ManifestResult parse_manifest_text(std::string_view text, const std::filesystem::path& folder);
// Reads folder/plugin.toml and also checks that the files it names
// (`symbols`, `settings`) exist in the folder.
ManifestResult load_manifest(const std::filesystem::path& folder);

bool is_valid_plugin_id(std::string_view id);

// Parses "48 8B ?? 24" (whitespace optional, "??" = any byte) into bytes and
// a compare mask. At least one byte must not be a wildcard.
bool parse_byte_pattern(std::string_view text, std::vector<uint8_t>& bytes, std::vector<uint8_t>& mask,
                        std::string& error);

}  // namespace anvil
