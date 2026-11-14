// settings.toml (design §5.3, §11): the schema, key bindings, the player's
// values, ANVIL_<ID>_<NAME> overrides and changes while the game runs.
#include "core/discovery.hpp"
#include "core/last_start.hpp"
#include "core/plugin_settings.hpp"
#include "core/retired_move.hpp"
#include "core/settings.hpp"
#include "test_support.hpp"
#include "ui/settings_order.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace anvil;

constexpr const char* kSchema = R"toml(# a comment
[setting.enabled]
type = "bool"
default = true
label = "Enabled"
restart = false

[setting.budget_mb]
type = "int"
default = 1024
min = 64
max = 8192
step = 64
label = "RAM prefetch budget (MB)"
description = "RAM for unpacked archive members."
group = "RAM prefetch"

[setting.scale]
type = "float"
default = 1
min = 0.5
max = 2.5
label = "Scale"
restart = false

[setting.mode]
type = "enum"
values = [{ name = "off", label = "Off" }, { name = "verify", label = "Verify only" }, { name = "on" }]
default = "verify"
label = "Mode"
restart = false

[setting.greeting]
type = "string"
default = "hello"
label = "Greeting"
restart = false

[setting.panel_key]
type = "key"
default = "Ctrl+Shift+F12"
label = "Panel key"
)toml";

SettingsFile schema() {
    const SettingsFileResult r = parse_settings_text(kSchema);
    if (!r.file) std::printf("  schema error: %s\n", r.error.c_str());
    return r.file ? *r.file : SettingsFile{};
}

// The error of a settings.toml with one [setting.x] table of `body`.
SettingsFileResult one(const std::string& body) { return parse_settings_text("[setting.x]\n" + body); }

bool fails(const std::string& body, const std::string& part, uint32_t line = 0) {
    const SettingsFileResult r = one(body);
    const bool ok = !r.file && r.error.find(part) != std::string::npos && (line == 0 || r.line == line);
    if (!ok) std::printf("  expected \"%s\" at line %u, got \"%s\" at line %u\n", part.c_str(), line, r.error.c_str(), r.line);
    return ok;
}

std::string read(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void write(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

std::filesystem::path scratch(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / ("anvil_settings_tests_" + std::to_string(GetCurrentProcessId())) / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

struct Recorder {
    std::vector<AnvilSetting> calls;
    static void callback(AnvilSetting setting, void* context) { static_cast<Recorder*>(context)->calls.push_back(setting); }
};

PluginSettings::Environment environment(std::map<std::string, std::string> values) {
    return [values](const std::string& name) -> std::optional<std::string> {
        const auto it = values.find(name);
        if (it == values.end()) return std::nullopt;
        return it->second;
    };
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// A plugin that changes the game, with a module that does and one that does not.
constexpr const char* kModular = R"toml(id = "lode"
name = "Lode"
version = "1.2.0"
api = 2
entry = "lode.dll"
affects_gameplay = true
allow_multiplayer = "opt-in"

[[modules]]
id = "flows"
name = "Flows"
group = "Trade"
affects_gameplay = true
allow_multiplayer = "opt-in"

[[modules]]
id = "log"
name = "Log"
affects_gameplay = false
)toml";

constexpr const char* kModuleSettings = R"toml([setting.flip_ratio]
type = "float"
default = 1.5
label = "Flip ratio"
module = "flows"

[setting.log_flows]
type = "bool"
default = false
label = "Log flows"
module = "flows"
affects_gameplay = false
advanced = true

[setting.cooldown_months]
type = "int"
default = 24
label = "Cooldown"

[setting.panel_key]
type = "key"
default = "F9"
label = "Panel key"

[setting.verbose]
type = "bool"
default = false
label = "Verbose"
module = "log"

[retired.flows_mode]
module = "flows"
off = ["off", false, 0]
)toml";

void module_settings_tests() {
    run("settings: module, advanced and affects_gameplay (ADR 0013)", [] {
        const SettingsFileResult r = parse_settings_text(kModuleSettings);
        CHECK(r.file.has_value());
        if (!r.file) return;
        const SettingsFile& f = *r.file;
        CHECK(f.settings.size() == 5 && f.settings[0].module == "flows" && f.settings[0].module_line == 5);
        CHECK(!f.settings[0].advanced && !f.settings[0].affects_gameplay && f.settings[1].advanced);
        CHECK(f.settings[1].affects_gameplay == false && f.settings[2].module.empty());
        CHECK(f.retired.size() == 1 && f.retired[0].name == "flows_mode" && f.retired[0].module == "flows" &&
              f.retired[0].line == 31 && f.retired[0].module_line == 32 && f.retired[0].off.size() == 3);
        CHECK(f.find_retired("flows_mode") == &f.retired[0] && f.find_retired("flip_ratio") == nullptr);
        CHECK(fails("type = \"bool\"\ndefault = true\nlabel = \"X\"\nadvanced = 1\n", "advanced must be true or false", 5));
        CHECK(fails("type = \"bool\"\ndefault = true\nlabel = \"X\"\nmodule = \"\"\n", "module must not be empty", 5));
        CHECK(fails("type = \"bool\"\ndefault = true\nlabel = \"X\"\naffects_gameplay = \"no\"\n",
                    "affects_gameplay must be true or false", 5));

        const Manifest m = manifest(kModular);
        CHECK(m.modules[0].group == "Trade" && m.modules[1].group.empty());
        uint32_t line = 0;
        CHECK(settings_module_error(f, m, line).empty());
        // Which settings count: the owner changes the game, and the setting does not say false.
        CHECK(setting_counts(f.settings[0], m));    // flows, float
        CHECK(!setting_counts(f.settings[1], m));   // affects_gameplay = false
        CHECK(setting_counts(f.settings[2], m));    // the plugin's own, which affects gameplay
        CHECK(!setting_counts(f.settings[3], m));   // a key defaults to false
        CHECK(!setting_counts(f.settings[4], m));   // module log does not change the game
        Setting key = f.settings[3];
        key.affects_gameplay = true;
        CHECK(setting_counts(key, m));
        const Manifest calm = manifest(plugin_toml("calm"));
        Setting own = f.settings[2];
        own.affects_gameplay = true;
        CHECK(!setting_counts(own, calm));  // nothing of calm changes the game
    });

    run("settings: module must name a [[modules]] id, retired names are checked", [] {
        const Manifest m = manifest(kModular);
        uint32_t line = 0;
        auto r = parse_settings_text("[setting.a]\ntype = \"bool\"\ndefault = true\nlabel = \"A\"\nmodule = \"flow\"\n");
        CHECK(r.file && settings_module_error(*r.file, m, line) ==
                            "settings.toml: setting a: module 'flow' is not declared in plugin.toml [[modules]]" &&
              line == 5);
        r = parse_settings_text("[retired.old]\nmodule = \"gone\"\noff = [false]\n");
        CHECK(r.file && settings_module_error(*r.file, m, line) ==
                            "settings.toml: retired old: module 'gone' is not declared in plugin.toml [[modules]]" &&
              line == 2);
        CHECK(r.file && contains(settings_module_error(*r.file, manifest(plugin_toml("flat")), line), "which has no [[modules]]"));
        // An opt-in the module does not offer (flows offers multiplayer only).
        r = parse_settings_text("[retired.old]\nmodule = \"flows\"\nmultiplayer = [true]\n\n[retired.older]\nmodule = \"flows\"\nironman = [true]\n");
        CHECK(r.file && settings_module_error(*r.file, m, line) ==
                            "settings.toml: retired older: ironman needs module 'flows' to offer allow_ironman = \"opt-in\" in plugin.toml" &&
              line == 5);

        const auto error = [](const std::string& text, const std::string& part, uint32_t at) {
            const SettingsFileResult bad = parse_settings_text(text);
            const bool ok = !bad.file && contains(bad.error, part) && bad.line == at;
            if (!ok) std::printf("  expected \"%s\" at %u, got \"%s\" at %u\n", part.c_str(), at, bad.error.c_str(), bad.line);
            return ok;
        };
        CHECK(error("[retired.old]\noff = [false]\n", "retired old: missing 'module'", 1));
        // Each list is optional: with none the values are only removed.
        CHECK(parse_settings_text("[retired.old]\nmodule = \"m\"\n").file.has_value());
        CHECK(error("[retired.old]\nmodule = \"m\"\noff = []\n", "off must be a non-empty array", 3));
        CHECK(error("[retired.old]\nmodule = \"m\"\non = true\n", "on must be a non-empty array", 3));
        CHECK(error("[retired.old]\nmodule = \"m\"\nironman = [{ a = 1 }]\n", "each of ironman must be a bool", 3));
        CHECK(error("[retired.old]\nmodule = \"m\"\noff = [\"no\", 0]\non = [\"yes\", 0.0]\n",
                    "retired old: a value cannot be in both off and on: 0", 1));
        CHECK(error("[retired.old]\nmodule = \"m\"\noff = [[1]]\n", "each of off must be a bool", 3));
        CHECK(error("[retired.old]\nmodule = \"m\"\noff = [1]\nlabel = \"x\"\n", "retired old: unknown key 'label'", 4));
        CHECK(error("[retired.Old]\nmodule = \"m\"\noff = [1]\n", "the name must match", 1));
        CHECK(error("[setting.old]\ntype = \"bool\"\ndefault = true\nlabel = \"O\"\n[retired.old]\nmodule = \"m\"\noff = [1]\n",
                    "retired old: a setting of this name is declared", 5));
        CHECK(error("retired = 1\n", "retired must be a table", 1));
    });

    run("settings: the retire move on the player's text (ADR 0013, point 4)", [] {
        const SettingsFileResult r = parse_settings_text(kModuleSettings);
        if (!r.file) {
            CHECK(false);
            return;
        }
        std::string error;
        // A value in `off`, another setting kept, a comment kept.
        auto move = plan_retired_move(*r.file, "# mine\nflows_mode = \"off\"\ncooldown_months = 12\n", error);
        CHECK(move && move->items.size() == 1 && move->items[0].turns_off && move->items[0].module == "flows" &&
              move->items[0].value == "\"off\"" && move->items[0].line == 2);
        CHECK(move && move->text == "# mine\ncooldown_months = 12\n");
        // false and 0 are in `off` too; 0.0 compares as a number.
        for (const char* value : {"false", "0", "0.0"}) {
            move = plan_retired_move(*r.file, std::string("flows_mode = ") + value + "\n", error);
            CHECK(move && move->items.size() == 1 && move->items[0].turns_off && move->text.empty());
        }
        // A value not in `off` is removed as well, and turns nothing off.
        move = plan_retired_move(*r.file, "flows_mode = \"verify\"\r\nverbose = true\r\n", error);
        CHECK(move && move->items.size() == 1 && !move->items[0].turns_off && move->items[0].value == "\"verify\"");
        CHECK(move && move->text == "verbose = true\r\n");
        move = plan_retired_move(*r.file, "flows_mode = true\n", error);
        CHECK(move && !move->items[0].turns_off);  // true is not false
        // Nothing retired in the file: nothing to do.
        move = plan_retired_move(*r.file, "verbose = true\n", error);
        CHECK(move && move->items.empty());
        // A multi-line value goes with all its lines; only comments left: no file.
        move = plan_retired_move(*r.file, "flows_mode = \"\"\"\noff\"\"\"\n# end\n", error);
        CHECK(move && move->items.size() == 1 && move->text.empty());
        // A file that does not parse waits.
        CHECK(!plan_retired_move(*r.file, "flows_mode = \n", error) && contains(error, "line 1"));
        CHECK(!plan_retired_move(*r.file, "[flows_mode]\na = 1\n", error) && contains(error, "is a table"));
        // A schema without retired settings never reads the text.
        const SettingsFile plain = schema();
        move = plan_retired_move(plain, "not toml at all = = =", error);
        CHECK(move && move->items.empty());
    });

    run("settings: the retire move on the files, before modules resolve", [] {
        const SettingsFileResult r = parse_settings_text(kModuleSettings);
        if (!r.file) {
            CHECK(false);
            return;
        }
        const Manifest m = manifest(kModular);
        const auto dir = scratch("retire");
        const auto player = dir / "data" / "lode" / "settings.toml";
        const auto toml = dir / "anvil.toml";
        write(toml, "# the player's\nenabled_modules = [\"other.thing\"]\n");
        write(player, "flows_mode = \"off\"\ncooldown_months = 12\n");
        AnvilSettings lists;
        auto notes = move_retired_settings(m, *r.file, player, toml, lists);
        CHECK(lists.disabled_modules == std::vector<std::string>{"lode.flows"});
        CHECK(read(toml) == "# the player's\nenabled_modules = [\"other.thing\"]\ndisabled_modules = [\"lode.flows\"]\n");
        CHECK(read(player) == "cooldown_months = 12\n");
        CHECK(notes.size() == 2 && notes[0].level == ANVIL_LOG_INFO &&
              notes[0].message == "retired setting flows_mode = \"off\" (settings.toml line 1) meant module lode.flows "
                                  "off: added it to disabled_modules in anvil.toml, off from this start");
        CHECK(notes.size() == 2 &&
              notes[1].message == "removed the retired setting(s) flows_mode = \"off\" from settings.toml");
        // Once: the next start finds nothing to move.
        CHECK(move_retired_settings(m, *r.file, player, toml, lists).empty() && lists.disabled_modules.size() == 1);
        // The module is off at this start through the list the loader reads.
        CHECK(!module_enabled(lists, "lode.flows", true));

        // A value not in `off`: removed, the module keeps its switch.
        write(player, "flows_mode = \"on\"\n");
        lists = {};
        std::filesystem::remove(toml);
        notes = move_retired_settings(m, *r.file, player, toml, lists);
        CHECK(lists.disabled_modules.empty() && !std::filesystem::exists(toml) && !std::filesystem::exists(player));
        CHECK(notes.size() == 1 && contains(notes[0].message, "flows_mode = \"on\"") &&
              contains(notes[0].message, "the file is removed"));

        // Listed already: no second entry, the value still goes.
        write(player, "flows_mode = false\n");
        lists.disabled_modules = {"lode.flows"};
        notes = move_retired_settings(m, *r.file, player, toml, lists);
        CHECK(lists.disabled_modules.size() == 1 && !std::filesystem::exists(player) && !std::filesystem::exists(toml));
        CHECK(!notes.empty() && contains(notes[0].message, "meant module lode.flows off; anvil.toml has it so already"));

        // anvil.toml cannot be written (it does not parse): the player's file stays for the next start.
        write(toml, "disabled_modules = [\n");
        write(player, "flows_mode = \"off\"\n");
        lists = {};
        notes = move_retired_settings(m, *r.file, player, toml, lists);
        CHECK(lists.disabled_modules.empty() && read(player) == "flows_mode = \"off\"\n" && notes.size() == 1 &&
              notes[0].level == ANVIL_LOG_WARNING && contains(notes[0].message, "the values stay for the next start"));

        // A player's file that does not parse: a warning, nothing changes.
        write(player, "flows_mode = \n");
        notes = move_retired_settings(m, *r.file, player, dir / "none.toml", lists);
        CHECK(notes.size() == 1 && notes[0].level == ANVIL_LOG_WARNING && read(player) == "flows_mode = \n");
        // No player's file: nothing.
        CHECK(move_retired_settings(m, *r.file, dir / "missing.toml", toml, lists).empty());
    });

    run("settings: retired values turn a module on and allow it, by the rule of the module switch", [] {
        const std::string extra = std::string(kModular) +
                                  "\n[[modules]]\nid = \"extra\"\nname = \"Extra\"\naffects_gameplay = true\n"
                                  "allow_ironman = \"opt-in\"\nallow_multiplayer = \"opt-in\"\ndefault = false\n";
        const Manifest m = manifest(extra);
        const SettingsFileResult r = parse_settings_text(R"toml([retired.extra_mode]
module = "extra"
off = ["off"]
on = ["on", "verify"]

[retired.extra_ironman]
module = "extra"
ironman = [true]

[retired.extra_multiplayer]
module = "extra"
multiplayer = [true]

[retired.flows_switch]
module = "flows"
on = [true]
off = [false]

[retired.old_note]
module = "extra"
)toml");
        uint32_t line = 0;
        CHECK(r.file && settings_module_error(*r.file, m, line).empty());
        if (!r.file) return;
        const auto dir = scratch("retire-on");
        const auto player = dir / "data" / "lode" / "settings.toml";
        const auto toml = dir / "anvil.toml";

        // On (off by default: into enabled_modules), allowed in ironman; false
        // is in no list of extra_multiplayer and a name with no list goes too.
        // flows was off by the player's anvil.toml: the retired value wins.
        write(toml, "# mine\ndisabled_modules = [\"lode.flows\"]\n");
        write(player, "extra_mode = \"verify\"\nextra_ironman = true\nextra_multiplayer = false\nold_note = \"x\"\nflows_switch = true\n");
        AnvilSettings lists = read_anvil_settings(toml);
        auto notes = move_retired_settings(m, *r.file, player, toml, lists);
        CHECK(lists.enabled_modules == std::vector<std::string>{"lode.extra"} &&
              lists.allow_ironman == std::vector<std::string>{"lode.extra"} && lists.allow_multiplayer.empty() &&
              lists.disabled_modules.empty());
        const AnvilSettings written = read_anvil_settings(toml);
        CHECK(written.enabled_modules == lists.enabled_modules && written.allow_ironman == lists.allow_ironman &&
              written.disabled_modules.empty() && read(toml).starts_with("# mine\n"));
        CHECK(module_enabled(lists, "lode.extra", false) && module_enabled(lists, "lode.flows", true));
        CHECK(!std::filesystem::exists(player));
        CHECK(notes.size() == 4 &&
              notes[0].message == "retired setting extra_mode = \"verify\" (settings.toml line 1) meant module lode.extra on: "
                                  "added it to enabled_modules in anvil.toml, on from this start" &&
              notes[1].message == "retired setting extra_ironman = true (settings.toml line 2) meant module lode.extra "
                                  "allowed in ironman: added it to allow_ironman in anvil.toml, allowed in ironman from this start" &&
              notes[2].message == "retired setting flows_switch = true (settings.toml line 5) meant module lode.flows on: "
                                  "took it out of disabled_modules in anvil.toml, on from this start" &&
              contains(notes[3].message, "removed the retired setting(s) extra_mode = \"verify\", extra_ironman = true, "
                                         "extra_multiplayer = false, old_note = \"x\", flows_switch = true"));

        // Off again (off by default: out of enabled_modules, never into
        // disabled_modules); a module on by default goes into disabled_modules.
        write(player, "extra_mode = \"off\"\nflows_switch = false\n");
        notes = move_retired_settings(m, *r.file, player, toml, lists);
        CHECK(lists.enabled_modules.empty() && lists.disabled_modules == std::vector<std::string>{"lode.flows"} &&
              lists.allow_ironman == std::vector<std::string>{"lode.extra"});
        CHECK(!notes.empty() && contains(notes[0].message, "meant module lode.extra off: took it out of enabled_modules"));
        // As it is already: no write, an info line, the value still goes.
        const std::string before = read(toml);
        write(player, "extra_mode = \"off\"\nextra_ironman = true\n");
        notes = move_retired_settings(m, *r.file, player, toml, lists);
        CHECK(read(toml) == before && !std::filesystem::exists(player) && notes.size() == 3 &&
              contains(notes[0].message, "lode.extra off; anvil.toml has it so already") &&
              contains(notes[1].message, "lode.extra allowed in ironman; anvil.toml has it so already"));
        // On again: back into enabled_modules.
        write(player, "extra_mode = \"on\"\n");
        move_retired_settings(m, *r.file, player, toml, lists);
        CHECK(lists.enabled_modules == std::vector<std::string>{"lode.extra"});
    });

    run("last-start.toml: written, read back, unknown keys and other formats (ADR 0013, point 8)", [] {
        LastStart r;
        r.anvil_version = "1.23.0";
        r.written = "2026-11-14T18:05:11Z";
        r.disabled_plugins = {"tongs"};
        r.allow_multiplayer = {"lode", "lode.flows"};
        r.disabled_modules = {"bellows.diagnostics"};
        r.approvals = {{"trade-tweaks", "mod/ugc_123.mod", "1.0.2", "ab12"}};
        r.settings["lode"] = {{"flip_ratio", "1.5"}, {"cooldown_months", "24"}, {"mode", "\"on\""}, {"on", "true"}};
        r.settings["anvil-overlay"] = {{"panel_key", "\"Ctrl+Shift+F12\""}, {"scale", "1.0"}};
        const std::string text = last_start_text(r);
        CHECK(contains(text, "format = 1\nanvil = \"1.23.0\"\n"));
        CHECK(contains(text, "allow_multiplayer = [\"lode\", \"lode.flows\"]\n"));
        CHECK(contains(text, "[settings.\"anvil-overlay\"]\npanel_key = \"Ctrl+Shift+F12\"\nscale = 1.0\n"));
        CHECK(contains(text, "[[approval]]\nid = \"trade-tweaks\"\nmod = \"mod/ugc_123.mod\"\n"));
        std::string error;
        const auto back = parse_last_start(text, error);
        CHECK(back && *back == r);

        // The values as the settings write them, so the Manager compares text with setting_toml.
        const SettingsFile s = schema();
        CHECK(back && back->settings.at("lode").at("flip_ratio") == "1.5");
        CHECK(setting_toml(s.settings[2], {0, 1.0, {}}) == "1.0");  // scale: a float keeps its ".0"

        // A newer Anvil may add keys; another format is not read.
        CHECK(parse_last_start(text + "\n[future]\nx = 1\n", error) == r);
        CHECK(!parse_last_start("format = 2\n", error) && error == "not format 1");
        CHECK(!parse_last_start("format = \n", error) && contains(error, "line 1"));

        const auto dir = scratch("last-start");
        const auto file = dir / "data" / "anvil" / "last-start.toml";
        CHECK(!read_last_start(file));  // missing: nothing is marked
        CHECK(write_last_start(file, r, error) && read(file) == text && read_last_start(file) == r);
        write(file, "not = = toml");
        CHECK(!read_last_start(file));  // unreadable: nothing is marked
    });
}

}  // namespace

void settings_tests() {
    run("settings: every type, in declaration order", [] {
        const SettingsFile f = schema();
        CHECK(f.settings.size() == 6);
        if (f.settings.size() != 6) return;
        const Setting& enabled = f.settings[0];
        CHECK(enabled.name == "enabled" && enabled.id == 0 && enabled.line == 2 && enabled.type == SettingType::Bool);
        CHECK(enabled.default_value.integer == 1 && !enabled.restart && enabled.label == "Enabled");
        const Setting& budget = *f.find("budget_mb");
        CHECK(budget.id == 1 && budget.type == SettingType::Int && budget.default_value.integer == 1024);
        CHECK(budget.min->integer == 64 && budget.max->integer == 8192 && budget.step->integer == 64);
        CHECK(budget.restart && budget.group == "RAM prefetch" && budget.description == "RAM for unpacked archive members.");
        const Setting& scale = *f.find("scale");
        CHECK(scale.type == SettingType::Float && scale.default_value.real == 1.0 && scale.min->real == 0.5);
        const Setting& mode = *f.find("mode");
        CHECK(mode.type == SettingType::Enum && mode.options.size() == 3 && mode.default_value.integer == 1);
        CHECK(mode.options[1].label == "Verify only" && mode.options[2].label == "on");  // label defaults to the name
        CHECK(f.find("greeting")->default_value.text == "hello");
        const Setting& key = *f.find("panel_key");
        CHECK(key.id == 5 && key.default_value.integer == (kKeyCtrl | kKeyShift | 0x7B));
        CHECK(f.get(6) == nullptr && f.find("nope") == nullptr);
        CHECK(parse_settings_text("").file->settings.empty());  // a plugin may declare none
    });

    run("settings: the overlay and the Manager show each group once", [] {
        using Order = std::vector<std::size_t>;
        using anvil::ui::settings_display_order;
        CHECK(settings_display_order({}).empty());
        CHECK(settings_display_order({"", "", ""}) == (Order{0, 1, 2}));
        CHECK(settings_display_order({"A", "A", "A"}) == (Order{0, 1, 2}));
        // Interleaved groups: each once, in the order of its first setting; file order inside a group.
        CHECK(settings_display_order({"Start-up", "Loading", "Start-up", "Simulation", "Loading", "Simulation", "Start-up"}) ==
              (Order{0, 2, 6, 1, 4, 3, 5}));
        // Ungrouped settings come first, wherever they stand in the file.
        CHECK(settings_display_order({"A", "", "B", "A", ""}) == (Order{1, 4, 0, 3, 2}));
        // The schema's groups: RAM prefetch (budget_mb), after the ungrouped ones.
        const SettingsFile f = schema();
        std::vector<std::string_view> groups;
        for (const Setting& s : f.settings) groups.emplace_back(s.group);
        CHECK(settings_display_order(groups) == (Order{0, 2, 3, 4, 5, 1}));
    });

    run("settings: the schema refuses what it does not know", [] {
        CHECK(fails("type = \"int\"\ndefault = 1\nlabel = \"X\"\ncolour = 1\n", "setting x: unknown key 'colour'", 5));
        CHECK(fails("default = 1\nlabel = \"X\"\n", "missing 'type'", 1));
        CHECK(fails("type = \"number\"\ndefault = 1\nlabel = \"X\"\n", "type must be bool, int, float, enum, string or key", 2));
        CHECK(fails("type = \"int\"\nlabel = \"X\"\n", "missing 'default'", 1));
        CHECK(fails("type = \"int\"\ndefault = 1\n", "missing 'label'", 1));
        CHECK(fails("type = \"int\"\ndefault = 1\nlabel = \"\"\n", "label must not be empty", 4));
        CHECK(fails("type = \"int\"\ndefault = 1.5\nlabel = \"X\"\n", "default must be an integer", 3));
        CHECK(fails("type = \"bool\"\ndefault = 1\nlabel = \"X\"\n", "default must be true or false", 3));
        CHECK(fails("type = \"float\"\ndefault = nan\nlabel = \"X\"\n", "default must be a finite number", 3));
        CHECK(fails("type = \"int\"\ndefault = 1\nmin = 5\nlabel = \"X\"\n", "default: 1 is below the minimum 5", 3));
        CHECK(fails("type = \"int\"\ndefault = 9\nmax = 5\nlabel = \"X\"\n", "default: 9 is above the maximum 5", 3));
        CHECK(fails("type = \"int\"\ndefault = 1\nmin = 5\nmax = 2\nlabel = \"X\"\n", "max 2 is below min 5", 5));
        CHECK(fails("type = \"float\"\ndefault = 1\nstep = 0\nlabel = \"X\"\n", "step must be greater than 0", 4));
        CHECK(fails("type = \"bool\"\ndefault = true\nmin = 0\nlabel = \"X\"\n", "min applies only to int and float settings", 4));
        CHECK(fails("type = \"int\"\ndefault = 1\nvalues = []\nlabel = \"X\"\n", "values apply only to enum settings", 4));
        CHECK(fails("type = \"enum\"\ndefault = \"a\"\nlabel = \"X\"\n", "an enum setting needs 'values'", 1));
        CHECK(fails("type = \"enum\"\nvalues = []\ndefault = \"a\"\nlabel = \"X\"\n", "values must be a non-empty array", 3));
        CHECK(fails("type = \"enum\"\nvalues = [{ name = \"a\" }, { name = \"a\" }]\ndefault = \"a\"\nlabel = \"X\"\n",
                    "values: 'a' is declared twice"));
        CHECK(fails("type = \"enum\"\nvalues = [{ name = \"A\" }]\ndefault = \"A\"\nlabel = \"X\"\n",
                    "values: name 'A' must match [a-z][a-z0-9_]*"));
        CHECK(fails("type = \"enum\"\nvalues = [{ label = \"A\" }]\ndefault = \"a\"\nlabel = \"X\"\n", "an option without 'name'"));
        CHECK(fails("type = \"enum\"\nvalues = [{ name = \"a\", hint = 1 }]\ndefault = \"a\"\nlabel = \"X\"\n",
                    "values: unknown key 'hint'"));
        CHECK(fails("type = \"enum\"\nvalues = [{ name = \"a\" }, { name = \"b\" }]\ndefault = \"c\"\nlabel = \"X\"\n",
                    "default: \"c\" is not one of a, b", 4));
        CHECK(fails("type = \"key\"\ndefault = \"Ctrl+Hyper+K\"\nlabel = \"X\"\n", "is not a key binding", 3));
        CHECK(fails("type = \"bool\"\ndefault = true\nlabel = \"X\"\nrestart = \"yes\"\n", "restart must be true or false", 5));
        const SettingsFileResult name = parse_settings_text("[setting.Bad-Name]\ntype = \"bool\"\n");
        CHECK(!name.file && name.error.find("the name must match") != std::string::npos && name.line == 1);
        const SettingsFileResult top = parse_settings_text("version = 1\n");
        CHECK(!top.file && top.error == "settings.toml: unknown key 'version'" && top.line == 1);
        const SettingsFileResult syntax = parse_settings_text("[setting.x\n");
        CHECK(!syntax.file && syntax.error.starts_with("settings.toml: ") && syntax.line == 1);
    });

    run("settings: key bindings", [] {
        CHECK(parse_key("Ctrl+Shift+F12") == (kKeyCtrl | kKeyShift | 0x7B));
        CHECK(parse_key("shift+ctrl+f12") == parse_key("Ctrl+Shift+F12"));
        CHECK(parse_key("control+alt+delete") == (kKeyCtrl | kKeyAlt | 0x2E));
        CHECK(parse_key("A") == 0x41 && parse_key("7") == 0x37 && parse_key("F24") == 0x87 && parse_key("Numpad5") == 0x65);
        CHECK(parse_key("") == 0);
        CHECK(!parse_key("F25") && !parse_key("F0") && !parse_key("Ctrl") && !parse_key("Ctrl+") && !parse_key("Ctrl+Ctrl+A"));
        CHECK(!parse_key("+") && !parse_key("A+B") && !parse_key("Hyper+A"));
        CHECK(key_name(kKeyCtrl | kKeyShift | 0x7B) == "Ctrl+Shift+F12" && key_name(0x41 | kKeyAlt) == "Alt+A");
        CHECK(key_name(0) == "" && key_name(0x07) == "" && key_name(0x10000) == "");
        CHECK(key_name(*parse_key("backquote")) == "Backquote");
    });

    run("settings: values from text and as TOML", [] {
        const SettingsFile f = schema();
        if (f.settings.size() != 6) return;
        std::string error;
        CHECK(parse_setting_text(f.settings[0], "FALSE", error)->integer == 0);
        CHECK(parse_setting_text(f.settings[0], "1", error)->integer == 1);
        CHECK(!parse_setting_text(f.settings[0], "yes", error) && error == "\"yes\" is not true or false");
        error.clear();
        CHECK(parse_setting_text(f.settings[1], "2048", error)->integer == 2048);
        CHECK(!parse_setting_text(f.settings[1], "99999", error) && error == "99999 is above the maximum 8192");
        error.clear();
        CHECK(!parse_setting_text(f.settings[1], "12a", error) && error == "\"12a\" is not an integer");
        error.clear();
        CHECK(parse_setting_text(f.settings[2], "2.25", error)->real == 2.25);
        CHECK(!parse_setting_text(f.settings[2], "0.25", error) && error == "0.25 is below the minimum 0.5");
        error.clear();
        CHECK(parse_setting_text(f.settings[3], "on", error)->integer == 2);
        CHECK(!parse_setting_text(f.settings[3], "On", error) && error == "\"On\" is not one of off, verify, on");
        error.clear();
        CHECK(parse_setting_text(f.settings[4], "", error)->text.empty());
        CHECK(parse_setting_text(f.settings[5], "Alt+A", error)->integer == (kKeyAlt | 0x41));
        CHECK(setting_toml(f.settings[0], {1, 0, {}}) == "true" && setting_toml(f.settings[2], {0, 2.0, {}}) == "2.0");
        CHECK(setting_toml(f.settings[2], {0, 0.1, {}}) == "0.1" && setting_toml(f.settings[3], {2, 0, {}}) == "\"on\"");
        CHECK(setting_toml(f.settings[4], {0, 0, "a \"b\"\n\\"}) == "\"a \\\"b\\\"\\n\\\\\"");
        CHECK(setting_toml(f.settings[5], {kKeyCtrl | 0x70, 0, {}}) == "\"Ctrl+F1\"");
        CHECK(setting_environment_name("ram-prefetch", "budget_mb") == "ANVIL_RAM_PREFETCH_BUDGET_MB");
    });

    run("settings: the player's file keeps only what differs, and invalid values fall back", [] {
        const SettingsFile f = schema();
        if (f.settings.size() != 6) return;
        const PlayerSettings p = parse_player_settings(f, "budget_mb = 99999\nscale = 2\nmode = \"on\"\ncolour = 3\n"
                                                          "enabled = \"no\"\ngreeting = \"hi\"\npanel_key = \"\"\n");
        CHECK(!p.values[1] && p.values[2]->real == 2.0 && p.values[3]->integer == 2 && !p.values[0]);
        CHECK(p.values[4]->text == "hi" && p.values[5]->integer == 0);
        CHECK(p.warnings.size() == 3);
        if (p.warnings.size() == 3) {
            CHECK(p.warnings[0] == "line 1: budget_mb: 99999 is above the maximum 8192; the default 1024 applies");
            CHECK(p.warnings[1] == "line 4: 'colour' is not a setting of this plugin; ignored");
            CHECK(p.warnings[2] == "line 5: enabled must be true or false; the default true applies");
        }
        const PlayerSettings broken = parse_player_settings(f, "budget_mb = \n");
        CHECK(broken.warnings.size() == 1 && broken.warnings[0].starts_with("line 1: ") && !broken.values[1]);

        std::vector<SettingValue> values;
        for (const auto& s : f.settings) values.push_back(s.default_value);
        CHECK(player_settings_text(f, values).empty());
        values[1].integer = 2048;
        values[3].integer = 0;
        const std::string text = player_settings_text(f, values);
        CHECK(text.starts_with("# ") && text.ends_with("\nbudget_mb = 2048\nmode = \"off\"\n"));
        const PlayerSettings again = parse_player_settings(f, text);
        CHECK(again.warnings.empty() && again.values[1]->integer == 2048 && again.values[3]->integer == 0 && !again.values[0]);

        const auto dir = scratch("player");
        const auto file = dir / "data" / "x" / "settings.toml";
        std::string error;
        CHECK(write_player_settings(f, values, file, error) && read(file) == text && error.empty());
        CHECK(!std::filesystem::exists(file.string() + ".tmp"));
        CHECK(load_player_settings(f, file).values[1]->integer == 2048);
        for (const auto& s : f.settings) values[s.id] = s.default_value;
        CHECK(write_player_settings(f, values, file, error) && !std::filesystem::exists(file));
        CHECK(load_player_settings(f, file).warnings.empty());  // a missing file: the defaults
    });

    run("settings: the store starts from defaults, the player's file and the environment", [] {
        const auto dir = scratch("store");
        const auto file = dir / "settings.toml";
        write(file, "budget_mb = 2048\nscale = 9\n");
        PluginSettings store;
        const auto notes = store.init(schema(), file, "ram-prefetch",
                                      environment({{"ANVIL_RAM_PREFETCH_MODE", "off"}, {"ANVIL_RAM_PREFETCH_ENABLED", "maybe"}}));
        CHECK(notes.size() == 3);
        if (notes.size() == 3) {
            CHECK(notes[0].level == ANVIL_LOG_WARNING &&
                  notes[0].message == "data/ram-prefetch/settings.toml, line 2: scale: 9.0 is above the maximum 2.5; "
                                      "the default 1.0 applies");
            CHECK(notes[1].level == ANVIL_LOG_WARNING &&
                  notes[1].message == "ANVIL_RAM_PREFETCH_ENABLED: \"maybe\" is not true or false; ignored");
            CHECK(notes[2].level == ANVIL_LOG_INFO &&
                  notes[2].message == "setting mode = \"off\" from ANVIL_RAM_PREFETCH_MODE (environment)");
        }
        CHECK(store.warnings().size() == 2);
        CHECK(store.get_bool(0) == 1 && store.get_int(0) == 1);
        CHECK(store.get_int(1) == 2048 && store.get_float(1) == 2048.0 && std::string(store.get_string(1)).empty());
        CHECK(store.get_float(2) == 1.0 && store.get_int(2) == 0);
        CHECK(store.get_int(3) == 0 && std::string(store.get_string(3)) == "off");
        CHECK(store.saved(3)->integer == 1 && store.current(3)->integer == 0);  // the override is not saved
        CHECK(std::string(store.get_string(4)) == "hello" && store.get_int(4) == 0 && store.get_bool(4) == 0);
        CHECK(store.get_int(5) == (kKeyCtrl | kKeyShift | 0x7B) && std::string(store.get_string(5)) == "Ctrl+Shift+F12");
        CHECK(store.get_int(99) == 0 && store.get_float(99) == 0.0 && std::string(store.get_string(99)).empty());
        CHECK(!store.current(99) && !store.saved(99));
    });

    run("settings: a change saves, applies restart = false at once and calls the subscribers", [] {
        const auto dir = scratch("change");
        const auto file = dir / "data" / "p" / "settings.toml";
        PluginSettings store;
        CHECK(store.init(schema(), file, "p", environment({})).empty());
        Recorder a, b;
        CHECK(store.subscribe(4, &Recorder::callback, &a) == ANVIL_OK);
        CHECK(store.subscribe(4, &Recorder::callback, &b) == ANVIL_OK);
        CHECK(store.subscribe(1, &Recorder::callback, &a) == ANVIL_ERROR_WRONG_KIND);  // restart = true
        CHECK(store.subscribe(42, &Recorder::callback, &a) == ANVIL_ERROR_NOT_DECLARED);
        CHECK(store.subscribe(4, nullptr, &a) == ANVIL_ERROR_INVALID_ARGUMENT);

        const char* before = store.get_string(4);
        auto c = store.set_text(4, "good day");
        CHECK(c.result == ANVIL_OK && c.applied && c.saved && c.notified == 2 && c.error.empty());
        CHECK(std::string(store.get_string(4)) == "good day" && std::string(before) == "hello");  // the old text stays valid
        CHECK(a.calls == std::vector<AnvilSetting>{4} && b.calls == std::vector<AnvilSetting>{4});
        CHECK(read(file).ends_with("\ngreeting = \"good day\"\n"));

        c = store.set_text(1, "4096");  // restart = true: saved, applies at the next start
        CHECK(c.result == ANVIL_OK && !c.applied && c.saved && c.notified == 0);
        CHECK(store.get_int(1) == 1024 && store.saved(1)->integer == 4096 && store.current(1)->integer == 1024);
        CHECK(read(file).ends_with("\nbudget_mb = 4096\ngreeting = \"good day\"\n"));

        c = store.set_text(1, "1");
        CHECK(c.result == ANVIL_ERROR_INVALID_ARGUMENT && c.error == "budget_mb: 1 is below the minimum 64" && !c.saved);
        CHECK(store.set(3, {7, 0, {}}).result == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(store.set(42, {}).result == ANVIL_ERROR_NOT_DECLARED);
        CHECK(store.set_text(5, "Alt+F4").applied == false && store.get_int(5) == (kKeyCtrl | kKeyShift | 0x7B));

        // Back to the defaults: the file goes away, and the next start reads them.
        CHECK(store.reset(4).notified == 2 && store.reset(1).saved && store.reset(5).saved);
        CHECK(!std::filesystem::exists(file) && std::string(store.get_string(4)) == "hello");
        store.drop_subscriptions();
        CHECK(store.set_text(4, "bye").notified == 0 && a.calls.size() == 2);

        PluginSettings next;
        next.init(schema(), file, "p", environment({}));
        CHECK(std::string(next.get_string(4)) == "bye");
    });

    run("settings: the process environment", [] {
        SetEnvironmentVariableW(L"ANVIL_ENV_TEST_BUDGET_MB", L"512");
        PluginSettings store;
        const auto notes = store.init(schema(), scratch("env") / "none.toml", "env-test");
        SetEnvironmentVariableW(L"ANVIL_ENV_TEST_BUDGET_MB", nullptr);
        CHECK(store.get_int(1) == 512 && notes.size() == 1 &&
              notes[0].message == "setting budget_mb = 512 from ANVIL_ENV_TEST_BUDGET_MB (environment)");
    });

    module_settings_tests();
}
