#pragma once

#include "discovery.hpp"
#include "manifest.hpp"
#include "settings.hpp"

#include <anvil/anvil_plugin.h>

#include <filesystem>
#include <string>
#include <vector>

namespace anvil {

// ADR 0013, point 4: the one-time move of a plugin's retired settings, at the
// start, before its modules are resolved. For every value of the player's
// file (`player_file`, anvil/data/<id>/settings.toml) named as a
// [retired.<name>] of `schema`: a value in its `off` or `on` sets the switch
// of "<plugin>.<module>" by the rule of module_switch_edits (the Manager's
// and the overlay's), so the module is never in both lists; one in its
// `ironman` or `multiplayer` adds the key to allow_ironman or
// allow_multiplayer. All of them go into `anvil_toml` in one write and into
// `lists`, this start's lists, so they apply at this start. Every such value
// is removed from the player's file. anvil.toml is written first; when that
// fails the player's file stays as it was, so the next start tries again.
// Returns what to log, in the plugin's name: each half of the move, or why
// it waits.
struct RetiredMoveNote {
    AnvilLogLevel level = ANVIL_LOG_INFO;
    std::string message;
};

std::vector<RetiredMoveNote> move_retired_settings(const Manifest& manifest, const SettingsFile& schema,
                                                   const std::filesystem::path& player_file,
                                                   const std::filesystem::path& anvil_toml,
                                                   AnvilSettings& lists);

}  // namespace anvil
