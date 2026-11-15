#pragma once

#include "cli/check.hpp"
#include "core/mod_plugins.hpp"
#include "core/plugin_inventory.hpp"
#include "core/safe_mode.hpp"
#include "core/start_plan.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::cli {

struct GameOptions {
    std::filesystem::path game_dir;  // the EU4 folder: eu4.exe and anvil/
    // EU4's user folder with dlc_load.json and mod/; empty: the one the game
    // uses (eu4_user_dir).
    std::filesystem::path user_dir;
    // The build id of an eu4.exe by its SHA-256, or nullptr when it is unknown.
    // Empty: the table compiled into Anvil.
    std::function<const char*(std::string_view sha256)> build_of;
};

// One folder of anvil/plugins, by PluginInventory::local.
struct GamePlugin {
    std::string folder;  // the folder's name
    CheckReport report;  // the plugin as `anvil check <folder> --exe <eu4.exe>` sees it
};

// A plugin in <mod>/anvil/<id>/ of a mod enabled in the playset (design §14),
// by StartPlan::mods: its facts and its decision, as text for the reports.
struct GameModPlugin {
    std::string id;
    std::string mod;          // its name
    std::string descriptor;   // as dlc_load.json lists it
    std::string workshop_id;  // "" for a local mod
    std::string folder;       // <mod>/anvil/<id>
    ModPluginState state = ModPluginState::Pending;
    std::string reason;
    i18n::Message why;        // `reason` by code, for the player's language (#158)
    std::string hash;         // of the folder in the mod; "" when it cannot be hashed
    std::string version;      // its plugin.toml's; "" when that is broken
    std::string problem;      // why the folder in the mod cannot be approved; "" when it can
    // trust.toml's approval of the id for this mod; empty when there is none.
    std::string approved_version, approved_hash, approved_date;
    std::string loads_from;   // the approved copy for approved and update-pending; "" otherwise
    bool copy_missing = false;  // approved, and the copy is made at the next start
    std::vector<std::string> others;  // conflict, shadowed: the other mods with this id
    // The folder that loads (the approved copy, or the mod's folder while the
    // copy is not made), or for the others the mod's folder, as `anvil check
    // <folder> --exe <eu4.exe>` sees it.
    CheckReport report;
    // update-pending: the folder in the mod, checked the same way, since `report`
    // is the approved copy that still loads.
    std::optional<CheckReport> update;

    // The files an approval would approve.
    const CheckReport& offered() const { return update ? *update : report; }
};

// Two plugins that will load together declare code ranges that overlap.
struct OverlapWarning {
    std::string plugin;        // the one that loads first
    std::string other;
    std::string file;          // plugin's symbols.toml
    uint32_t line = 0;         // of the overlapping symbol
    std::string message;
};

// The report of `anvil check --game` (design §16), built on the start plan
// of the next start (ADR 0014): the plan, each plugin's CheckReport and the
// facts of eu4.exe. Strings for its states are made only when it is
// formatted.
struct GameReport {
    int exit_code = kExitOk;
    std::string cannot_check;  // why, with kExitCannotCheck for the whole folder
    std::string game_dir;      // as passed
    std::string exe_name = "eu4.exe";
    std::string exe_sha256;
    std::string exe_build;     // empty: not a game build this Anvil knows
    std::vector<Problem> problems;  // anvil.toml, dlc_load.json, trust.toml
    // What the folder and its playset hold, the player's choices as saved,
    // and the plan of the next start as a normal start (no safe mode, no
    // benchmark arm); `next_start` says whether that start is in safe mode.
    PluginInventory inventory;
    PlayerChoices choices;
    StartPlan plan;
    StartRecord next_start;
    std::vector<GamePlugin> plugins;         // by inventory.local
    std::vector<GameModPlugin> mod_plugins;  // by plan.mods
    std::vector<OverlapWarning> warnings;
    std::string user_dir;
    std::vector<std::string> mod_warnings;   // the playset's mods that cannot be read
};

// `anvil check --game <folder>` (design §16): the plugin inventory of the
// folder against its eu4.exe (every plugin of anvil/plugins and of the
// playset's mods, checked as `anvil check <folder> --exe <eu4.exe>` does,
// without writing symbols.lock), and the plan of the next start with the
// loader's own rules, so the report says what loads, in which order, what is
// skipped and why, and what that start writes first. Nothing is written: a
// missing approved copy is reported, not made.
GameReport check_game(const GameOptions& options);

// The plan's entry of a plugin of the report; nullptr when it has none (a
// plugin in a mod that a local plugin shadows).
const PlannedPlugin* planned(const GameReport& report, const GamePlugin& plugin);
const PlannedPlugin* planned(const GameReport& report, const GameModPlugin& plugin);
// "loaded", "disabled" or "skipped", as the reports say it.
const char* status_name(const PlannedPlugin* plugin);
// The player turned the plugin off (disabled_plugins, or enabled = false):
// its problems do not count.
bool turned_off(const GameReport& report, const GamePlugin& plugin);
// The plan's skipped plugins the report lists: not the plugins from mods
// that wait for the player (pending, a conflict) or cannot be approved,
// which the mods' section lists with their state.
std::vector<const PlannedPlugin*> listed_skipped(const GameReport& report);

std::string format_game_report(const GameReport& report, Format format);

}  // namespace anvil::cli
