#pragma once

#include <filesystem>

namespace anvil {

// From version.txt via CMake (ANVIL_VERSION on anvil_core).
constexpr const char* kAnvilVersion = ANVIL_VERSION;

// Runs once on the game's main thread at the executable entry point, before
// the game's C runtime initializes. Never throws; on any problem Anvil logs it
// and leaves the game alone.
void startup(const std::filesystem::path& game_dir);

}  // namespace anvil
