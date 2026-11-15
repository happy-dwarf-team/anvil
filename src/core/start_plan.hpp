#pragma once

#include "benchmark.hpp"
#include "crash_history.hpp"
#include "discovery.hpp"
#include "manifest.hpp"
#include "mod_plugins.hpp"
#include "plugin_inventory.hpp"
#include "resolver.hpp"
#include "retired_move.hpp"
#include "safe_mode.hpp"
#include "trust.hpp"

#include "i18n/message.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil {

// What the player chose for a start, as saved: anvil.toml, the approvals in
// trust.toml, and the player's settings file of each plugin whose
// settings.toml retires settings (ADR 0013, point 4), whose values move
// into anvil.toml at the start.
struct PlayerChoices {
    AnvilSettings anvil_toml;
    TrustFile trust;
    // anvil/data/<id>/settings.toml by plugin id; nullopt: there is none.
    std::map<std::string, std::optional<std::string>> settings_files;
};

// Reads the player's choices for the plugins of `inventory`. The loader,
// which read anvil.toml before anything else, passes it in.
PlayerChoices read_player_choices(const PluginInventory& inventory);
PlayerChoices read_player_choices(const PluginInventory& inventory, AnvilSettings anvil_toml);

// The conditions of a start that are not the player's choices.
struct StartConditions {
    bool anvil_disabled = false;  // ANVIL_DISABLE=1: nothing loads
    // Safe mode (design §10) and, in it, the crash reports of the starts that
    // led to it (safe_mode_crashes): a standard plugin one of them was in is
    // left out.
    bool safe_mode = false;
    std::vector<CrashSummary> crashes;
    // A benchmark start (docs/spec/benchmark-v1.md §2): without:<id> leaves
    // the compared plugin out.
    std::optional<BenchmarkArm> benchmark;
    // The host the plugins are checked against: this Anvil.
    unsigned host_api = kManifestApi;
    std::optional<Version> host_version = Version::parse(ANVIL_VERSION);
};

// Why a plugin does not load at a start, as the player reads it (the
// Manager's and the overlay's list): the player's switch, a plugin it needs,
// an approval, or something else it cannot get past.
enum class NotLoading {
    None,              // it loads
    Off,               // the player's switch, all plugins off, or the benchmark arm
    NeedsPlugin,       // `missing` does not load; `through` is the path to it
    NotInstalled,      // `missing` is required and not here at all
    WaitsForApproval,  // from a mod, not approved
    CannotLoad,        // anything else; `why` says what
};

// "off", "needs", "not-installed", "approval", "cannot-load"; "" for None.
const char* not_loading_name(NotLoading why);

// One plugin of a start plan.
struct PlannedPlugin {
    std::string id;  // the folder's name when plugin.toml does not parse
    // The folder it loads from: anvil/plugins/<folder>, or for a mod's
    // plugin its approved copy (the mod's folder while it does not load).
    std::filesystem::path folder;
    int local = -1;  // its index in PluginInventory::local, or -1
    int mod = -1;    // its index in PluginInventory::mods and StartPlan::mods, or -1
    std::string mod_name;
    // plugin.toml with the dependency on anvil-game that affects_gameplay
    // implies (add_implied_dependencies) and `folder` as above; empty when
    // it does not parse.
    std::optional<Manifest> manifest;
    // By [[modules]] entry: on or off at this start (the plan's lists).
    std::vector<bool> modules_on;
    std::string approved_hash;  // a mod's plugin: the approved copy's hash
    bool makes_copy = false;    // the start makes its approved copy first (StartWrites::copies)

    // Why it does not load; None in the load order.
    NotLoading not_loading = NotLoading::None;
    i18n::Message why;    // the reason by code
    std::string reason;   // its English words, as the log says it
    SkipKind kind = SkipKind::other;
    bool disabled = false;       // the player's switch (or all plugins off, or the arm): not a fault
    bool benchmark_arm = false;  // left out by the benchmark arm
    bool awaiting_approval = false;  // a mod's plugin that waits for the player's approval
    std::string missing;               // NeedsPlugin, NotInstalled: a plugin id
    std::vector<std::string> through;  // plugin ids from this one to `missing`, both excluded

    bool loads() const { return not_loading == NotLoading::None; }
};

// An approved copy to make from the mod's folder before any plugin loads.
struct PlannedCopy {
    std::string id;
    int mod = -1;  // index in StartPlan::mods
    std::filesystem::path from;  // the folder in the mod
    std::filesystem::path to;    // anvil/data/anvil/approved/<id>/<hash>
    std::string hash;
};

// A plugin's retired settings to move into anvil.toml.
struct PlannedRetiredMove {
    std::string plugin;
    std::filesystem::path player_file;  // anvil/data/<id>/settings.toml
    RetiredMovePlan move;
    // What the move was planned from, so it can be planned again from the
    // files as they are later (carry_out_retired_move_now).
    Manifest manifest;
    SettingsFile schema;
};

// What a start writes before any plugin loads, in this order.
struct StartWrites {
    // Approved copies trust.toml no longer names (revoked ids, older hashes,
    // unfinished copies); none while trust.toml does not parse.
    std::vector<std::filesystem::path> remove;
    std::vector<PlannedCopy> copies;
    std::vector<PlannedRetiredMove> retired;  // only the moves that write
    bool empty() const { return remove.empty() && copies.empty() && retired.empty(); }
};

// Start plan (GLOSSARY, ADR 0014): what one start would do with the plugins
// of a game folder: which load, in what order and with which modules on, and
// which do not load and why. The loader carries it out; anvil check --game
// and the Manager show it for the next start.
struct StartPlan {
    bool enabled = true;     // false: anvil.toml's enabled = false or ANVIL_DISABLE=1; nothing loads
    bool safe_mode = false;
    std::vector<PlannedPlugin> load;     // the load order: early plugins first, then main
    std::vector<PlannedPlugin> skipped;  // as the log lists them: at discovery, then the resolver's
    // anvil.toml as it applies at this start: after the retired settings moved.
    AnvilSettings lists;
    // The playset's plugins, decided against trust.toml (design §14), by
    // PluginInventory::mods. A Shadowed one has no PlannedPlugin.
    std::vector<ModPlugin> mods;
    std::vector<std::string> warnings;  // trust.toml that does not parse
    // Retired settings that cannot move (their player's file does not parse).
    std::vector<std::pair<std::string, RetiredMoveNote>> notes;
    StartWrites writes;

    // The plugin's entry, in the load order or skipped; nullptr for none.
    const PlannedPlugin* of_local(size_t index) const;
    const PlannedPlugin* of_mod(size_t index) const;
    // The first with this id: a loading one wins.
    const PlannedPlugin* find(std::string_view id) const;
};

// The plan of one start. A pure function: it reads nothing and writes
// nothing. The order of the checks is the loader's (design §10, §14): at
// discovery a plugin.toml that does not parse, a missing entry DLL and safe
// mode, for a mod's plugin its approval; then the resolver (anvil.toml's
// switch, the benchmark arm, the API and Anvil range, the plugin's check
// against the game, conflicts, dependencies, cycles).
StartPlan plan_start(const PluginInventory& inventory, const PlayerChoices& choices, const StartConditions& conditions);

// Carries a planned move out later than the plan was made (the Manager,
// before a switch the player makes): planned again from the player's file
// and anvil.toml as they are now, so a value saved since the plan stays.
// Returns what to show; a warning means a file was not written, and
// `anvil_toml_written` false that it was anvil.toml (nothing changed).
std::vector<RetiredMoveNote> carry_out_retired_move_now(const PlannedRetiredMove& move,
                                                        const std::filesystem::path& anvil_toml,
                                                        bool* anvil_toml_written = nullptr);

// The check of the folder a planned plugin loads from (for a mod's plugin
// whose update waits, its approved copy's); nullptr when there is none.
const PluginCheck* check_of(const PluginInventory& inventory, const StartPlan& plan, const PlannedPlugin& plugin);

// The conditions of the next start as anvil/data/anvil/unfinished-starts.txt
// and anvil/crashes say them, without counting a start in: what the Manager
// and anvil check --game say about the next start.
struct NextStart {
    StartRecord record;
    StartConditions conditions;  // safe_mode and crashes from `record`
};
NextStart read_next_start(const std::filesystem::path& anvil_dir);

}  // namespace anvil
