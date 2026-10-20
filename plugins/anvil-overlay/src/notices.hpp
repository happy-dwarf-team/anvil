// What the overlay tells the player without being asked (design §12, §13, §15):
// pure functions from what anvil.control reports to notices, so the rules are
// tested without a game or a device.
#pragma once

#include "control_client.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace overlay {

enum class NoticeKind { Info, Warning, Error };

struct NoticeAction {
    enum class Type { OpenPanel, DisablePlugin };
    Type type = Type::OpenPanel;
    std::string plugin;  // DisablePlugin
    std::string label;
};

struct Notice {
    std::string id;  // stable: "safe-mode", "crash", "not-loaded", "ironman", "multiplayer"
    NoticeKind kind = NoticeKind::Info;
    std::string title;
    std::vector<std::string> lines;
    std::vector<NoticeAction> actions;
    // Session notices stay while their session lasts and cannot be closed;
    // the others close after the notification time or on a click.
    bool lasting = false;
};

// Once per start, when the game is ready and every plugin was processed.
struct StartFacts {
    bool safe_mode = false;
    uint32_t unfinished_starts = 0;
    std::vector<PluginRow> plugins;
    std::optional<CrashInfo> crash;  // the previous crash, if any
};
std::vector<Notice> start_notices(const StartFacts& facts);

// While a game session runs (anvil-game.session), design §13.
struct SessionFacts {
    bool session = false;
    bool ironman = false;
    bool multiplayer = false;
    uint32_t gameplay_active = 0;
    std::string gameplay_hash;
};
std::vector<Notice> session_notices(const SessionFacts& facts);

// The panel key: a key code of settings.toml (virtual key in bits 0-7,
// 0x100 Shift, 0x200 Ctrl, 0x400 Alt) against a key press with the
// modifiers held. Exact: Ctrl+F12 does not match Ctrl+Shift+F12.
bool key_matches(int64_t code, unsigned vk, bool shift, bool ctrl, bool alt);
// A key press as a key code; 0 for a modifier key on its own.
int64_t key_code(unsigned vk, bool shift, bool ctrl, bool alt);

}  // namespace overlay
