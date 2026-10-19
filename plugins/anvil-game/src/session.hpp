// Game sessions as anvil-game sees them, one view per frame of the main loop.
#pragma once

#include <anvil/anvil_game.h>

#include <cstdint>

namespace anvil_game {

// What one frame read from the game.
struct SessionView {
    bool readable = true;  // false: reading faulted; the view says nothing
    bool running = false;  // a game session runs: game session class, speed object, date set
    std::uintptr_t session = 0;
    bool ironman = false;
    bool multiplayer = false;
};

// The session events one view causes, in order: at most an ENDED and a STARTED.
struct SessionChanges {
    AnvilGameSession items[2]{};
    int count = 0;
};

// Turns views into session events. A session ends when the view stops
// running, shows another session object, or changes ironman or multiplayer;
// the next running view starts a new one. A view that is not readable changes
// nothing. Main thread only.
class SessionTracker {
public:
    SessionChanges update(const SessionView& view);
    bool active() const { return active_; }
    // The running session (change = STARTED); meaningful while active().
    const AnvilGameSession& current() const { return current_; }

private:
    bool active_ = false;
    std::uintptr_t session_ = 0;
    AnvilGameSession current_{};
    std::uint64_t started_ = 0;
};

}  // namespace anvil_game
