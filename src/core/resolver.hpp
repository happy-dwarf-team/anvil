#pragma once

#include "manifest.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace anvil {

struct ResolveInput {
    std::vector<Manifest> manifests;  // in discovery order
    unsigned host_api = 0;
    std::optional<Version> host_version;  // Anvil version, checked against each plugin's "anvil" range
    const char* game_build = nullptr;  // nullptr = unknown build
    std::vector<std::string> disabled;  // ids disabled in anvil.toml
    // Optional check after the API and Anvil version checks (the plugin's
    // symbols.toml); returns why the plugin must be skipped, empty = keep it.
    std::function<std::string(const Manifest&)> verify;
};

struct Skipped {
    std::string id;     // may be a folder name when the manifest is broken
    std::string reason;
    std::filesystem::path folder;  // the plugin folder
    bool disabled = false;         // disabled by the player in anvil.toml, not a fault
    std::string mod;               // the mod's name for a plugin from a mod (design §14)
    bool awaiting_approval = false;  // a plugin from a mod that waits for the player's approval
};

struct ResolveResult {
    std::vector<Manifest> order;  // load order: all early plugins first, then main
    std::vector<Skipped> skipped;
};

// Validates plugins against the host (and, through `verify`, the game), skips
// plugins that declare a `conflicts` entry matching another loaded plugin,
// resolves required and optional dependencies (SemVer ranges) and load_after,
// and orders them topologically (ties broken by id). A plugin with
// affects_gameplay = true depends on anvil-game (any version) without
// declaring it (design §13): the resolver adds that dependency, `implied`. Plugins that cannot be
// loaded are listed in `skipped` with a human-readable reason; anything
// depending on them is skipped too. Conflict edge cases: docs/design.md §10.
ResolveResult resolve(const ResolveInput& input);

}  // namespace anvil
