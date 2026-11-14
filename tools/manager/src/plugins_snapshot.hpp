// The Manager's side of the Plugins tab's view model (ADR 0013, point 9):
// builds the model's snapshot from what `anvil game` found in the folder,
// anvil.toml and the players' settings files, and keeps beside each plugin
// what the Manager needs to draw and write it (its manifest, schema and
// values, its folder or its mod).
#pragma once

#include "cli/game.hpp"
#include "core/discovery.hpp"
#include "core/manifest.hpp"
#include "core/settings.hpp"
#include "ui/plugin_model.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace anvil::manager {

struct PluginSource {
    std::string folder;                      // in anvil/plugins; "" for a plugin from a mod
    const cli::GameModPlugin* mod = nullptr; // into the report the snapshot was built from
    std::optional<Manifest> manifest;
    bool standard = false;                   // comes with Anvil
    std::optional<SettingsFile> schema;
    std::vector<SettingValue> values;        // saved, one per setting of `schema`
    std::filesystem::path player_file;       // anvil/data/<id>/settings.toml
    std::string settings_error;              // the plugin's settings.toml cannot be read
    std::vector<std::string> settings_warnings;  // lines of the player's file that were ignored
};

struct PluginsSnapshot {
    anvil::ui::Snapshot model;
    std::vector<PluginSource> sources;  // one per model.plugins, same order
};

// `text(folder, english)` gives a plugin's text in the player's language.
using PluginText = std::function<std::string(const std::filesystem::path&, const std::string&)>;

// The plugins of anvil/plugins in discovery order, then those of enabled
// mods. Nothing waits for the next start until the core's start record is read.
PluginsSnapshot build_plugins_snapshot(const std::filesystem::path& game_dir, const cli::GameReport& report,
                                       const AnvilSettings& toml, const std::vector<std::string>& standard_plugins,
                                       const PluginText& text);

// The canonical text of a value, as the model compares values.
std::string value_text(const Setting& setting, const SettingValue& value);

// Applies one setting edit of the model to `source`'s values: the edit's
// value is the canonical text of the default or of a value it already knows.
bool apply_setting_edit(PluginSource& source, const anvil::ui::Edit& edit, std::string& error);

}  // namespace anvil::manager
