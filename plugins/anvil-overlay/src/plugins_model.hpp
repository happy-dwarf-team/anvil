// The overlay's side of the Plugins tab's view model (ADR 0013, point 9): the
// snapshot built from anvil.control, and beside each plugin what the overlay
// needs to draw and write it (its settings' handles and types, its mod entry).
// The Manager builds the same snapshot from the files
// (tools/manager/src/plugins_snapshot.hpp).
#pragma once

#include "control_client.hpp"
#include "ui/plugin_model.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace overlay {

struct OverlaySetting {
    AnvilSetting id = 0;
    AnvilControlSetting info{};
    std::vector<std::pair<std::string, std::string>> options;  // enum: (name, label in the player's language)
};

struct OverlaySource {
    bool known = false;              // its plugin.toml was read: switches, modules and settings can be shown
    std::string refusal;             // the switch_state refusal of its own switch ("needed", ...)
    std::optional<size_t> mod;       // index into the mod rows it was built from
    std::optional<size_t> row;       // index into the plugin rows it was built from
    std::vector<OverlaySetting> settings;  // one per model settings, same order
};

struct OverlayPlugins {
    anvil::ui::Snapshot model;
    std::vector<OverlaySource> sources;  // one per model.plugins, same order
};

// `standard` lists the ids of the plugins that come with Anvil.
OverlayPlugins build_overlay_plugins(const ControlClient& control, const AnvilControlStatus& status,
                                     const std::vector<PluginRow>& plugins, const std::vector<ModRow>& mods,
                                     const std::vector<std::string>& standard);

// The ids of the plugins that come with Anvil (the build's plugins/ folders).
std::vector<std::string> standard_plugin_ids();

}  // namespace overlay
