// The window's rows and the log's lines for a reload run, within the limits
// of anvil-overlay.panel v1 (1024 rows, each text at most 4096 bytes).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "files.hpp"
#include "reload.hpp"
#include "texts.hpp"

namespace model_reload {

inline constexpr std::uint32_t kText = 1;       // ANVIL_PANEL_TEXT
inline constexpr std::uint32_t kButton = 2;     // ANVIL_PANEL_BUTTON
inline constexpr std::uint32_t kSeparator = 3;  // ANVIL_PANEL_SEPARATOR
inline constexpr std::uint64_t kReload = 1;     // the button's id
inline constexpr std::size_t kFilesShown = 40;  // changed files listed; the rest is "N more"
inline constexpr std::size_t kScanLimit = 200000;

struct Row {
    std::uint32_t kind = kText;
    std::string label;
    std::string value;
    bool has_value = false;
    std::uint64_t id = 0;
};

enum class Mode { menu, game, ironman, multiplayer };

struct PanelState {
    Mode mode = Mode::menu;
    bool looked = false;  // the first look at the files is done
    std::vector<std::string> mods;
    std::size_t mod_problems = 0;
    bool cut = false;
    std::vector<Change> changed;
    bool running = false;
    bool has_last = false;
    RunResult last;
    std::string last_time;  // "12:04:31"
};

std::vector<Row> rows(const PanelState& state, const Texts& t);

// What a changed file runs: a console command or the plugin's own reload, as
// the window and the log write it (not translated).
const char* action_of(Kind kind);

// One line per step, in English, for anvil.log and the lab's control file.
std::vector<std::string> log_lines(const RunResult& result);

}  // namespace model_reload
