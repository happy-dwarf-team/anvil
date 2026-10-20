// The C++ side of anvil.control (src/control/anvil_control.h): copies of what
// the core reports, in std::string and std::vector.
#pragma once

#include "anvil_control.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace overlay {

struct PluginRow {
    std::string id, name, version, state, reason, issues;
    bool early = false;
    bool gameplay = false;
    bool suspended = false;
    bool disabled = false;       // by the player at this start
    bool disabled_next = false;  // listed in anvil.toml now
    uint32_t settings = 0;
};

struct CrashInfo {
    std::string name, level, summary, issues, report;
    std::vector<std::string> plugins;
    std::wstring path;
    bool before_game_ready = false;
    bool is_new = false;
};

class ControlClient {
public:
    explicit ControlClient(const AnvilControlService* service = nullptr) : service_(service) {}
    const AnvilControlService* service() const { return service_; }
    explicit operator bool() const { return service_ != nullptr; }

    AnvilControlStatus status() const;
    std::vector<PluginRow> plugins() const;
    std::vector<std::string> warnings() const;
    std::optional<CrashInfo> previous_crash() const;
    std::string log_tail(uint32_t max_lines) const;

    uint32_t setting_count(const std::string& plugin) const;
    std::optional<AnvilControlSetting> setting_info(const std::string& plugin, AnvilSetting setting) const;
    // (name, label) of each option of an enum setting.
    std::vector<std::pair<std::string, std::string>> setting_options(const std::string& plugin,
                                                                     AnvilSetting setting) const;
    std::string setting_value(const std::string& plugin, AnvilSetting setting, uint32_t which) const;
    std::vector<std::string> setting_warnings(const std::string& plugin) const;

    // "" on success, otherwise why.
    std::string set(const std::string& plugin, AnvilSetting setting, const AnvilControlValue& value) const;
    std::string set_text(const std::string& plugin, AnvilSetting setting, const std::string& text) const;
    std::string reset(const std::string& plugin, AnvilSetting setting) const;
    std::string disable_next_start(const std::string& plugin) const;

private:
    const AnvilControlService* service_;
};

// "a, b" -> {"a", "b"}
std::vector<std::string> split_list(const std::string& text);

}  // namespace overlay
