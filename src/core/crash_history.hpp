#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil {

// Crash reports already written (anvil/crashes/<name>.json and .txt, design
// §12), read back at a later start: by the core for safe mode and the
// overlay, and outside the game by the Manager.

// The report names (file names without extension), newest first: they sort
// by time.
std::vector<std::string> crash_report_names(const std::filesystem::path& crash_dir);

// What a report says about the crash, from its JSON (format 1). A partial
// report (a fault while writing it) gives what it has.
struct CrashSummary {
    std::string name;
    std::string time;
    std::string level;    // in_plugin_code | in_plugin_change | plugin_on_stack | no_plugin; "" if missing
    std::string summary;  // "in plugin x's code"
    std::vector<std::string> plugins;  // involved, most involved first
    std::string issues;                // the first issues link of an involved plugin, or ""
    bool before_game_ready = false;
};

CrashSummary parse_crash_summary(std::string_view json);
// nullopt when the file cannot be read.
std::optional<CrashSummary> load_crash_summary(const std::filesystem::path& crash_dir, const std::string& name);

// The crash was in the plugin's own code or in one of its changes (the two
// levels that put the crash address in the plugin, design §12). "On the
// stack" does not count.
bool crash_in_plugin(const CrashSummary& crash, std::string_view plugin_id);

// The whole file as text; nullopt when it cannot be read or is larger than
// `max_bytes`.
std::optional<std::string> read_text_file(const std::filesystem::path& file, size_t max_bytes);

// The last lines of a text file such as anvil.log: at most `max_lines` and
// `max_bytes` (a cut first line is dropped), without a UTF-8 BOM. Empty when
// the file cannot be read. Opens the file shared, so a writer may hold it.
std::string read_text_tail(const std::filesystem::path& file, size_t max_lines, size_t max_bytes);

}  // namespace anvil
