#pragma once

#include <filesystem>
#include <string_view>

namespace anvil {

class PluginSettings;

// From version.txt via CMake (ANVIL_VERSION on anvil_core).
constexpr const char* kAnvilVersion = ANVIL_VERSION;

// Runs once on the game's main thread at the executable entry point, before
// the game's C runtime initializes. Never throws; on any problem Anvil logs it
// and leaves the game alone.
void startup(const std::filesystem::path& game_dir);

// Runs from DllMain when the process exits (DLL_PROCESS_DETACH, before the
// DLL's own static objects go): with profile = true it writes the call
// counters to anvil.log. Other threads are gone by then and may have died
// holding a lock, so it never waits for one; it writes nothing instead.
void shutdown();

// The settings of a plugin that resolved (loaded, failed or not loaded yet),
// for in-game editors (design §11): PluginSettings::set saves a value and
// applies it. nullptr for any other id, or before start-up has resolved the
// plugins. The overlay (#58) reaches it through an Anvil service.
PluginSettings* plugin_settings(std::string_view plugin_id);

}  // namespace anvil
