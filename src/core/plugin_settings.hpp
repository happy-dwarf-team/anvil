#pragma once

#include "settings.hpp"

#include <anvil/anvil_plugin.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil {

// The settings of one plugin while the game runs (design §11): the values the
// plugin reads, the player's saved values and the subscriptions. Reads are
// lock-free (the host's setting_* calls, from any thread); changes take a
// lock and run the callbacks on the changing thread after it is released.
class PluginSettings {
public:
    // One line for the log, in the plugin's name.
    struct Note {
        AnvilLogLevel level = ANVIL_LOG_INFO;
        std::string message;
    };
    // The value of an environment variable (UTF-8), nullopt when unset.
    using Environment = std::function<std::optional<std::string>(const std::string& name)>;

    PluginSettings() = default;
    PluginSettings(const PluginSettings&) = delete;
    PluginSettings& operator=(const PluginSettings&) = delete;

    // Before the plugin loads: the defaults of `schema`, then the player's
    // values from `player_file` (anvil/data/<id>/settings.toml; an invalid one
    // keeps the default), then the ANVIL_<ID>_<NAME> variables (empty
    // `environment`: the process environment). Returns what to log: every
    // warning and every override. Called once.
    std::vector<Note> init(SettingsFile schema, const std::filesystem::path& player_file, std::string_view plugin_id,
                           const Environment& environment = {});

    const SettingsFile& schema() const { return schema_; }
    const std::filesystem::path& player_file() const { return player_file_; }
    size_t size() const { return slots_.size(); }
    // Warnings of init (the overlay shows them with the log).
    const std::vector<std::string>& warnings() const { return warnings_; }

    // The host's getters. An unknown setting reads 0, 0.0 or "". setting_int
    // also reads a bool (0/1), an enum (the option's index) and a key (the
    // code); setting_float an int; setting_string an enum (the option's name)
    // and a key ("Ctrl+F12"). Other combinations read 0, 0.0 or "".
    int get_bool(AnvilSetting id) const;
    int64_t get_int(AnvilSetting id) const;
    double get_float(AnvilSetting id) const;
    // Valid for the process: every text a setting held is kept.
    const char* get_string(AnvilSetting id) const;

    // The value the plugin reads now, and the one saved for the next start
    // (they differ after a change of a restart = true setting, or while an
    // environment variable overrides it). nullopt for an unknown id.
    std::optional<SettingValue> current(AnvilSetting id) const;
    std::optional<SettingValue> saved(AnvilSetting id) const;

    // subscribe_setting: NOT_DECLARED for an unknown id, WRONG_KIND for a
    // restart = true setting, INVALID_ARGUMENT without a callback.
    AnvilResult subscribe(AnvilSetting id, AnvilSettingCallback callback, void* context);
    // A plugin whose load failed gets no more callbacks.
    void drop_subscriptions();

    struct Change {
        AnvilResult result = ANVIL_OK;  // NOT_DECLARED, INVALID_ARGUMENT (the value does not fit)
        std::string error;              // why, when result is not OK, or why the file was not written
        bool applied = false;           // the plugin reads the new value (restart = false)
        bool saved = false;             // the player's file holds it
        size_t notified = 0;            // callbacks run
    };
    // The change of design §11, for the overlay and other in-game editors:
    // checks the value, saves it in the player's file (only values that
    // differ from the default), and for a restart = false setting makes it
    // the current value and runs the subscribers' callbacks on this thread.
    // A restart = true setting changes at the next start. A change replaces an
    // environment override for the rest of the session.
    Change set(AnvilSetting id, const SettingValue& value);
    Change set_text(AnvilSetting id, std::string_view text);  // as parse_setting_text reads it
    Change reset(AnvilSetting id);                              // to the default

private:
    struct Slot {
        std::atomic<int64_t> integer{0};
        std::atomic<double> real{0.0};
        std::atomic<const char*> text{""};
        std::vector<std::unique_ptr<std::string>> texts;  // every text it held, for get_string
        struct Subscriber {
            AnvilSettingCallback callback;
            void* context;
        };
        std::vector<Subscriber> subscribers;
    };

    void store(const Setting& setting, Slot& slot, const SettingValue& value);  // under mutex_ or before load

    SettingsFile schema_;
    std::filesystem::path player_file_;
    std::vector<std::unique_ptr<Slot>> slots_;  // by id
    std::vector<SettingValue> current_;         // by id, under mutex_
    std::vector<SettingValue> saved_;           // by id, under mutex_
    std::vector<std::string> warnings_;
    mutable std::mutex mutex_;
};

}  // namespace anvil
