#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace anvil {

// "Disable at the next start" (design §12, §15): the player's anvil.toml with
// `id` added to disabled_plugins. Everything else stays as it was, comments
// and layout included: the id is inserted into the existing list, or a
// `disabled_plugins = ["<id>"]` line is added before the first table (or at
// the end). An id already listed gives the text unchanged. nullopt with
// `error` when the id is not a plugin id, when the text does not parse or
// disabled_plugins is not a list, or when the edited text would not read back
// as the same file plus the id.
std::optional<std::string> with_disabled_plugin(std::string_view text, std::string_view id, std::string& error);

// The same on the file: reads it (a missing file is an empty one), writes it
// through a temporary file in the same folder and replaces the old one in one
// step. False with `error` on failure; the file is then untouched.
bool add_disabled_plugin(const std::filesystem::path& file, std::string_view id, std::string& error);

}  // namespace anvil
