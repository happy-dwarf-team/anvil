#include "notices.hpp"

#include <algorithm>

namespace overlay {
namespace {

constexpr size_t kMaxListed = 6;

bool known_plugin(const StartFacts& facts, const std::string& id) {
    return std::any_of(facts.plugins.begin(), facts.plugins.end(), [&](const PluginRow& p) { return p.id == id; });
}

bool disabled_next(const StartFacts& facts, const std::string& id) {
    return std::any_of(facts.plugins.begin(), facts.plugins.end(),
                       [&](const PluginRow& p) { return p.id == id && p.disabled_next; });
}

std::string crash_line(const CrashInfo& crash) {
    if (crash.level == "no_plugin") return "No plugin was involved.";
    if (!crash.summary.empty()) return "The crash was " + crash.summary + ".";
    return "The report names no plugin.";
}

}  // namespace

std::vector<Notice> start_notices(const StartFacts& facts) {
    std::vector<Notice> notices;
    const bool new_crash = facts.crash && facts.crash->is_new;

    if (facts.safe_mode) {
        Notice n;
        n.id = "safe-mode";
        n.kind = NoticeKind::Error;
        n.title = "Anvil safe mode";
        n.lines.push_back("The last " + std::to_string(facts.unfinished_starts) +
                          " starts ended before the game was ready, so this start loads only Anvil's standard "
                          "plugins. The next start loads every plugin again.");
        if (facts.crash) n.lines.push_back(crash_line(*facts.crash));
        n.actions.push_back({NoticeAction::Type::OpenPanel, {}, "Open the panel"});
        notices.push_back(std::move(n));
    }

    if (new_crash) {
        const CrashInfo& crash = *facts.crash;
        Notice n;
        n.id = "crash";
        n.kind = NoticeKind::Warning;
        n.title = crash.before_game_ready ? "The previous start crashed before the game was ready"
                                          : "The game crashed last time";
        n.lines.push_back(crash_line(crash));
        n.lines.push_back("Report: anvil\\crashes\\" + crash.name + ".txt");
        // Only the levels that put the crash in a plugin offer to disable it;
        // "on the stack" is too weak to suggest it.
        if (crash.level == "in_plugin_code" || crash.level == "in_plugin_change") {
            for (const std::string& id : crash.plugins) {
                if (!known_plugin(facts, id) || disabled_next(facts, id)) continue;
                n.actions.push_back({NoticeAction::Type::DisablePlugin, id, "Disable " + id + " at the next start"});
            }
        }
        n.actions.push_back({NoticeAction::Type::OpenPanel, {}, "Crash report"});
        notices.push_back(std::move(n));
    }

    std::vector<std::string> lines;
    for (const PluginRow& p : facts.plugins) {
        if (p.state == "failed") {
            lines.push_back(p.id + ": failed: " + p.reason);
        } else if (p.state == "skipped" && !p.disabled && !p.awaiting_approval &&
                   !(facts.safe_mode && p.reason == "safe mode")) {
            lines.push_back(p.id + ": " + p.reason);
        }
    }
    if (!lines.empty()) {
        Notice n;
        n.id = "not-loaded";
        n.kind = NoticeKind::Warning;
        n.title = std::to_string(lines.size()) + (lines.size() == 1 ? " plugin was not loaded" : " plugins were not loaded");
        const size_t shown = std::min(lines.size(), kMaxListed);
        n.lines.assign(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(shown));
        if (lines.size() > shown) n.lines.push_back("and " + std::to_string(lines.size() - shown) + " more");
        n.actions.push_back({NoticeAction::Type::OpenPanel, {}, "Details"});
        notices.push_back(std::move(n));
    }

    // Plugins in mods that wait for the player (design §14): a notice that
    // leads to the panel; nothing is approved without a click there.
    std::vector<std::string> waiting;
    for (const ModRow& m : facts.mod_plugins) {
        if (!m.waiting()) continue;
        const std::string what = m.state == "update-pending" ? "an update; the approved " + m.approved_version + " runs"
                                 : m.state == "conflict"     ? "the same id as mod(s) " + m.others
                                                             : "new";
        waiting.push_back(m.id + (m.version.empty() ? "" : " " + m.version) + " from mod '" + m.mod + "': " + what);
    }
    if (!waiting.empty()) {
        Notice n;
        n.id = "approvals";
        n.kind = NoticeKind::Info;
        n.title = std::to_string(waiting.size()) +
                  (waiting.size() == 1 ? " plugin from a mod waits for your approval" : " plugins from mods wait for your approval");
        const size_t shown = std::min(waiting.size(), kMaxListed);
        n.lines.assign(waiting.begin(), waiting.begin() + static_cast<std::ptrdiff_t>(shown));
        if (waiting.size() > shown) n.lines.push_back("and " + std::to_string(waiting.size() - shown) + " more");
        n.lines.push_back("Plugins are native code. Approved ones load from the next start.");
        n.actions.push_back({NoticeAction::Type::OpenPanel, {}, "Review"});
        notices.push_back(std::move(n));
    }
    return notices;
}

std::vector<Notice> session_notices(const SessionFacts& facts) {
    std::vector<Notice> notices;
    if (!facts.session) return notices;
    if (facts.ironman && facts.gameplay_active > 0) {
        Notice n;
        n.id = "ironman";
        n.kind = NoticeKind::Warning;
        n.lasting = true;
        n.title = "Ironman with gameplay plugins";
        n.lines.push_back(std::to_string(facts.gameplay_active) +
                          (facts.gameplay_active == 1 ? " plugin that changes the game acts"
                                                      : " plugins that change the game act") +
                          " in this ironman game because you allowed it. Achievements stay enabled, but such "
                          "plugins may break Paradox's rules for them.");
        notices.push_back(std::move(n));
    }
    if (facts.multiplayer) {
        Notice n;
        n.id = "multiplayer";
        n.kind = NoticeKind::Info;
        n.lasting = true;
        n.title = "Multiplayer";
        if (facts.gameplay_active == 0) {
            n.lines.push_back("No plugin that changes the game acts in this game.");
        } else {
            n.lines.push_back("Gameplay plugins: " + facts.gameplay_hash + " (" + std::to_string(facts.gameplay_active) +
                              " active). Every player should see the same.");
        }
        notices.push_back(std::move(n));
    }
    return notices;
}

namespace {
bool is_modifier(unsigned vk) {
    switch (vk) {
    case 0x10: case 0x11: case 0x12:  // VK_SHIFT, VK_CONTROL, VK_MENU
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5:  // left/right variants
    case 0x5B: case 0x5C:  // VK_LWIN, VK_RWIN
        return true;
    default:
        return false;
    }
}
}  // namespace

int64_t key_code(unsigned vk, bool shift, bool ctrl, bool alt) {
    if (vk == 0 || vk > 0xFF || is_modifier(vk)) return 0;
    return static_cast<int64_t>(vk) | (shift ? 0x100 : 0) | (ctrl ? 0x200 : 0) | (alt ? 0x400 : 0);
}

bool key_matches(int64_t code, unsigned vk, bool shift, bool ctrl, bool alt) {
    return code != 0 && key_code(vk, shift, ctrl, alt) == code;
}

}  // namespace overlay
