#pragma once

#include "cli/check.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::cli {

struct GameOptions {
    std::filesystem::path game_dir;  // the EU4 folder: eu4.exe and anvil/
    // The build id of an eu4.exe by its SHA-256, or nullptr when it is unknown.
    // Empty: the table compiled into Anvil.
    std::function<const char*(std::string_view sha256)> build_of;
};

// One folder of anvil/plugins.
struct GamePlugin {
    std::string folder;  // the folder's name
    CheckReport report;  // the plugin as `anvil check <folder> --exe <eu4.exe>` sees it
    // Disabled in anvil.toml (or all plugins are): its problems are listed but
    // do not count towards the exit code.
    bool disabled = false;
    std::string status;  // "loaded", "skipped" or "disabled"
};

struct LoadedPlugin {
    std::string id;
    std::string version;
    std::string phase;   // "early" or "main"
    std::string folder;
};

struct SkippedPlugin {
    std::string id;      // the folder's name when plugin.toml is broken
    std::string folder;
    std::string reason;
    bool disabled = false;  // by the player in anvil.toml: not a fault
};

// Two plugins that will load together declare code ranges that overlap.
struct OverlapWarning {
    std::string plugin;        // the one that loads first
    std::string other;
    std::string file;          // plugin's symbols.toml
    uint32_t line = 0;         // of the overlapping symbol
    std::string message;
};

struct GameReport {
    int exit_code = kExitOk;
    std::string cannot_check;  // why, with kExitCannotCheck for the whole folder
    std::string game_dir;      // as passed
    std::string exe_name = "eu4.exe";
    std::string exe_sha256;
    std::string exe_build;     // empty: not a game build this Anvil knows
    bool enabled = true;       // anvil.toml
    std::vector<std::string> disabled_plugins;
    std::vector<Problem> problems;       // anvil.toml
    std::vector<GamePlugin> plugins;     // in discovery order
    std::vector<LoadedPlugin> load_order;
    std::vector<SkippedPlugin> skipped;  // discovery first, then the resolver, as in the log
    std::vector<OverlapWarning> warnings;
};

// `anvil check --game <folder>` (design §16): every plugin in anvil/plugins
// checked against the folder's eu4.exe (without writing symbols.lock), then
// resolved with the loader's own rules (anvil.toml, API, Anvil range, build
// and symbols, duplicates, conflicts, dependencies, cycles), so the report
// says what loads, in which order, and what is skipped and why.
GameReport check_game(const GameOptions& options);

std::string format_game_report(const GameReport& report, Format format);

}  // namespace anvil::cli
