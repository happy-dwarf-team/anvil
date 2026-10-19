#pragma once

#include "changes.hpp"
#include "manifest.hpp"
#include "services.hpp"

#include <anvil/anvil_plugin.h>

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace anvil {

// The modes of a game session the policy looks at (anvil-game's session event).
struct SessionMode {
    bool ironman = false;
    bool multiplayer = false;
};

// The player's opt-ins from anvil.toml (allow_ironman, allow_multiplayer).
struct GameplayOptIns {
    std::vector<std::string> ironman;
    std::vector<std::string> multiplayer;
};

// Why a plugin must be suspended in a session of `mode`; empty when it stays
// active. Only plugins with affects_gameplay = true are ever suspended; one of
// them stays active in a mode only when its plugin.toml offers "opt-in" for it
// and the player listed it in anvil.toml. A session that is both ironman and
// multiplayer needs both.
std::string suspension_reason(const Manifest& manifest, SessionMode mode, const GameplayOptIns& opt_ins);

// Problems with the player's opt-ins for this plugin (e.g. listed for a mode
// its plugin.toml does not offer), one line each; nothing for a plugin that
// is not listed.
std::vector<std::string> opt_in_problems(const Manifest& manifest, const GameplayOptIns& opt_ins);

// The gameplay policy of design §13: while a session that is ironman or
// multiplayer runs, every plugin with affects_gameplay = true that is not
// allowed in it is suspended (its changes taken out of the game, then
// anvil.suspended sent to it); when the session ends they are resumed
// (changes back, then anvil.resumed). anvil-game reports the sessions through
// ANVIL_GAME_EVENT_SESSION, which only anvil-game can publish.
class GameplayPolicy {
public:
    using LogSink = std::function<void(AnvilLogLevel level, const std::string& id, const std::string& message)>;

    GameplayPolicy(ChangeRegistry& changes, ServiceBus& bus, GameplayOptIns opt_ins, LogSink log);

    // A plugin, in load order, before it loads; logs its opt-in problems. If
    // a session that calls for it runs already, the plugin's owner is
    // suspended now, so the changes it requests are enabled suspended.
    void add(const Manifest& manifest, OwnerId owner);
    // A plugin whose load failed: it has no changes and nothing to tell.
    void forget(OwnerId owner);

    void session_started(SessionMode mode);
    void session_ended();
    // ANVIL_GAME_EVENT_SESSION from anvil-game (an AnvilGameSession payload).
    void on_session_event(const void* payload);
    // Subscribes on_session_event to the bus, as the core ("anvil").
    void subscribe();

    std::optional<SessionMode> session() const;
    bool suspended(OwnerId owner) const;  // by this policy

private:
    struct Entry {
        Manifest manifest;
        OwnerId owner = 0;
        bool suspended = false;
        bool forgotten = false;
    };
    void apply(Entry& entry, const std::string& reason);  // mutex_ held
    void release(Entry& entry, const char* why);          // mutex_ held

    ChangeRegistry& changes_;
    ServiceBus& bus_;
    GameplayOptIns opt_ins_;
    LogSink log_;
    mutable std::mutex mutex_;
    std::optional<SessionMode> session_;
    std::vector<Entry> entries_;
};

}  // namespace anvil
