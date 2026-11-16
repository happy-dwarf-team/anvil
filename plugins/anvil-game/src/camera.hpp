// The map camera's pan as the arrow keys make it (include/anvil/anvil_game.h,
// camera_pan). CEU3Camera::HandleMouseAndKBInput turns the arrows into a
// vector of length 1 (x to the right, y up) times a speed, and
// CEU3Camera::UpdateMouseAndKB pans by it with the frame's time, capped.
#pragma once

#include <cmath>

namespace anvil_game {

// The game's constants of the arrows' speed (symbols.toml camera_key_speed and
// camera_frame_time_cap).
struct CameraSpeed {
    float divisor = 0;  // height / divisor, clamped to [lower, upper]
    float lower = 0;
    float upper = 0;
    float factor = 0;    // times the player's scroll speed
    float time_cap = 0;  // the frame time a pan takes at most
};

struct CameraStep {
    float pan[2] = {0, 0};  // the vector CEU3Camera::Pan takes
    float frame_time = 0;
};

// False for a value that is not finite. `right` and `up` longer than 1 are
// shortened to length 1; (0, 0) gives a zero pan.
inline bool camera_step(const CameraSpeed& k, float right, float up, float height, float scroll_speed,
                        float frame_time, CameraStep& out) {
    if (!std::isfinite(right) || !std::isfinite(up)) return false;
    const float length = std::hypot(right, up);
    if (length > 1.0f) {
        right /= length;
        up /= length;
    }
    // As the game: `lower > c ? lower : min(upper, c)`, with c = height / divisor.
    const float c = height / k.divisor;
    const float zoom = k.lower > c ? k.lower : std::fmin(k.upper, c);
    const float speed = scroll_speed * k.factor * zoom;
    out.pan[0] = right * speed;
    out.pan[1] = up * speed;
    out.frame_time = std::fmin(frame_time, k.time_cap);
    return std::isfinite(out.pan[0]) && std::isfinite(out.pan[1]) && std::isfinite(out.frame_time);
}

}  // namespace anvil_game
