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

std::vector<RetiredMoveNote> move_retired_settings(const Manifest& manifest, const SettingsFile& schema,
                                                   const std::filesystem::path& player_file,
                                                   const std::filesystem::path& anvil_toml,
                                                   AnvilSettings& lists) {
    std::vector<RetiredMoveNote> notes;
    if (schema.retired.empty()) return notes;
    std::optional<std::string> text = read_text_file(player_file, kMaxPluginTomlBytes);
    if (!text) return notes;  // no player's file: nothing to move
    if (text->starts_with("\xEF\xBB\xBF")) text->erase(0, 3);  // saved by Notepad
    std::string error;
    const std::optional<RetiredMove> move = plan_retired_move(schema, *text, error);
    if (!move) {
        notes.push_back({ANVIL_LOG_WARNING, "retired settings not moved from settings.toml: " + error});
        return notes;
    }
    if (move->items.empty()) return notes;
    // Every edit of every value, in file order, against a copy of this
    // start's lists, so a later value sees what an earlier one did.
    AnvilSettings after = lists;
    std::vector<ListEdit> edits;
    std::vector<RetiredMoveNote> pending;
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
                std::vector<std::string>* l = list_of(after, e.key);
                if (l == nullptr || listed(*l, e.id) == e.listed) continue;
                if (e.listed) {
                    l->push_back(e.id);
                } else {
                    l->erase(std::remove(l->begin(), l->end(), e.id), l->end());
                }
                edits.push_back(e);
                changes += (changes.empty() ? "" : " and ") + std::string(e.listed ? "added it to " : "took it out of ") + e.key;
            }
            pending.push_back({ANVIL_LOG_INFO,
                               changes.empty() ? what + " meant module " + key + " " + state + "; anvil.toml has it so already"
                                               : what + " meant module " + key + " " + state + ": " + changes +
                                                     " in anvil.toml, " + state + " from this start"});
        }
    }
    if (!edits.empty() && !set_listed(anvil_toml, edits, error)) {
        notes.push_back({ANVIL_LOG_WARNING, "retired settings not moved: cannot write anvil.toml (" + error +
                                                "); the values stay for the next start"});
        return notes;
    }
    lists = std::move(after);
    notes.insert(notes.end(), pending.begin(), pending.end());
    if (!replace_text_file(player_file, move->text, error)) {
        notes.push_back({ANVIL_LOG_WARNING, "cannot remove the retired settings from settings.toml: " + error});
        return notes;
    }
    std::string names;
    for (const RetiredMove::Item& item : move->items) {
        names += (names.empty() ? "" : ", ") + item.name + " = " + item.value;
    }
    notes.push_back({ANVIL_LOG_INFO, "removed the retired setting(s) " + names + " from settings.toml" +
                                         (move->text.empty() ? " (nothing else was in it: the file is removed)" : "")});
    return notes;
}

}  // namespace anvil
