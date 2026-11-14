#pragma once

#include "manifest.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil {

// A plugin's settings.toml (design §5.3, §11): the declarations, their
// defaults and limits. The player's values live elsewhere
// (anvil/data/<id>/settings.toml) and are checked against these.
enum class SettingType { Bool, Int, Float, Enum, String, Key };

const char* setting_type_name(SettingType type);

// One value. Which field holds it depends on the type: `integer` for bool
// (0 or 1), int, enum (the option's index) and key (the key code), `real` for
// float, `text` for string.
struct SettingValue {
    int64_t integer = 0;
    double real = 0.0;
    std::string text;

    bool operator==(const SettingValue&) const = default;
};

// An option of an enum setting: `name` is what the files hold, `label` what
// the player sees (the name when not declared).
struct SettingOption {
    std::string name;
    std::string label;
};

struct Setting {
    uint32_t id = 0;  // declaration order in the file: the plugin's AnvilSetting
    std::string name;
    uint32_t line = 0;  // of its [setting.<name>] table in settings.toml
    SettingType type = SettingType::Bool;
    SettingValue default_value;
    // int and float: optional limits and the editor's increment (not a rule
    // for values).
    std::optional<SettingValue> min, max, step;
    std::vector<SettingOption> options;  // enum: in declaration order
    std::string label;
    std::string description;
    std::string group;  // empty: ungrouped
    // true: a new value applies at the next start; false: at once, and the
    // plugin may subscribe to changes.
    bool restart = true;
    // ADR 0013. `module`: the [[modules]] id the setting belongs to (empty:
    // the plugin's), checked against plugin.toml by settings_module_error.
    std::string module;
    uint32_t module_line = 0;  // of its `module` key
    bool advanced = false;     // folded behind "Advanced (n)"
    // As declared; setting_counts gives the rule in effect.
    std::optional<bool> affects_gameplay;
};

// A value of a retired setting's `off`, as TOML wrote it.
struct RetiredValue {
    enum class Kind { Bool, Int, Float, String } kind = Kind::Bool;
    int64_t integer = 0;  // bool (0 or 1) and int
    double real = 0.0;
    std::string text;
};

// [retired.<name>] (ADR 0013, point 4): a setting an earlier version had,
// which the switch of `module` replaces. At the start the core moves a value
// in `off` from the player's file into disabled_modules.
struct RetiredSetting {
    std::string name;
    std::string module;
    std::vector<RetiredValue> off;
    uint32_t line = 0;         // of its [retired.<name>] table
    uint32_t module_line = 0;  // of its `module` key
};

struct SettingsFile {
    std::vector<Setting> settings;  // in declaration order (index == id)
    std::vector<RetiredSetting> retired;  // in file order

    const Setting* find(std::string_view name) const;
    const Setting* get(uint32_t id) const { return id < settings.size() ? &settings[id] : nullptr; }
    const RetiredSetting* find_retired(std::string_view name) const;
};

struct SettingsFileResult {
    std::optional<SettingsFile> file;
    std::string error;  // set when file is empty; starts with "settings.toml: "
    uint32_t line = 0;  // the line of settings.toml the error is about; 0 = the whole file
};

// Parses settings.toml (design §5.3). Unknown keys, values of the wrong type,
// keys that do not fit the type and a default outside the limits are errors.
SettingsFileResult parse_settings_text(std::string_view text);
SettingsFileResult load_settings(const std::filesystem::path& file);

// The schema error of a `module` (of a setting or of a [retired.<name>]) that
// is not an id of the plugin's [[modules]]; empty when every one is. `line` is
// set to the line of that key. The loader skips the plugin for it, as for
// any other schema error (ADR 0013, point 1).
std::string settings_module_error(const SettingsFile& file, const Manifest& manifest, uint32_t& line);

// Whether the setting counts for the game (ADR 0013, point 14): its owner
// (its module, or the plugin's own handle) affects gameplay, and it does not
// say affects_gameplay = false; a `key` setting counts only when it says
// affects_gameplay = true.
bool setting_counts(const Setting& setting, const Manifest& manifest);

// The one-time move of retired settings (ADR 0013, point 4) on the text of
// the player's file (anvil/data/<id>/settings.toml): every top-level value
// named as a [retired.<name>] is removed; one that is in its `off` (the same
// TOML type; an int and a float compare as numbers) also turns its module
// off.
struct RetiredMove {
    struct Item {
        std::string name;
        std::string module;
        std::string value;  // as the file wrote it, e.g. "off" or false
        uint32_t line = 0;  // in the player's file
        bool turns_off = false;
    };
    std::vector<Item> items;  // in file order; empty: nothing to do
    // The file without them. Empty when nothing but comments and blank lines
    // is left: then the file is removed, as write_player_settings does.
    std::string text;
};
// nullopt with `error` when the text does not parse (the move waits; the
// player's file is ignored as before) or the edit would not read back as the
// same file without those values.
std::optional<RetiredMove> plan_retired_move(const SettingsFile& schema, std::string_view player_text,
                                             std::string& error);

// Writes `text` to `file` through a temporary file in the same folder and
// replaces it in one step; an empty `text` removes the file. False with
// `error` on failure.
bool replace_text_file(const std::filesystem::path& file, const std::string& text, std::string& error);

// Why `value` is not valid for the setting (wrong type, out of range, not an
// option, not a key); empty when it is.
std::string check_setting_value(const Setting& setting, const SettingValue& value);

// A value from text, as an ANVIL_<ID>_<NAME> environment variable gives it:
// bool true/false/1/0, a decimal int, a float, an enum option's name, any
// string, a key binding. Checked with check_setting_value; nullopt and
// `error` set when it does not fit.
std::optional<SettingValue> parse_setting_text(const Setting& setting, std::string_view text, std::string& error);

// A TOML basic string: "text" with its escapes.
std::string toml_string(std::string_view text);

// The value as a TOML value: true, 1024, 0.5, "on", "Ctrl+F12".
std::string setting_toml(const Setting& setting, const SettingValue& value);

// Key bindings (type "key"): "Ctrl+Shift+F12", "A", "Numpad5"; "" is no
// binding (code 0). The code is the Windows virtual-key code in bits 0-7 with
// the modifiers above it, as the Win32 hot-key control encodes them.
inline constexpr int64_t kKeyShift = 0x100;
inline constexpr int64_t kKeyCtrl = 0x200;
inline constexpr int64_t kKeyAlt = 0x400;

// Names are case-insensitive; modifiers come first, each at most once.
std::optional<int64_t> parse_key(std::string_view text);
// The canonical name ("Ctrl+Shift+F12"); empty for 0 or an unknown code.
std::string key_name(int64_t code);

// ---- the player's values: anvil/data/<id>/settings.toml -----------------------

// `name = value` per setting whose value differs from the default.
struct PlayerSettings {
    // By id: the player's value, or nullopt for the default.
    std::vector<std::optional<SettingValue>> values;
    // One line per value that was ignored (the default applies): a line of
    // the file, then why. A file that does not parse gives one warning.
    std::vector<std::string> warnings;
};

// Reads the player's values. Unknown names (a setting the plugin dropped) and
// invalid values are warnings; the rest is used.
PlayerSettings parse_player_settings(const SettingsFile& schema, std::string_view text);
// A missing file is no values and no warnings.
PlayerSettings load_player_settings(const SettingsFile& schema, const std::filesystem::path& file);

// The file for `values` (by id): only those that differ from the default, in
// declaration order.
std::string player_settings_text(const SettingsFile& schema, const std::vector<SettingValue>& values);
// Writes it through a temporary file, creating the folder. With only default
// values the file is removed. False with `error` on failure.
bool write_player_settings(const SettingsFile& schema, const std::vector<SettingValue>& values,
                           const std::filesystem::path& file, std::string& error);

// ANVIL_<ID>_<NAME>: upper case, '-' in the plugin id becomes '_'.
std::string setting_environment_name(std::string_view plugin_id, std::string_view setting_name);

}  // namespace anvil
