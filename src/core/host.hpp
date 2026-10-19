#pragma once

#include "changes.hpp"
#include "manifest.hpp"
#include "plugin_symbols.hpp"
#include "seh_guard.hpp"
#include "services.hpp"

#include <anvil/anvil_plugin.h>
#include <windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <string_view>

namespace anvil {
struct HostContext;
}

// The handle a plugin gets as `self`: one per resolved plugin, alive for the
// process.
struct AnvilPlugin {
    anvil::HostContext* context = nullptr;
    anvil::Manifest manifest;
    anvil::OwnerId owner = 0;      // in the change registry; ids follow the load order
    anvil::PluginSymbols symbols;  // verified symbols.toml, by AnvilSymbol
    std::wstring plugin_dir;
    std::wstring data_dir;
    HMODULE module = nullptr;
    std::string state = "pending";  // pending | loaded | failed
    // Inside anvil_plugin_load the load itself is the plugin's group.
    std::atomic<bool> loading{false};
};

namespace anvil {

// What the host table acts on. The game has one for the process
// (runtime.cpp); the tests build their own around a synthetic image.
struct HostContext {
    ChangeRegistry* changes = nullptr;
    ServiceBus* bus = nullptr;
    HMODULE image = nullptr;  // whose imports replace_import changes (eu4.exe)
    // Plugin log lines; empty: the process log (Log::instance()).
    std::function<void(const AnvilPlugin& plugin, AnvilLogLevel level, std::string_view message)> log;
};

// The API 2 host table: every field filled, `game` as given. The functions
// reach their context through `self`, so one table serves every plugin.
AnvilHost make_host(const AnvilGameInfo* game);

AnvilResult to_result(ChangeStatus status);
const char* result_name(AnvilResult result);

struct LoadOutcome {
    AnvilResult result = ANVIL_OK;
    unsigned long exception = 0;  // the structured exception that ended anvil_plugin_load, 0 if none
    size_t staged = 0;            // changes requested during the load
    bool commit_failed = false;   // ANVIL_OK, but the staged changes could not be enabled
};

// Calls anvil_plugin_load as its own group (design §8.4): the changes it
// requests are staged and enabled together when it returns ANVIL_OK;
// otherwise none are, and the plugin's services and subscriptions are dropped.
LoadOutcome run_plugin_load(AnvilPlugin& plugin, const AnvilHost& host, PluginLoadFn fn);

}  // namespace anvil
