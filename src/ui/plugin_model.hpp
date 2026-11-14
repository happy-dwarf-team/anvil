// The view model of the Plugins tab, shared by the Manager and the overlay
// (ADR 0013, point 9; docs/ui-guidelines.md §5). It takes a snapshot of
// plugins, modules and settings with their saved values, the values in effect
// and their defaults, and answers what the player sees: what loads and why
// not, what waits for the next start, what a search and the filter chips
// match, what a dependency chain turns on, and what a reset or an undo
// changes, as a list of edits. It draws nothing and writes nothing: the
// Manager applies edits to the files, the overlay through anvil.control.
//
// Text in it is UTF-8. Setting values are canonical text (the apps write them
// the same way for the same value), so equal values compare equal.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::ui {

// --- The snapshot -----------------------------------------------------------

// A plugin's or a module's switch and opt-ins (anvil.toml).
struct Switches {
    bool on = true;
    bool allow_ironman = false;
    bool allow_multiplayer = false;
    bool operator==(const Switches&) const = default;
};

// What the author offers (plugin.toml): a choice not offered is disabled.
struct Offers {
    bool ironman = true;
    bool multiplayer = true;
};

struct SettingSnap {
    std::string name;         // the key in settings.toml
    std::string label;        // in the player's language
    std::string description;  // in the player's language
    std::string group;        // "" for none
    std::string module;       // "" when it belongs to the plugin
    bool advanced = false;
    bool restart = false;     // true: takes effect at the next start
    bool counts = false;      // counts for the game (the multiplayer hash)
    bool locked = false;      // locked in a multiplayer game (overlay)
    std::string default_value;
    std::string saved;        // the player's file
    std::string in_effect;    // the running game's value, else the last start's
    std::string overridden;   // in the game: the value in effect when an environment
                              // variable overrides the saved one; "" for none
};

struct ModuleSnap {
    std::string id;
    std::string name;
    std::string description;
    std::string group;  // "" for none
    bool changes_game = false;
    bool default_on = true;  // plugin.toml's `default`
    Offers offers;
    Switches saved;      // `on` resolved from the default and anvil.toml's lists
    Switches in_effect;  // as the running game or the last start had it
};

enum class Origin { Own, FromMod, WithAnvil };

// Why a plugin does not load, when the reason is its own (dependencies are
// worked out by the model).
enum class Blocked {
    None,
    CannotLoad,       // invalid, a conflict between mods, a local plugin with its id loads instead
    Failed,           // it loaded and failed (the running game only)
    WaitsForApproval  // from a mod, never approved
};

struct PluginSnap {
    std::string id;
    std::string name;
    std::string version;      // the version that runs
    std::string description;
    Origin origin = Origin::Own;
    bool changes_game = false;  // the plugin or any of its modules
    Offers offers;
    Switches saved;
    Switches in_effect;
    std::vector<std::string> requires_ids;  // plugin ids
    std::vector<ModuleSnap> modules;
    std::vector<SettingSnap> settings;
    Blocked blocked = Blocked::None;
    std::string blocked_reason;  // the app's words for CannotLoad and Failed
    // From a mod: an update waits for approval (the approved copy runs) and
    // whether the approval itself waits for the next start.
    std::string update_waits;  // the version that waits, "" for none
    bool approved = false;
    bool approved_in_effect = false;
    // The overlay refuses to turn it off (it is the overlay or what it needs).
    bool needed_here = false;
};

// The kind of game the overlay draws in ("kept out of this game").
enum class Game { None, Normal, Ironman, Multiplayer };

struct Snapshot {
    std::vector<PluginSnap> plugins;
    Game game = Game::None;
};

// --- Edits ------------------------------------------------------------------

enum class Which { On, Ironman, Multiplayer };

// One change for the app to write. A switch edit names `key` as the plugin id
// or "<plugin>.<module>", as anvil.control's switch calls take it.
struct Edit {
    enum class Kind { Switch, Setting } kind = Kind::Switch;
    std::string plugin;
    std::string module;   // Switch: "" for the plugin's own switch
    Which which = Which::On;
    bool on = false;      // Switch
    std::string setting;  // Setting: its name
    std::string value;    // Setting: the new value
    std::string old_value;  // Setting: the value it replaces (for the confirmation)
    std::string key() const { return module.empty() ? plugin : plugin + "." + module; }
    bool operator==(const Edit&) const = default;
};

// --- What the player sees ---------------------------------------------------

// The list entry's glyph: ✓ on, ○ off, ▲ needs attention, ✕ cannot load.
enum class Glyph { On, Off, Attention, CannotLoad };

// Why a plugin does not load, after its dependencies.
struct LoadState {
    bool loads = false;  // at the next start, with the saved files
    enum class Why {
        None,
        Off,                // the player's switch
        NeedsPlugin,        // `missing` does not load; `through` is the path to it
        NotInstalled,       // `missing` is required and not here at all
        CannotLoad,
        Failed,
        WaitsForApproval
    } why = Why::None;
    std::string missing;               // a plugin id
    std::vector<std::string> through;  // plugin ids from the plugin to `missing`, both excluded
};

// The list entry's second line: the exception if there is one, else a summary.
struct EntryLine {
    enum class Kind {
        Summary,
        Off,
        NeedsPlugin,      // `name`: the missing plugin's name
        NotInstalled,     // `name`: the missing id
        CannotLoad,
        Failed,
        WaitsForApproval,
        UpdateWaits,      // `name`: the version that waits
        KeptOutOfIronman,
        KeptOutOfMultiplayer
    } kind = Kind::Summary;
    std::string name;
    // Summary: "20 of 25 modules on · 2 changed", "4 settings", "changes the game".
    std::size_t modules_on = 0;
    std::size_t modules = 0;
    std::size_t settings = 0;
    std::size_t changed = 0;
    bool changes_game = false;
};

struct PluginView {
    LoadState load;
    Glyph glyph = Glyph::On;
    EntryLine line;
    std::size_t pending = 0;  // changes that wait for the next start
    std::size_t changed = 0;  // settings away from their default
    std::vector<std::string> needed_by;  // plugins that are on and require it, by id
    bool needs_attention = false;
    bool in_this_game = false;  // runs in the overlay's game (Game::None: loads)
};

// Where a plugin or module that changes the game runs: the segmented control.
enum class Where { Off, Normal, PlusIronman, PlusMultiplayer, Everywhere };

const PluginSnap* find_plugin(const Snapshot& snapshot, std::string_view id);
const ModuleSnap* find_module(const PluginSnap& plugin, std::string_view id);

// What loads at the next start, with the saved files.
LoadState load_state(const Snapshot& snapshot, const PluginSnap& plugin);
PluginView view(const Snapshot& snapshot, const PluginSnap& plugin);

// A module in the overlay's game, from what is in effect (the overlay's
// "two states", ui-guidelines §5): it runs, it is off, it is off with its
// plugin, or it is kept out of this ironman or multiplayer game.
enum class InGame { Runs, Off, OffWithPlugin, KeptOutOfIronman, KeptOutOfMultiplayer };
InGame module_in_game(const Snapshot& snapshot, const PluginSnap& plugin, const ModuleSnap& module);

// Changes that wait for the next start: switches, opt-ins and approvals, and
// settings whose saved value is not in effect. A change is counted where it
// was made: a plugin turned off is one change, not one per module.
std::size_t pending_count(const PluginSnap& plugin);
std::size_t pending_count(const Snapshot& snapshot);
bool setting_pending(const SettingSnap& setting);
bool setting_changed(const SettingSnap& setting);
// Plugins from mods waiting for a first approval or an update's (the tab's label).
std::size_t to_approve(const Snapshot& snapshot);

Where where(const Switches& switches);
Switches with_where(Switches switches, Where where);
bool where_offered(const Offers& offers, Where where);
// The edits that move a plugin (module "") or a module to `where`.
std::vector<Edit> set_where(const PluginSnap& plugin, std::string_view module, Where where);
// The switch edit, or nothing when it is already so.
std::vector<Edit> set_switch(const PluginSnap& plugin, std::string_view module, Which which, bool on);
std::vector<Edit> set_setting(const PluginSnap& plugin, std::string_view setting, std::string_view value);

// Turning on a plugin whose dependencies are off: the plugins it turns on,
// dependencies first, each with the plugin that needs it. `missing` lists
// required plugins that are not installed, which no switch brings back.
struct Chain {
    struct Step {
        std::string id;
        std::string needed_by;  // "" for the plugin turned on itself
    };
    std::vector<Step> steps;
    std::vector<std::string> missing;
};
Chain turn_on_chain(const Snapshot& snapshot, std::string_view id);
std::vector<Edit> edits_of(const Snapshot& snapshot, const Chain& chain);

// Resets. One setting acts at once; the others are confirmed with their list.
std::vector<Edit> reset_setting(const PluginSnap& plugin, std::string_view setting);
std::vector<Edit> reset_module_settings(const PluginSnap& plugin, std::string_view module);
// Settings, modules' switches and opt-ins, and the plugin's opt-ins; never the
// plugin's own switch and never an approval. Module switches go back to their
// `default_on`, opt-ins to not allowed.
std::vector<Edit> reset_plugin(const PluginSnap& plugin);
// Back to what is in effect: switches, opt-ins and settings that wait (not approvals).
std::vector<Edit> undo_pending(const PluginSnap& plugin);
std::vector<Edit> undo_pending(const Snapshot& snapshot);

// --- Find and filter (ui-guidelines §5) -------------------------------------

struct Match {
    enum class Where { None, Name, Module, Setting } where = Where::None;
    std::string what;                   // the module's name or the setting's label, for the third line
    std::vector<std::string> modules;   // matching modules (to mark, and open their groups)
    std::vector<std::string> settings;  // matching settings, by name
};
// Case is ignored for ASCII letters; an empty query matches as Name.
Match find(const PluginSnap& plugin, std::string_view query);

enum Chip : unsigned {
    ChipNone = 0,
    ChipAttention = 1u << 0,
    ChipChangesGame = 1u << 1,
    ChipChanged = 1u << 2,
    ChipPending = 1u << 3,
    ChipOff = 1u << 4
};
bool has_chip(const Snapshot& snapshot, const PluginSnap& plugin, Chip chip);
// A plugin shows when it matches the query and holds every chip that is on.
bool passes(const Snapshot& snapshot, const PluginSnap& plugin, std::string_view query, unsigned chips);
// How many plugins hold each chip (shown only while it holds one or is on).
std::size_t chip_count(const Snapshot& snapshot, Chip chip);

// --- Sections and layout ----------------------------------------------------

struct SectionCounts {
    std::size_t plugins = 0;
    std::size_t attention = 0;   // ▲
    std::size_t cannot_load = 0; // ✕
    std::size_t waits_for_approval = 0;
};
SectionCounts section_counts(const Snapshot& snapshot, Origin origin);

// Settings of a plugin (module "") or a module, in the order to draw them:
// groups as in settings_order.hpp, the advanced ones apart.
struct SettingsLayout {
    std::vector<std::size_t> shown;     // indices into PluginSnap::settings
    std::vector<std::size_t> advanced;  // folded into "Advanced (n)"
};
SettingsLayout settings_layout(const PluginSnap& plugin, std::string_view module);

// Modules by group, in the order of each group's first module.
struct ModuleGroup {
    std::string name;  // "" for modules without a group
    std::vector<std::size_t> modules;  // indices into PluginSnap::modules
    std::size_t on = 0;                // saved
};
std::vector<ModuleGroup> module_groups(const PluginSnap& plugin);
// A plugin with more than 12 modules starts with its groups folded.
inline constexpr std::size_t kFoldGroupsAbove = 12;

}  // namespace anvil::ui
