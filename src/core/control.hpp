#pragma once

#include "control/anvil_control.h"
#include "crash_history.hpp"
#include "gameplay_policy.hpp"
#include "mod_plugins.hpp"
#include "plugin_translation.hpp"
#include "resolver.hpp"
#include "settings.hpp"

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct AnvilPlugin;

namespace anvil {

class PluginRecord;
class PluginSet;
class PluginSettings;

// What anvil.control reports on. The runtime fills it once the plugins are
// resolved; the tests build their own.
struct ControlSources {
    std::filesystem::path anvil_dir;  // anvil/: anvil.toml, anvil.log, crashes/, data/anvil/
    const char* anvil_version = "";
    const char* game_build = nullptr;
    // The plugin set of this start; may be null. Its outcomes are reported
    // from loading_done on.
    const PluginSet* plugins = nullptr;
    const GameplayPolicy* policy = nullptr;  // may be null
    std::vector<std::string> disabled;       // disabled_plugins at this start
    // The other lists of anvil.toml as this start read them (after the move
    // of retired settings, ADR 0013): the opt-ins and the module switches.
    std::vector<std::string> allow_ironman;
    std::vector<std::string> allow_multiplayer;
    std::vector<std::string> enabled_modules;
    std::vector<std::string> disabled_modules;
    // anvil/data/anvil/last-start.toml as this start wrote it; "" when none.
    std::string last_start;
    bool safe_mode = false;
    unsigned unfinished_starts = 0;
    // The newest crash report written before this start ("": none). Read at
    // start-up, before this process could write one.
    std::string previous_crash;
    // The plugins in the playset's mods as the start decided them (design
    // §14); may be null. Does not change after start-up.
    const ModDiscovery* mods = nullptr;
    // The player's language for the overlay (i18n::language_code).
    std::string language = "en";
    // The plugins' translations into that language (design §5.4): by plugin
    // id, and for the plugins in mods by their index in mods->plugins. A
    // plugin without one is not listed.
    std::map<std::string, PluginTranslation, std::less<>> translations;
    std::map<uint32_t, PluginTranslation> mod_translations;
};

// The core's private service for anvil-overlay (src/control/anvil_control.h,
// design §15). One per process; service() is what the core registers as
// "anvil.control" v1. Every call may come from any thread.
class Control {
public:
    explicit Control(ControlSources sources);
    ~Control();
    Control(const Control&) = delete;
    Control& operator=(const Control&) = delete;

    // The table of function pointers. They reach this object through a
    // process-wide pointer that the constructor sets (and the destructor
    // clears), so only one Control exists at a time.
    const AnvilControlService* service() const;

    // Every plugin was processed: the plugins' outcomes are final from now on
    // and the service reports them.
    void loading_done();

    void status(AnvilControlStatus& out) const;
    bool plugin(uint32_t index, AnvilControlPlugin& out) const;
    // plugin() of the plugin with this handle, with a module's key, flags and
    // state for a module's handle; false for any other pointer.
    bool plugin_of(const AnvilPlugin* plugin, AnvilControlPlugin& out) const;
    std::optional<std::string> warning(uint32_t index) const;
    PluginSettings* settings_of(std::string_view plugin_id) const;
    // The previous crash, loaded on the first call; that call also marks it
    // as shown in anvil/data/anvil/crash-shown.txt.
    const AnvilControlCrash* previous_crash();
    std::string log_tail(uint32_t max_lines) const;
    const char* language() const { return sources_.language.c_str(); }
    // `english` in the player's language from the plugin's translation, or
    // `english` itself. The result lives as long as this object.
    const char* plugin_text(std::string_view plugin_id, const char* english) const;
    const char* mod_plugin_text(uint32_t index, const char* english) const;
    AnvilResult disable_next_start(std::string_view plugin_id, std::string& error);
    bool disabled_next_start(std::string_view plugin_id) const;

    // Plugins in mods (design §14). Approving or revoking changes trust.toml
    // for the next start; what this start loaded stays.
    uint32_t mod_plugin_count() const;
    bool mod_plugin(uint32_t index, AnvilControlModPlugin& out) const;
    AnvilResult approve_mod_plugin(uint32_t index, std::string_view hash, std::string& error);
    AnvilResult revoke_mod_plugin(uint32_t index, std::string& error);

    // Modules (ADR 0011): of a plugin of this start by id, with their state at
    // this start, and of a plugin in a mod by its index, as declared.
    uint32_t module_count(std::string_view plugin_id) const;
    bool module(std::string_view plugin_id, uint32_t index, AnvilControlModule& out) const;
    uint32_t mod_plugin_module_count(uint32_t index) const;
    bool mod_plugin_module(uint32_t index, uint32_t module, AnvilControlModule& out) const;

    // setting_info of a resolved plugin, with the index of its module.
    bool setting_info(std::string_view plugin_id, AnvilSetting setting, AnvilControlSetting& out) const;
    // ADR 0013, point 14: a multiplayer session runs, and the setting counts
    // for the game, so the overlay may not change it.
    bool multiplayer_session() const;
    bool setting_locked(std::string_view plugin_id, AnvilSetting setting) const;

    // Switches and opt-ins for the next start (ADR 0013, point 7): `key` is a
    // plugin id of this start or "<plugin>.<module>" of a resolved plugin,
    // `which` an ANVIL_CONTROL_SWITCH_*.
    bool switch_state(std::string_view key, uint32_t which, AnvilControlSwitch& out) const;
    AnvilResult switch_set(std::string_view key, uint32_t which, bool on, std::string& error);
    const std::string& last_start() const { return sources_.last_start; }
    // anvil-overlay and the plugins it needs, by id: the overlay does not
    // turn them off.
    bool needed_by_overlay(std::string_view plugin_id) const { return needed_.contains(plugin_id); }

private:
    struct CrashCache {
        CrashSummary summary;
        std::string plugins;
        std::string report;
        std::wstring path;
        AnvilControlCrash view{};
    };

    // A plugin of this start that Anvil loads, by id.
    const PluginRecord* loadable(std::string_view plugin_id) const;

    // What a switch key names: a plugin of this start, and for a module key
    // the module of a resolved plugin.
    struct Target {
        const PluginRecord* plugin = nullptr;
        const Module* module = nullptr;  // null: the plugin's own switch
        std::string key;
    };
    std::optional<Target> target(std::string_view key) const;
    // The lists of anvil.toml: as this start read them, and now.
    struct Lists {
        std::set<std::string, std::less<>> ironman, multiplayer, enabled_modules, disabled_modules;
    };
    bool state_of(const Target& t, uint32_t which, AnvilControlSwitch& out) const;  // mutex_ held
    // The hash line of a handle (ADR 0013): "<key> <version>" and the values
    // in effect of the owner's settings that count, names sorted.
    std::string hash_line(const PluginRecord& p, const std::string& key, const Module* module) const;

    ControlSources sources_;
    std::vector<std::string> versions_;  // by plugin index; "" for a skipped one
    // The reasons as messages (i18n::serialize), by plugin index: the skipped
    // plugins' made once, the others' at loading_done.
    std::vector<std::string> messages_;
    std::atomic<bool> loading_done_{false};
    mutable std::mutex mutex_;
    std::set<std::string, std::less<>> disabled_next_;  // under mutex_
    Lists start_;                                       // fixed
    Lists next_;                                        // under mutex_
    std::set<std::string, std::less<>> needed_;         // fixed
    bool crash_loaded_ = false;                         // under mutex_
    std::unique_ptr<CrashCache> crash_;                 // under mutex_, then fixed

    // The strings of each mod plugin's view, made once.
    struct ModView {
        std::string name, version, authors, description, repository, issues, others;
        std::string approved_version, approved_hash, approved_date;
        std::string reason_message;
        uint32_t symbols = 0;
        std::vector<std::string> module_keys;  // "<plugin>.<module>", in plugin.toml order
    };
    // Which mod and hash trust.toml binds an id to now ("" mod: none).
    struct Binding {
        std::string mod;
        std::string hash;
    };
    std::vector<ModView> mod_views_;
    std::map<std::string, Binding, std::less<>> bindings_;  // under mutex_
    std::string running_hash(const std::string& id) const;  // the approved copy this start loaded for the id
};

// The value as text, as parse_setting_text reads it back: true/false, a
// decimal int, a number, an option's name, the string, a key name.
std::string setting_text(const Setting& setting, const SettingValue& value);

// The hash of design §13 over plugin ids and versions ("id version" lines in
// byte order), shown as "XXXX-XXXX-XXXX-XXXX" (the start of a SHA-256);
// "" for none.
std::string plugin_set_hash(std::vector<std::string> ids_and_versions);

}  // namespace anvil
