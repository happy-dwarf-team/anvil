#include "session.hpp"

namespace anvil_game {

SessionChanges SessionTracker::update(const SessionView& view) {
    SessionChanges out;
    if (!view.readable) return out;
    if (active_) {
        const bool same = view.running && view.session == session_ && (current_.ironman != 0) == view.ironman &&
                          (current_.multiplayer != 0) == view.multiplayer;
        if (same) return out;
        AnvilGameSession& ended = out.items[out.count++];
        ended = current_;
        ended.change = ANVIL_GAME_SESSION_ENDED;
        active_ = false;
    }
    if (view.running) {
        current_ = AnvilGameSession{};
        current_.size = sizeof(AnvilGameSession);
        current_.change = ANVIL_GAME_SESSION_STARTED;
        current_.ironman = view.ironman ? 1u : 0u;
        current_.multiplayer = view.multiplayer ? 1u : 0u;
        current_.number = ++started_;
        session_ = view.session;
        active_ = true;
        out.items[out.count++] = current_;
    }
    return out;
}

}  // namespace anvil_game
