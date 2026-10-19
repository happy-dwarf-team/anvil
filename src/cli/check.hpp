#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::cli {

// Exit codes (design §16).
inline constexpr int kExitOk = 0;
inline constexpr int kExitProblems = 1;
inline constexpr int kExitCannotCheck = 2;

enum class Format { Text, Json, Github };

std::optional<Format> parse_format(std::string_view text);

// A problem in one of the plugin's files. `file` is the path as the author
// passed it (the plugin folder joined with the file name), `line` is 1-based
// or 0 for the whole file.
struct Problem {
    std::string file;
    uint32_t line = 0;
    std::string message;
};

struct CheckOptions {
    std::filesystem::path plugin_dir;
    std::filesystem::path exe;  // empty: no --exe, check symbols.lock instead
    // The build id of an eu4.exe by its SHA-256 (upper-case hex), or nullptr
    // when the build is unknown. Empty: the table compiled into Anvil.
    std::function<const char*(std::string_view sha256)> build_of;
};

struct CheckReport {
    int exit_code = kExitOk;
    std::string cannot_check;  // why, with kExitCannotCheck
    std::string plugin;        // the plugin id, once plugin.toml parsed
    bool has_symbols = false;
    std::string build;         // symbols.toml `build`
    size_t symbols = 0;
    bool with_exe = false;
    std::string exe_name;      // the --exe file name
    std::string exe_sha256;
    std::string exe_build;     // empty: not a known build
    std::string lock;          // symbols.lock: "written", "unchanged", "matches" or empty
    std::vector<Problem> problems;
};

// `anvil check <plugin-dir> [--exe <eu4.exe>]` (design §16): the strict
// plugin.toml and symbols.toml schemas (the loader's parsers); with an exe the
// build, every symbol (verify_symbols) and instruction boundaries, then
// writes symbols.lock; without one, that symbols.lock matches symbols.toml.
CheckReport check_plugin(const CheckOptions& options);

std::string format_report(const CheckReport& report, Format format);

// symbols.lock (design §16), next to plugin.toml.
struct SymbolsLock {
    std::string tool;            // "anvil <version>" that wrote it
    std::string symbols_sha256;  // symbols_digest() of symbols.toml
    std::string exe_sha256;      // of the eu4.exe it was checked against
    std::string build;           // that eu4.exe's build id, or "unknown"
    bool ok = false;             // result = "ok" | "problems"
};

inline constexpr const char* kLockFile = "symbols.lock";

// SHA-256 of symbols.toml with a UTF-8 BOM removed and CRLF read as LF, so a
// checkout with other line endings (git autocrlf) gives the same digest.
std::string symbols_digest(std::string_view text);
std::string lock_text(const SymbolsLock& lock);

struct LockResult {
    std::optional<SymbolsLock> lock;
    std::string error;
    uint32_t line = 0;
    uint32_t symbols_line = 0;  // of symbols_sha256
    uint32_t result_line = 0;   // of result
};
LockResult parse_lock(std::string_view text);

}  // namespace anvil::cli
