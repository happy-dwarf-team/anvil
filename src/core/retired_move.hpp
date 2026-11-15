#pragma once

#include "anvil_toml_edit.hpp"
#include "discovery.hpp"
#include "manifest.hpp"
#include "settings.hpp"

#include <anvil/anvil_plugin.h>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil {

// The record of moved retired settings, anvil/data/anvil/retired-moved.toml:
// values whose move into anvil.toml was written while the player's file
// could not be (read-only, or held open by another program), so they are
// still in it. A start, or the Manager before a switch, that finds such a
// value with the same text only tries again to remove it from the player's
// file: anvil.toml has its move already, and the player may have switched
// the module since. A value whose text changed is a new one and moves as
// usual. A plugin's entries go once its file no longer holds them.
struct MovedRetired {
    std::string name;
    std::string value;  // as the player's file wrote it (RetiredMove::Item::value)

    bool operator==(const MovedRetired&) const = default;
};
using MovedRetiredRecord = std::map<std::string, std::vector<MovedRetired>>;  // by plugin id

std::filesystem::path moved_retired_file(const std::filesystem::path& anvil_dir);
// Empty when the file is missing, does not parse or is another format: the
// values then move again, as without a record.
MovedRetiredRecord read_moved_retired(const std::filesystem::path& file);
std::optional<MovedRetiredRecord> parse_moved_retired(std::string_view text);
// "" when no plugin has an entry.
std::string moved_retired_text(const MovedRetiredRecord& record);
// Through a temporary file; an empty record removes the file.
bool write_moved_retired(const std::filesystem::path& file, const MovedRetiredRecord& record, std::string& error);

// ADR 0013, point 4: the one-time move of a plugin's retired settings, read
// from the files as they are now and carried out at once. A start plans it
// (plan_retired_settings, ADR 0014) and carries the plan out; the Manager
// makes it this way before a switch (carry_out_retired_move_now), so a
// setting saved since the plan stays. For every value of the player's
// file (`player_file`, anvil/data/<id>/settings.toml) named as a
// [retired.<name>] of `schema`: a value in its `off` or `on` sets the switch
// of "<plugin>.<module>" by the rule of module_switch_edits (the Manager's
// and the overlay's), so the module is never in both lists; when values of
// one module disagree (one in an `off`, another in an `on`), off wins
// whatever their order in the file; one in its
// `ironman` or `multiplayer` adds the key to allow_ironman or
// allow_multiplayer. All of them go into `anvil_toml` in one write and into
// `lists`, this start's lists, so they apply at this start. Every such value
// is removed from the player's file. anvil.toml is written first; when that
// fails the player's file stays as it was, so the next start tries again.
// Returns what to log, in the plugin's name: each half of the move, or why
// it waits. With `write_files` false (a benchmark start, which never writes
// anvil.toml: docs/spec/benchmark-v1.md §2) the values apply to `lists` for
// this start only and both files stay, so a later start moves them.
// `anvil_toml_written` false: anvil.toml could not be written.
// With a `record` file (moved_retired_file), a value it holds for the
// plugin with the same text is only removed (above), and the record is
// updated as carry_out_retired_move says. `settled`, if given: true when a
// later start will not move these values again (anvil.toml was written, and
// the player's file lost them or the record holds them).
struct RetiredMoveNote {
    AnvilLogLevel level = ANVIL_LOG_INFO;
    std::string message;
};

std::vector<RetiredMoveNote> move_retired_settings(const Manifest& manifest, const SettingsFile& schema,
                                                   const std::filesystem::path& player_file,
                                                   const std::filesystem::path& anvil_toml,
                                                   AnvilSettings& lists, bool write_files = true,
                                                   bool* anvil_toml_written = nullptr,
                                                   const std::filesystem::path& record = {}, bool* settled = nullptr);

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
    // The values whose move anvil.toml has once the plan is written (each in
    // an `off`, `on`, `ironman` or `multiplayer` list): what the record of
    // moved retired settings keeps for the plugin when its file cannot be
    // written.
    std::vector<MovedRetired> moved;
    // The record has values of the plugin that its file no longer holds
    // (the file is gone, or holds no retired value): they leave the record.
    bool forget_recorded = false;
    bool writes() const { return !edits.empty() || writes_player_file; }
};

// With `write_files` false (a benchmark start) the edits apply to `lists`
// and the plan writes nothing; its notes say so. `recorded`: the plugin's
// entries of the record of moved retired settings; such a value is only
// removed from the player's file, and anvil.toml's switch stays as it is.
RetiredMovePlan plan_retired_settings(const Manifest& manifest, const SettingsFile& schema,
                                      const std::optional<std::string>& player_text, AnvilSettings& lists,
                                      bool write_files = true, const std::vector<MovedRetired>& recorded = {});

// `anvil_toml_written`, if given: false when anvil.toml could not be written
// (the player's file then stays as it was, so the next start tries again).
// `recorded`, if given, is the plugin's entries of the record of moved
// retired settings, updated for what was written: none once the player's
// file lost the values, the plan's `moved` when only anvil.toml was
// written, unchanged when nothing was. The caller writes the record.
std::vector<RetiredMoveNote> carry_out_retired_move(const RetiredMovePlan& plan, const std::filesystem::path& player_file,
                                                    const std::filesystem::path& anvil_toml,
                                                    bool* anvil_toml_written = nullptr,
                                                    std::vector<MovedRetired>* recorded = nullptr,
                                                    bool* player_file_written = nullptr);

}  // namespace anvil
