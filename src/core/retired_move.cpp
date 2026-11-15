#include "retired_move.hpp"

#include "anvil_toml_edit.hpp"
#include "crash_history.hpp"

#include <algorithm>

namespace anvil {

namespace {

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

}  // namespace

RetiredMovePlan plan_retired_settings(const Manifest& manifest, const SettingsFile& schema,
                                      const std::optional<std::string>& player_text, AnvilSettings& lists,
                                      bool write_files) {
    RetiredMovePlan plan;
    if (schema.retired.empty() || !player_text) return plan;  // no player's file: nothing to move
    std::string_view text = *player_text;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);  // saved by Notepad
    std::string error;
    const std::optional<RetiredMove> move = plan_retired_move(schema, text, error);
    if (!move) {
        plan.notes.push_back({ANVIL_LOG_WARNING, "retired settings not moved from settings.toml: " + error});
        return plan;
    }
    if (move->items.empty()) return plan;
    // Every edit of every value, in file order, against this start's lists,
    // so a later value sees what an earlier one did.
    for (const RetiredMove::Item& item : move->items) {
        const std::string key = manifest.id + "." + item.module;
        const auto declared = std::find_if(manifest.modules.begin(), manifest.modules.end(),
                                           [&](const Module& m) { return m.id == item.module; });
        const bool on_by_default = declared == manifest.modules.end() || declared->enabled_by_default;
        std::vector<std::pair<std::string, std::vector<ListEdit>>> meant;  // what, its edits
        if (item.turns_off) meant.push_back({"off", module_switch_edits(key, false, on_by_default)});
        if (item.turns_on) meant.push_back({"on", module_switch_edits(key, true, on_by_default)});
        if (item.allows_ironman) meant.push_back({"allowed in ironman", {ListEdit{"allow_ironman", key, true}}});
        if (item.allows_multiplayer) meant.push_back({"allowed in multiplayer", {ListEdit{"allow_multiplayer", key, true}}});
        const std::string what = "retired setting " + item.name + " = " + item.value + " (settings.toml line " +
                                 std::to_string(item.line) + ")";
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
    }
    return plan;
}

std::vector<RetiredMoveNote> carry_out_retired_move(const RetiredMovePlan& plan, const std::filesystem::path& player_file,
                                                    const std::filesystem::path& anvil_toml, bool* anvil_toml_written) {
    if (anvil_toml_written != nullptr) *anvil_toml_written = true;
    std::vector<RetiredMoveNote> notes;
    std::string error;
    if (!plan.edits.empty() && !set_listed(anvil_toml, plan.edits, error)) {
        if (anvil_toml_written != nullptr) *anvil_toml_written = false;
        notes.push_back({ANVIL_LOG_WARNING, "retired settings not moved: cannot write anvil.toml (" + error +
                                                "); their modules are switched as they say at this start only, "
                                                "and the values stay for the next start"});
        return notes;
    }
    notes.insert(notes.end(), plan.notes.begin(), plan.notes.end());
    if (!plan.writes_player_file) return notes;
    if (!replace_text_file(player_file, plan.player_text, error)) {
        notes.push_back({ANVIL_LOG_WARNING, "cannot remove the retired settings from settings.toml: " + error});
        return notes;
    }
    notes.push_back({ANVIL_LOG_INFO, "removed the retired setting(s) " + plan.removed + " from settings.toml" +
                                         (plan.player_text.empty() ? " (nothing else was in it: the file is removed)" : "")});
    return notes;
}

std::vector<RetiredMoveNote> move_retired_settings(const Manifest& manifest, const SettingsFile& schema,
                                                   const std::filesystem::path& player_file,
                                                   const std::filesystem::path& anvil_toml,
                                                   AnvilSettings& lists, bool write_files) {
    if (schema.retired.empty()) return {};
    const std::optional<std::string> text = read_text_file(player_file, kMaxPluginTomlBytes);
    AnvilSettings after = lists;
    const RetiredMovePlan plan = plan_retired_settings(manifest, schema, text, after, write_files);
    std::vector<RetiredMoveNote> notes = carry_out_retired_move(plan, player_file, anvil_toml);
    // Also when anvil.toml cannot be written: the module is as the value says at this start
    // (ADR 0013 point 4), and the player's file keeps the value for the next start.
    lists = std::move(after);
    return notes;
}

}  // namespace anvil
