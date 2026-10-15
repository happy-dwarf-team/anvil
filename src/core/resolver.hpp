#pragma once

#include "manifest.hpp"

#include <string>
#include <vector>

namespace anvil {

struct ResolveInput {
    std::vector<Manifest> manifests;  // in discovery order
    unsigned host_api = 0;
    const char* game_build = nullptr;  // nullptr = unknown build
    std::vector<std::string> disabled;  // ids disabled in anvil.toml
};

struct Skipped {
    std::string id;     // may be a folder name when the manifest is broken
    std::string reason;
};

struct ResolveResult {
    std::vector<Manifest> order;  // load order: all early plugins first, then main
    std::vector<Skipped> skipped;
};

// Validates plugins against the host and the game build, resolves required and
// optional dependencies (SemVer ranges) and load_after, and orders them
// topologically (ties broken by id). Plugins that cannot be loaded are listed
// in `skipped` with a human-readable reason; anything depending on them is
// skipped too.
ResolveResult resolve(const ResolveInput& input);

}  // namespace anvil
