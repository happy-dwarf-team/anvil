#include "retired_move.hpp"

#include "anvil_toml_edit.hpp"
#include "crash_history.hpp"

#include <toml++/toml.hpp>

#include <algorithm>

namespace anvil {

namespace {

constexpr int64_t kMovedRetiredFormat = 1;

std::vector<std::string>* list_of(AnvilSettings& lists, const std::string& key) {
    if (key == "enabled_modules") return &lists.enabled_modules;
    if (key == "disabled_modules") return &lists.disabled_modules;
    if (key == "allow_ironman") return &lists.allow_ironman;
    if (key == "allow_multiplayer") return &lists.allow_multiplayer;
    return nullptr;
}

bool listed(const std::vector<std::string>& list, const std::string& id) {
    return std::find(list.begin(), list.end(), id) != list.end();
}

bool moves_something(const RetiredMove::Item& item) {
    return item.turns_off || item.turns_on || item.allows_ironman || item.allows_multiplayer;
}

}  // namespace

std::filesystem::path moved_retired_file(const std::filesystem::path& anvil_dir) {
    return anvil_dir / "data" / "anvil" / "retired-moved.toml";
}

std::optional<MovedRetiredRecord> parse_moved_retired(std::string_view text) {
    toml::table t;
    try {
        t = toml::parse(text);
    } catch (const toml::parse_error&) {
        return std::nullopt;
    }
    if (t["format"].value<int64_t>() != kMovedRetiredFormat) return std::nullopt;
    MovedRetiredRecord record;
    if (const auto* moved = t["moved"].as_table()) {
        for (const auto& [plugin, node] : *moved) {
            const auto* values = node.as_table();
            if (values == nullptr) continue;
            std::vector<MovedRetired> entries;
            for (const auto& [name, value] : *values) {
                if (auto v = value.value<std::string>()) entries.push_back({std::string(name.str()), *v});
            }
            if (!entries.empty()) record[std::string(plugin.str())] = std::move(entries);
        }
    }
    return record;
}

MovedRetiredRecord read_moved_retired(const std::filesystem::path& file) {
    const std::optional<std::string> text = read_text_file(file, kMaxPluginTomlBytes);
    if (!text) return {};
    return parse_moved_retired(*text).value_or(MovedRetiredRecord{});
}

std::string moved_retired_text(const MovedRetiredRecord& record) {
    std::string body;
    for (const auto& [plugin, entries] : record) {
        if (entries.empty()) continue;
        body += "\n[moved." + toml_string(plugin) + "]\n";
        // By name, as a parse gives them back. The value as the player's file wrote it, kept
        // as text: compared as text.
        std::vector<MovedRetired> sorted = entries;
        std::sort(sorted.begin(), sorted.end(), [](const MovedRetired& x, const MovedRetired& y) { return x.name < y.name; });
        for (const MovedRetired& e : sorted) body += toml_string(e.name) + " = " + toml_string(e.value) + "\n";
    }
    if (body.empty()) return {};
    return "# Retired settings (ADR 0013, point 4) whose move into anvil.toml was written while the player's\n"
           "# data/<id>/settings.toml could not be: the next start only removes these values from it again, and\n"
           "# leaves the module's switch as anvil.toml has it. Anvil writes this file; do not edit it.\n"
           "format = " +
           std::to_string(kMovedRetiredFormat) + "\n" + body;
}

bool write_moved_retired(const std::filesystem::path& file, const MovedRetiredRecord& record, std::string& error) {
    return replace_text_file(file, moved_retired_text(record), error);
}

RetiredMovePlan plan_retired_settings(const Manifest& manifest, const SettingsFile& schema,
                                      const std::optional<std::string>& player_text, AnvilSettings& lists,
                                      bool write_files, const std::vector<MovedRetired>& recorded) {
    RetiredMovePlan plan;
    if (schema.retired.empty()) return plan;
    if (!player_text) {  // no player's file: nothing to move
        plan.forget_recorded = write_files && !recorded.empty();
        return plan;
    }
    std::string_view text = *player_text;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);  // saved by Notepad
    std::string error;
    const std::optional<RetiredMove> move = plan_retired_move(schema, text, error);
    if (!move) {
        plan.notes.push_back({ANVIL_LOG_WARNING, "retired settings not moved from settings.toml: " + error});
        return plan;
    }
    if (move->items.empty()) {
        plan.forget_recorded = write_files && !recorded.empty();
        return plan;
    }
    // Every edit of every value, in file order, against this start's lists,
    // so a later value sees what an earlier one did.
    for (const RetiredMove::Item& item : move->items) {
        const std::string key = manifest.id + "." + item.module;
        const std::string what = "retired setting " + item.name + " = " + item.value + " (settings.toml line " +
                                 std::to_string(item.line) + ")";
        // Moved before, while its file could not be written: anvil.toml has the move, and
        // the player may have switched the module since. Only its removal is left.
        if (moves_something(item) &&
            std::find(recorded.begin(), recorded.end(), MovedRetired{item.name, item.value}) != recorded.end()) {
            plan.notes.push_back({ANVIL_LOG_INFO, what + " was moved into anvil.toml before, when settings.toml could "
                                                         "not be written: module " + key +
                                                         " stays as anvil.toml has it"});
            continue;
        }
        const auto declared = std::find_if(manifest.modules.begin(), manifest.modules.end(),
                                           [&](const Module& m) { return m.id == item.module; });
        const bool on_by_default = declared == manifest.modules.end() || declared->enabled_by_default;
        std::vector<std::pair<std::string, std::vector<ListEdit>>> meant;  // what, its edits
        if (item.turns_off) meant.push_back({"off", module_switch_edits(key, false, on_by_default)});
        if (item.turns_on) meant.push_back({"on", module_switch_edits(key, true, on_by_default)});
        if (item.allows_ironman) meant.push_back({"allowed in ironman", {ListEdit{"allow_ironman", key, true}}});
        if (item.allows_multiplayer) meant.push_back({"allowed in multiplayer", {ListEdit{"allow_multiplayer", key, true}}});
        for (const auto& [state, list] : meant) {
            std::string changes;
            for (const ListEdit& e : list) {
                std::vector<std::string>* l = list_of(lists, e.key);
                if (l == nullptr || listed(*l, e.id) == e.listed) continue;
                if (e.listed) {
                    l->push_back(e.id);
                } else {
                    l->erase(std::remove(l->begin(), l->end(), e.id), l->end());
                }
                plan.edits.push_back(e);
                changes += (changes.empty() ? "" : " and ") + std::string(e.listed ? "added it to " : "took it out of ") + e.key;
            }
            if (!write_files) {
                plan.notes.push_back({ANVIL_LOG_INFO, changes.empty()
                                                          ? what + " means module " + key + " " + state + "; anvil.toml has it so already"
                                                          : what + " means module " + key + " " + state +
                                                                " at this benchmark start; anvil.toml is not written"});
                continue;
            }
            plan.notes.push_back({ANVIL_LOG_INFO,
                                  changes.empty() ? what + " meant module " + key + " " + state + "; anvil.toml has it so already"
                                                  : what + " meant module " + key + " " + state + ": " + changes +
                                                        " in anvil.toml, " + state + " from this start"});
        }
    }
    if (!write_files) {
        // A benchmark start: this start's lists only, nothing to write.
        plan.edits.clear();
        plan.notes.push_back({ANVIL_LOG_INFO, "a benchmark start writes neither anvil.toml nor settings.toml: the retired "
                                              "setting(s) stay for the next start"});
        return plan;
    }
    plan.writes_player_file = true;
    plan.player_text = move->text;
    for (const RetiredMove::Item& item : move->items) {
        plan.removed += (plan.removed.empty() ? "" : ", ") + item.name + " = " + item.value;
        if (moves_something(item)) plan.moved.push_back({item.name, item.value});
    }
    return plan;
}

std::vector<RetiredMoveNote> carry_out_retired_move(const RetiredMovePlan& plan, const std::filesystem::path& player_file,
                                                    const std::filesystem::path& anvil_toml, bool* anvil_toml_written,
                                                    std::vector<MovedRetired>* recorded, bool* player_file_written) {
    if (anvil_toml_written != nullptr) *anvil_toml_written = true;
    if (player_file_written != nullptr) *player_file_written = true;
    std::vector<RetiredMoveNote> notes;
    std::string error;
    if (!plan.edits.empty() && !set_listed(anvil_toml, plan.edits, error)) {
        if (anvil_toml_written != nullptr) *anvil_toml_written = false;
        if (player_file_written != nullptr) *player_file_written = false;
        notes.push_back({ANVIL_LOG_WARNING, "retired settings not moved: cannot write anvil.toml (" + error +
                                                "); their modules are switched as they say at this start only, "
                                                "and the values stay for the next start"});
        return notes;
    }
    notes.insert(notes.end(), plan.notes.begin(), plan.notes.end());
    if (plan.forget_recorded && recorded != nullptr) recorded->clear();
    if (!plan.writes_player_file) return notes;
    if (!replace_text_file(player_file, plan.player_text, error)) {
        if (player_file_written != nullptr) *player_file_written = false;
        if (recorded == nullptr || plan.moved.empty()) {
            notes.push_back({ANVIL_LOG_WARNING, "cannot remove the retired settings from settings.toml: " + error});
            return notes;
        }
        *recorded = plan.moved;
        notes.push_back({ANVIL_LOG_WARNING, "cannot remove the retired settings from settings.toml: " + error +
                                                "; recorded in data/anvil/retired-moved.toml, so a later start only "
                                                "tries again to remove them and keeps the switches anvil.toml has"});
        return notes;
    }
    if (recorded != nullptr) recorded->clear();
    notes.push_back({ANVIL_LOG_INFO, "removed the retired setting(s) " + plan.removed + " from settings.toml" +
                                         (plan.player_text.empty() ? " (nothing else was in it: the file is removed)" : "")});
    return notes;
}

std::vector<RetiredMoveNote> move_retired_settings(const Manifest& manifest, const SettingsFile& schema,
                                                   const std::filesystem::path& player_file,
                                                   const std::filesystem::path& anvil_toml,
                                                   AnvilSettings& lists, bool write_files, bool* anvil_toml_written,
                                                   const std::filesystem::path& record, bool* settled) {
    if (anvil_toml_written != nullptr) *anvil_toml_written = true;
    if (settled != nullptr) *settled = true;
    if (schema.retired.empty()) return {};
    const std::optional<std::string> text = read_text_file(player_file, kMaxPluginTomlBytes);
    const bool recording = !record.empty() && write_files;
    MovedRetiredRecord moved = recording ? read_moved_retired(record) : MovedRetiredRecord{};
    const MovedRetiredRecord before = moved;
    std::vector<MovedRetired>& entries = moved[manifest.id];
    AnvilSettings after = lists;
    const RetiredMovePlan plan = plan_retired_settings(manifest, schema, text, after, write_files, entries);
    bool written = true;
    bool player_written = true;
    std::vector<RetiredMoveNote> notes =
        carry_out_retired_move(plan, player_file, anvil_toml, &written, recording ? &entries : nullptr, &player_written);
    if (anvil_toml_written != nullptr) *anvil_toml_written = written;
    bool record_ok = true;
    if (recording && moved_retired_text(moved) != moved_retired_text(before)) {
        std::string error;
        record_ok = write_moved_retired(record, moved, error);
        if (!record_ok) {
            notes.push_back({ANVIL_LOG_WARNING, "cannot write data/anvil/retired-moved.toml: " + error +
                                                    "; a later start moves the values still in settings.toml again"});
        }
    }
    if (settled != nullptr) {
        // A later start moves nothing of this again: the player's file lost the values, or
        // the record holds those that would move (none would: nothing to keep).
        *settled = written && (player_written || plan.moved.empty() || (recording && record_ok));
    }
    // Also when anvil.toml cannot be written: the module is as the value says at this start
    // (ADR 0013 point 4), and the player's file keeps the value for the next start.
    lists = std::move(after);
    return notes;
}

}  // namespace anvil
