#pragma once

#include "semver.hpp"

#include <cstdint>
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

// Game bytes a plugin depends on (functions it calls, vtables, globals),
// declared as [[expect]] and verified before the plugin DLL is loaded.
struct Expectation {
    std::string build;           // build id from game_builds, or "*" (bytes identical in every build)
    uint32_t rva = 0;
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask;   // 0xFF = compare, 0x00 = wildcard ("??")
    std::string name;            // optional, for the log
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
    bool affects_gameplay = false;
    std::vector<std::string> load_after;
    std::vector<Dependency> dependencies;
    std::vector<std::string> game_builds;  // build ids or "*"
    std::vector<Expectation> expectations;

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

// Parses "48 8B ?? 24" (whitespace optional, "??" = any byte) into bytes and
// a compare mask. At least one byte must not be a wildcard.
bool parse_byte_pattern(std::string_view text, std::vector<uint8_t>& bytes, std::vector<uint8_t>& mask,
                        std::string& error);

}  // namespace anvil
