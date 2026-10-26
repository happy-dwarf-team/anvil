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

// Runs once when the process exits: when the game calls ExitProcess (the
// core replaces that import of eu4.exe), or else from DllMain
// (DLL_PROCESS_DETACH, before the DLL's own static objects go). With
// profile = true it writes the call counters to anvil.log; an exit before
// game_ready without a crash is not counted for safe mode. It never waits for
// a lock (at DLL_PROCESS_DETACH other threads are gone and may have died
// holding one); it writes nothing instead.
void shutdown();

// The settings of a plugin that resolved (loaded, failed or not loaded yet),
// for in-game editors (design §11): PluginSettings::set saves a value and
// applies it. nullptr for any other id, or before start-up has resolved the
// plugins. The overlay reaches it through anvil.control (control.hpp).
PluginSettings* plugin_settings(std::string_view plugin_id);

}  // namespace anvil
