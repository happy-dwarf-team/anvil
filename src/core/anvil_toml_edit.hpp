#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil {

// The player's anvil.toml with `id` added to (`listed`) or removed from one
// of its lists of plugin ids: disabled_plugins (design §12, §15: "disable at
// the next start"; the Manager enables plugins again), allow_ironman or
// allow_multiplayer (§13, plugin ids or "<plugin>.<module>"), or
// enabled_modules / disabled_modules ("<plugin>.<module>" only, ADR 0011).
// Everything else stays as it was, comments and
// layout included: the id is inserted into the existing list, or a
// `<key> = ["<id>"]` line is added before the first table (or at the end); a
// removed value takes its comma with it, and its line when it stood alone. A
// text that already says so is returned unchanged. nullopt with `error` when
// `key` is not one of those lists or `id` not a plugin id, when the text does
// not parse or the key is not a list, or when the edited text would not read
// back as the same file with the id added or removed.
std::optional<std::string> with_plugin_listed(std::string_view text, std::string_view key, std::string_view id,
                                              bool listed, std::string& error);
// with_plugin_listed(text, "disabled_plugins", id, true, error).
std::optional<std::string> with_disabled_plugin(std::string_view text, std::string_view id, std::string& error);

// The same on the file: reads it (a missing file is an empty one), writes it
// through a temporary file in the same folder and replaces the old one in one
// step. False with `error` on failure; the file is then untouched. A missing
// file stays missing when there is nothing to remove.
bool set_plugin_listed(const std::filesystem::path& file, std::string_view key, std::string_view id, bool listed,
                       std::string& error);
bool add_disabled_plugin(const std::filesystem::path& file, std::string_view id, std::string& error);

// One change of a list: `id` into (`listed`) or out of `key`.
struct ListEdit {
    std::string key;
    std::string id;
    bool listed = false;
};
// Several changes of the file in one write, in order; false with `error`, and
// the file untouched, when any of them cannot be made.
bool set_listed(const std::filesystem::path& file, const std::vector<ListEdit>& edits, std::string& error);

// A module's switch for the next start (ADR 0011, 0013), the same text in
// the Manager and in the overlay: on is out of disabled_modules, and into
// enabled_modules when the module is off by default; off is into
// disabled_modules when it is on by default, else out of enabled_modules.
std::vector<ListEdit> module_switch_edits(std::string_view key, bool on, bool on_by_default);
bool set_module_switch(const std::filesystem::path& file, std::string_view key, bool on, bool on_by_default,
                       std::string& error);

}  // namespace anvil
