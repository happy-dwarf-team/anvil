#pragma once

#include "control/anvil_control.h"
#include "crash_history.hpp"
#include "gameplay_policy.hpp"
#include "resolver.hpp"
#include "settings.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct AnvilPlugin;

namespace anvil {

class PluginSettings;

// What anvil.control reports on. The runtime fills it once the plugins are
// resolved; the tests build their own.
struct ControlSources {
    std::filesystem::path anvil_dir;  // anvil/: anvil.toml, anvil.log, crashes/, data/anvil/
    const char* anvil_version = "";
    const char* game_build = nullptr;
    // Resolved plugins in load order, and the skipped ones; neither changes
    // after start-up (the plugins' states do, until loading_done).
    const std::vector<std::unique_ptr<AnvilPlugin>>* plugins = nullptr;
    const std::vector<Skipped>* skipped = nullptr;
    const GameplayPolicy* policy = nullptr;  // may be null
    std::vector<std::string> disabled;       // disabled_plugins at this start
    bool safe_mode = false;
    unsigned unfinished_starts = 0;
    // The newest crash report written before this start ("": none). Read at
    // start-up, before this process could write one.
    std::string previous_crash;
};

// The core's private service for anvil-overlay (src/control/anvil_control.h,
// design §15). One per process; service() is what the core registers as
// "anvil.control" v1. Every call may come from any thread.
class Control {
public:
    explicit Control(ControlSources sources);
    ~Control();
    Control(const Control&) = delete;
    Control& operator=(const Control&) = delete;

    // The table of function pointers. They reach this object through a
    // process-wide pointer that the constructor sets (and the destructor
    // clears), so only one Control exists at a time.
    const AnvilControlService* service() const;

    // Every plugin was processed: the plugins' states are final from now on
    // and the service reports them.
    void loading_done();

    void status(AnvilControlStatus& out) const;
    bool plugin(uint32_t index, AnvilControlPlugin& out) const;
    std::optional<std::string> warning(uint32_t index) const;
    PluginSettings* settings_of(std::string_view plugin_id) const;
    // The previous crash, loaded on the first call; that call also marks it
    // as shown in anvil/data/anvil/crash-shown.txt.
    const AnvilControlCrash* previous_crash();
    std::string log_tail(uint32_t max_lines) const;
    AnvilResult disable_next_start(std::string_view plugin_id, std::string& error);
    bool disabled_next_start(std::string_view plugin_id) const;

private:
    struct CrashCache {
        CrashSummary summary;
        std::string plugins;
        std::string report;
        std::wstring path;
        AnvilControlCrash view{};
    };

    ControlSources sources_;
    std::vector<std::string> versions_;  // by plugin index
    std::atomic<bool> loading_done_{false};
    mutable std::mutex mutex_;
    std::set<std::string, std::less<>> disabled_next_;  // under mutex_
    bool crash_loaded_ = false;                         // under mutex_
    std::unique_ptr<CrashCache> crash_;                 // under mutex_, then fixed
};

// The value as text, as parse_setting_text reads it back: true/false, a
// decimal int, a number, an option's name, the string, a key name.
std::string setting_text(const Setting& setting, const SettingValue& value);

// The hash of design §13 over plugin ids and versions ("id version" lines in
// byte order), shown as "XXXX-XXXX-XXXX-XXXX" (the start of a SHA-256);
// "" for none.
std::string plugin_set_hash(std::vector<std::string> ids_and_versions);

}  // namespace anvil
