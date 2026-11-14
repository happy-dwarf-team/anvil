#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil {

// anvil/data/anvil/last-start.toml (ADR 0013, point 8): what the last start
// of the game used. The core writes it at each start that loads plugins, after
// the retired settings moved and the plugins' settings were read; the Manager
// compares the files with it to mark "at the next start" and to undo, also
// while the game is closed. A missing or unreadable record marks nothing.
struct LastStart {
    struct Approval {
        std::string id;
        std::string mod;  // the mod's descriptor ("mod/ugc_123.mod")
        std::string version;
        std::string hash;

        bool operator==(const Approval&) const = default;
    };

    std::string anvil_version;
    std::string written;  // UTC, "2026-11-14T18:05:11Z"
    bool safe_mode = false;
    // The lists of anvil.toml as this start read them (after the move of
    // retired settings into disabled_modules).
    std::vector<std::string> disabled_plugins;
    std::vector<std::string> allow_ironman;
    std::vector<std::string> allow_multiplayer;
    std::vector<std::string> enabled_modules;
    std::vector<std::string> disabled_modules;
    std::vector<Approval> approvals;  // trust.toml's, in its order
    // By plugin id, then setting name: the value this start read (the
    // player's file or the default; not an ANVIL_<ID>_<NAME> override), as a
    // TOML value (setting_toml: true, 1024, 0.5, "on", "Ctrl+F12"). Every
    // declared setting of each plugin that resolved; none of a skipped one.
    std::map<std::string, std::map<std::string, std::string>> settings;

    bool operator==(const LastStart&) const = default;
};

inline constexpr uint32_t kLastStartFormat = 1;

// The file for `record`. Plugin ids and setting names are bare TOML keys.
std::string last_start_text(const LastStart& record);
// nullopt with `error` when the text does not parse or is not format 1.
// Unknown keys are ignored, so a newer Anvil may add some.
std::optional<LastStart> parse_last_start(std::string_view text, std::string& error);
// nullopt when the file is missing or unreadable.
std::optional<LastStart> read_last_start(const std::filesystem::path& file);
// Through a temporary file, creating the folder. False with `error` on failure.
bool write_last_start(const std::filesystem::path& file, const LastStart& record, std::string& error);

}  // namespace anvil
