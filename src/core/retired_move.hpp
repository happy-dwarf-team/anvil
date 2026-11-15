#pragma once

#include "anvil_toml_edit.hpp"
#include "discovery.hpp"
#include "manifest.hpp"
#include "settings.hpp"

#include <anvil/anvil_plugin.h>

#include <filesystem>
#include <optional>
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
// it waits. With `write_files` false (a benchmark start, which never writes
// anvil.toml: docs/spec/benchmark-v1.md §2) the values apply to `lists` for
// this start only and both files stay, so a later start moves them.
struct RetiredMoveNote {
    AnvilLogLevel level = ANVIL_LOG_INFO;
    std::string message;
};

std::vector<RetiredMoveNote> move_retired_settings(const Manifest& manifest, const SettingsFile& schema,
                                                   const std::filesystem::path& player_file,
                                                   const std::filesystem::path& anvil_toml,
                                                   AnvilSettings& lists, bool write_files = true);

// The same move in two halves, so the start plan (ADR 0014) can say what a
// start writes before it writes it. plan_retired_settings reads nothing:
// `player_text` is the player's file (nullopt: there is none), and `lists`
// gets the edits as they apply at the start. carry_out_retired_move writes
// anvil.toml, then the player's file, and returns what to log.
struct RetiredMovePlan {
    std::vector<ListEdit> edits;       // into anvil.toml, in one write; empty: nothing to write there
    bool writes_player_file = false;   // the player's file loses the moved values
    std::string player_text;           // its text after the move; "" removes the file
    std::string removed;               // "name = value, ..." of the values it loses
    // What each half of the move did (logged once the writes are done), or
    // why the move waits (then nothing is written).
    std::vector<RetiredMoveNote> notes;
    bool writes() const { return !edits.empty() || writes_player_file; }
};

// With `write_files` false (a benchmark start) the edits apply to `lists`
// and the plan writes nothing; its notes say so.
RetiredMovePlan plan_retired_settings(const Manifest& manifest, const SettingsFile& schema,
                                      const std::optional<std::string>& player_text, AnvilSettings& lists,
                                      bool write_files = true);

// `anvil_toml_written`, if given: false when anvil.toml could not be written
// (the player's file then stays as it was, so the next start tries again).
std::vector<RetiredMoveNote> carry_out_retired_move(const RetiredMovePlan& plan, const std::filesystem::path& player_file,
                                                    const std::filesystem::path& anvil_toml,
                                                    bool* anvil_toml_written = nullptr);

}  // namespace anvil
