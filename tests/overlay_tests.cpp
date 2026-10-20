// anvil-overlay (plugins/anvil-overlay) without a device: which notices it
// shows (design §12, §13, §15), the panel key, and its client of anvil.control
// against the core's service on fake plugins.
#include "core/control.hpp"
#include "core/host.hpp"
#include "core/settings.hpp"
#include "test_support.hpp"

#include "../plugins/anvil-overlay/src/control_client.hpp"
#include "../plugins/anvil-overlay/src/notices.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace overlay;
namespace fs = std::filesystem;

PluginRow row(const std::string& id, const std::string& state, const std::string& reason = "", bool disabled = false) {
    PluginRow p;
    p.id = id;
    p.state = state;
    p.reason = reason;
    p.disabled = disabled;
    return p;
}

const Notice* find(const std::vector<Notice>& notices, const std::string& id) {
    for (const Notice& n : notices) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

size_t disable_actions(const Notice& n) {
    size_t count = 0;
    for (const auto& a : n.actions) count += a.type == NoticeAction::Type::DisablePlugin ? 1 : 0;
    return count;
}

}  // namespace

int main() {
    run("notices: a clean start shows none", [] {
        StartFacts facts;
        facts.plugins = {row("anvil-game", "loaded"), row("anvil-overlay", "loaded"), row("off", "skipped", "disabled", true)};
        CHECK(start_notices(facts).empty());
        CrashInfo old;
        old.is_new = false;
        old.level = "in_plugin_code";
        facts.crash = old;
        CHECK(start_notices(facts).empty());  // a crash already shown
    });

    run("notices: skipped and failed plugins", [] {
        StartFacts facts;
        facts.plugins = {row("a", "skipped", "requires plugin 'b', which is not loaded"), row("c", "failed", "boom"),
                         row("d", "skipped", "disabled in anvil.toml", true), row("e", "loaded")};
        const auto notices = start_notices(facts);
        const Notice* n = find(notices, "not-loaded");
        CHECK(notices.size() == 1 && n != nullptr && n->title == "2 plugins were not loaded" && !n->lasting);
        CHECK(n->lines.size() == 2 && n->lines[0].find("a: requires") == 0 && n->lines[1] == "c: failed: boom");
        for (int i = 0; i < 10; ++i) facts.plugins.push_back(row("x" + std::to_string(i), "skipped", "why"));
        const auto more = start_notices(facts);
        const Notice* many = find(more, "not-loaded");
        CHECK(many != nullptr);
        if (many != nullptr) CHECK(many->lines.size() == 7 && many->lines.back() == "and 6 more");
    });

    run("notices: the previous crash offers to disable the plugins it was in", [] {
        StartFacts facts;
        facts.plugins = {row("tongs", "loaded"), row("bellows", "loaded")};
        facts.plugins[1].disabled_next = true;
        CrashInfo crash;
        crash.is_new = true;
        crash.name = "2026-10-20_18-05-11";
        crash.level = "in_plugin_change";
        crash.summary = "in plugin tongs's change (also bellows)";
        crash.plugins = {"tongs", "bellows", "gone"};
        crash.before_game_ready = true;
        facts.crash = crash;
        const auto notices = start_notices(facts);
        const Notice* n = find(notices, "crash");
        CHECK(n != nullptr && n->kind == NoticeKind::Warning);
        if (n == nullptr) return;
        CHECK(n->title.find("before the game was ready") != std::string::npos);
        CHECK(n->lines[0] == "The crash was in plugin tongs's change (also bellows).");
        CHECK(n->lines[1] == "Report: anvil\\crashes\\2026-10-20_18-05-11.txt");
        // bellows is already disabled from the next start, gone is not a plugin of this start.
        CHECK(disable_actions(*n) == 1 && n->actions[0].plugin == "tongs");
        CHECK(n->actions.back().type == NoticeAction::Type::OpenPanel);
        facts.crash->level = "plugin_on_stack";
        CHECK(disable_actions(*find(start_notices(facts), "crash")) == 0);
        facts.crash->level = "no_plugin";
        CHECK(find(start_notices(facts), "crash")->lines[0] == "No plugin was involved.");
    });

    run("notices: safe mode explains itself", [] {
        StartFacts facts;
        facts.safe_mode = true;
        facts.unfinished_starts = 2;
        facts.plugins = {row("anvil-game", "loaded"), row("anvil-overlay", "loaded"), row("tongs", "skipped", "safe mode")};
        auto notices = start_notices(facts);
        const Notice* n = find(notices, "safe-mode");
        CHECK(notices.size() == 1 && n != nullptr && n->kind == NoticeKind::Error);
        if (n != nullptr) CHECK(n->lines[0].find("The last 2 starts") == 0);
        CrashInfo crash;
        crash.is_new = true;
        crash.level = "in_plugin_code";
        crash.summary = "in plugin tongs's code";
        crash.plugins = {"tongs"};
        facts.crash = crash;
        notices = start_notices(facts);
        CHECK(find(notices, "safe-mode")->lines.size() == 2 && find(notices, "crash") != nullptr);
        // A standard plugin left out for the crash still shows as not loaded.
        facts.plugins.push_back(row("anvil-x", "skipped", "safe mode; the crash before it was in plugin anvil-x's code"));
        CHECK(find(start_notices(facts), "not-loaded") != nullptr);
    });

    run("notices: ironman and multiplayer", [] {
        SessionFacts facts;
        CHECK(session_notices(facts).empty());
        facts.session = true;
        CHECK(session_notices(facts).empty());  // single player
        facts.ironman = true;
        CHECK(session_notices(facts).empty());  // no gameplay plugin acts
        facts.gameplay_active = 1;
        auto notices = session_notices(facts);
        CHECK(notices.size() == 1 && notices[0].id == "ironman" && notices[0].lasting);
        facts.ironman = false;
        facts.multiplayer = true;
        facts.gameplay_hash = "ABCD-EF01-2345-6789";
        notices = session_notices(facts);
        CHECK(notices.size() == 1 && notices[0].id == "multiplayer" && notices[0].lasting);
        CHECK(notices[0].lines[0].find("ABCD-EF01-2345-6789 (1 active)") != std::string::npos);
        facts.gameplay_active = 0;
        CHECK(session_notices(facts)[0].lines[0].find("No plugin") == 0);
        facts.session = false;
        CHECK(session_notices(facts).empty());
    });

    run("panel key: exact match with modifiers", [] {
        const int64_t ctrl_shift_f12 = 0x7B | 0x100 | 0x200;
        CHECK(key_matches(ctrl_shift_f12, VK_F12, true, true, false));
        CHECK(!key_matches(ctrl_shift_f12, VK_F12, false, true, false));
        CHECK(!key_matches(ctrl_shift_f12, VK_F12, true, true, true));
        CHECK(!key_matches(ctrl_shift_f12, VK_F11, true, true, false));
        CHECK(!key_matches(0, VK_F12, false, false, false));
        CHECK(key_code(VK_F11, false, false, false) == VK_F11);
        CHECK(key_code(VK_CONTROL, false, true, false) == 0 && key_code(VK_LSHIFT, true, false, false) == 0);
        CHECK(key_code('A', false, false, true) == ('A' | 0x400));
    });

    run("control client: the core's service through the overlay's client", [] {
        const fs::path dir = fs::temp_directory_path() / "anvil-overlay-client";
        std::error_code ignored;
        fs::remove_all(dir, ignored);
        fs::create_directories(dir / "anvil");
        std::vector<std::unique_ptr<AnvilPlugin>> plugins;
        auto p = std::make_unique<AnvilPlugin>();
        p->manifest = manifest(plugin_toml("anvil-overlay", "", "1.0.0", "early"));
        p->state = "loaded";
        auto schema = anvil::parse_settings_text(
            "[setting.mode]\ntype = \"enum\"\nvalues = [{ name = \"off\", label = \"Off\" }, { name = \"on\" }]\n"
            "default = \"on\"\nlabel = \"Mode\"\nrestart = false\n");
        CHECK(schema.file.has_value());
        p->settings.init(std::move(*schema.file), dir / "anvil" / "data" / "anvil-overlay" / "settings.toml",
                         "anvil-overlay", [](const std::string&) { return std::optional<std::string>(); });
        plugins.push_back(std::move(p));
        std::vector<anvil::Skipped> skipped{{"tongs", "safe mode", dir / "tongs", false}};
        anvil::ControlSources sources;
        sources.anvil_dir = dir / "anvil";
        sources.anvil_version = "1.1.0";
        sources.plugins = &plugins;
        sources.skipped = &skipped;
        anvil::Control control(sources);
        control.loading_done();
        ControlClient client(control.service());
        CHECK(static_cast<bool>(client) && client.status().plugins == 2);
        const auto rows = client.plugins();
        CHECK(rows.size() == 2 && rows[0].id == "anvil-overlay" && rows[0].early && rows[0].settings == 1);
        CHECK(rows[1].id == "tongs" && rows[1].state == "skipped" && rows[1].reason == "safe mode");
        CHECK(client.setting_count("anvil-overlay") == 1);
        const auto options = client.setting_options("anvil-overlay", 0);
        CHECK(options.size() == 2 && options[0].first == "off" && options[0].second == "Off" && options[1].second == "on");
        CHECK(client.setting_value("anvil-overlay", 0, ANVIL_CONTROL_VALUE_CURRENT) == "on");
        CHECK(client.set_text("anvil-overlay", 0, "off").empty());
        CHECK(client.setting_value("anvil-overlay", 0, ANVIL_CONTROL_VALUE_CURRENT) == "off");
        CHECK(!client.set_text("anvil-overlay", 0, "sideways").empty());
        CHECK(client.reset("anvil-overlay", 0).empty());
        CHECK(client.disable_next_start("tongs").empty() && client.plugins()[1].disabled_next);
        CHECK(!client.disable_next_start("missing").empty());
        CHECK(!client.previous_crash().has_value());
        CHECK(client.log_tail(10).empty());
        CHECK(split_list("a, b,,c ") == std::vector<std::string>({"a", "b", "c"}) && split_list("").empty());
        CHECK(!ControlClient().status().loading_done && ControlClient().plugins().empty());
        fs::remove_all(dir, ignored);
    });

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
