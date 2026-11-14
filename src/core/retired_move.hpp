#pragma once

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
// [retired.<name>] of `schema`: a value in its `off` adds "<plugin>.<module>"
// to disabled_modules of `anvil_toml` (and to `disabled_modules`, this start's
// list, so the module is off at this start), and every such value is removed
// from the player's file. anvil.toml is written first; when that fails the
// player's file stays as it was, so the next start tries again. Returns what
// to log, in the plugin's name: each half of the move, or why it waits.
struct RetiredMoveNote {
    AnvilLogLevel level = ANVIL_LOG_INFO;
    std::string message;
};

std::vector<RetiredMoveNote> move_retired_settings(const Manifest& manifest, const SettingsFile& schema,
                                                   const std::filesystem::path& player_file,
                                                   const std::filesystem::path& anvil_toml,
                                                   std::vector<std::string>& disabled_modules);

}  // namespace anvil
