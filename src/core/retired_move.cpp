#include "retired_move.hpp"

#include "anvil_toml_edit.hpp"
#include "crash_history.hpp"

#include <algorithm>

namespace anvil {

std::vector<RetiredMoveNote> move_retired_settings(const Manifest& manifest, const SettingsFile& schema,
                                                   const std::filesystem::path& player_file,
                                                   const std::filesystem::path& anvil_toml,
                                                   std::vector<std::string>& disabled_modules) {
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
    for (const RetiredMove::Item& item : move->items) {
        if (!item.turns_off) continue;
        const std::string key = manifest.id + "." + item.module;
        const std::string what = "retired setting " + item.name + " = " + item.value + " (settings.toml line " +
                                 std::to_string(item.line) + ")";
        if (std::find(disabled_modules.begin(), disabled_modules.end(), key) != disabled_modules.end()) {
            notes.push_back({ANVIL_LOG_INFO, what + " meant module " + key + " off; it is in disabled_modules already"});
            continue;
        }
        if (!set_plugin_listed(anvil_toml, "disabled_modules", key, true, error)) {
            notes.push_back({ANVIL_LOG_WARNING, what + ": cannot add " + key + " to disabled_modules in anvil.toml (" +
                                                    error + "); the value stays for the next start"});
            return notes;
        }
        disabled_modules.push_back(key);
        notes.push_back({ANVIL_LOG_INFO, what + " meant module " + key +
                                             " off: added it to disabled_modules in anvil.toml, off from this start"});
    }
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
