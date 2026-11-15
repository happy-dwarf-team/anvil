// The Plugins tab's view model (src/ui/plugin_model.hpp; ADR 0013, point 9;
// docs/ui-guidelines.md §5), on made-up snapshots.
#include "ui/plugin_model.hpp"
#include "ui/line_breaks.hpp"

#include "test_support.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace anvil::ui;

namespace {

PluginSnap plugin(const std::string& id, std::vector<std::string> requires_ids = {}) {
    PluginSnap p;
    p.id = id;
    p.name = id + " name";
    p.version = "1.0.0";
    p.requires_ids = std::move(requires_ids);
    return p;
}

ModuleSnap module(const std::string& id, bool on = true, const std::string& group = "") {
    ModuleSnap m;
    m.id = id;
    m.name = id + " module";
    m.group = group;
    m.default_on = true;
    m.saved.on = on;
    m.in_effect.on = on;
    return m;
}

SettingSnap setting(const std::string& name, const std::string& value, const std::string& def,
                    const std::string& module = "") {
    SettingSnap s;
    s.name = name;
    s.label = name + " label";
    s.description = "About " + name;
    s.module = module;
    s.default_value = def;
    s.saved = value;
    s.in_effect = value;
    return s;
}

// Mei <- Anvil Map <- Tongs: Mei is off, so Anvil Map and Tongs do not load.
Snapshot chain_snapshot() {
    Snapshot s;
    PluginSnap mei = plugin("mei");
    mei.name = "Mei";
    mei.saved.on = false;
    mei.in_effect.on = false;
    PluginSnap map = plugin("anvil-map", {"mei"});
    map.name = "Anvil Map";
    PluginSnap tongs = plugin("tongs", {"anvil-map"});
    tongs.name = "Tongs";
    s.plugins = {mei, map, tongs};
    return s;
}

void load_tests() {
    run("a plugin with its dependencies loads", [] {
        Snapshot s;
        s.plugins = {plugin("a"), plugin("b", {"a"})};
        CHECK(load_state(s, s.plugins[1]).loads);
        CHECK(view(s, s.plugins[1]).glyph == Glyph::On);
        CHECK(view(s, s.plugins[1]).line.kind == EntryLine::Kind::Summary);
    });
    run("a dependency that is off names the path to it", [] {
        const Snapshot s = chain_snapshot();
        const LoadState l = load_state(s, s.plugins[2]);
        CHECK(!l.loads);
        CHECK(l.why == LoadState::Why::NeedsPlugin);
        CHECK(l.missing == "mei");
        CHECK(l.through == std::vector<std::string>{"anvil-map"});
        const PluginView v = view(s, s.plugins[2]);
        CHECK(v.glyph == Glyph::Attention);
        CHECK(v.line.kind == EntryLine::Kind::NeedsPlugin);
        CHECK(v.line.name == "Mei");
        CHECK(v.needs_attention);
    });
    run("the nearest plugin that is off is the one named", [] {
        Snapshot s = chain_snapshot();
        s.plugins[1].saved.on = false;  // Anvil Map off too
        const LoadState l = load_state(s, s.plugins[2]);
        CHECK(l.missing == "anvil-map");
        CHECK(l.through.empty());
    });
    run("a plugin that is off says so, not its dependency", [] {
        Snapshot s = chain_snapshot();
        s.plugins[1].saved.on = false;
        const PluginView v = view(s, s.plugins[1]);
        CHECK(v.load.why == LoadState::Why::Off);
        CHECK(v.glyph == Glyph::Off);
        CHECK(!v.needs_attention);
    });
    run("a required plugin that is not installed", [] {
        Snapshot s;
        s.plugins = {plugin("b", {"gone"})};
        const LoadState l = load_state(s, s.plugins[0]);
        CHECK(l.why == LoadState::Why::NotInstalled);
        CHECK(l.missing == "gone");
        CHECK(view(s, s.plugins[0]).line.kind == EntryLine::Kind::NotInstalled);
    });
    run("a cycle does not hang", [] {
        Snapshot s;
        s.plugins = {plugin("a", {"b"}), plugin("b", {"a"})};
        CHECK(load_state(s, s.plugins[0]).loads);
    });
    run("own reasons come before the switch", [] {
        Snapshot s;
        PluginSnap bad = plugin("bad");
        bad.blocked = Blocked::CannotLoad;
        bad.saved.on = false;
        PluginSnap mod = plugin("mod");
        mod.origin = Origin::FromMod;
        mod.blocked = Blocked::WaitsForApproval;
        s.plugins = {bad, mod};
        CHECK(view(s, s.plugins[0]).glyph == Glyph::CannotLoad);
        CHECK(view(s, s.plugins[0]).line.kind == EntryLine::Kind::CannotLoad);
        CHECK(view(s, s.plugins[1]).glyph == Glyph::Attention);
        CHECK(view(s, s.plugins[1]).line.kind == EntryLine::Kind::WaitsForApproval);
        s.plugins[1].approved = true;  // approved for the next start
        CHECK(load_state(s, s.plugins[1]).loads);
        CHECK(pending_count(s.plugins[1]) == 1);
    });
    run("an update waiting for approval keeps the approved copy running", [] {
        Snapshot s;
        PluginSnap mod = plugin("mod");
        mod.origin = Origin::FromMod;
        mod.approved = mod.approved_in_effect = true;
        mod.update_waits = "1.1.0";
        s.plugins = {mod};
        const PluginView v = view(s, s.plugins[0]);
        CHECK(v.load.loads);
        CHECK(v.glyph == Glyph::Attention);
        CHECK(v.line.kind == EntryLine::Kind::UpdateWaits);
        CHECK(v.line.name == "1.1.0");
        CHECK(to_approve(s) == 1);
    });
    run("needed by lists the plugins that are on and require it", [] {
        Snapshot s = chain_snapshot();
        CHECK(view(s, s.plugins[1]).needed_by == std::vector<std::string>{"tongs"});
        CHECK(view(s, s.plugins[0]).needed_by == std::vector<std::string>{"anvil-map"});
        s.plugins[1].saved.on = false;
        CHECK(view(s, s.plugins[0]).needed_by.empty());
    });
}

void game_tests() {
    run("kept out of an ironman game", [] {
        Snapshot s;
        s.game = Game::Ironman;
        PluginSnap p = plugin("lodestone");
        p.changes_game = true;
        s.plugins = {p};
        const PluginView v = view(s, s.plugins[0]);
        CHECK(!v.in_this_game);
        CHECK(v.line.kind == EntryLine::Kind::KeptOutOfIronman);
        CHECK(v.glyph == Glyph::Off);
        s.plugins[0].in_effect.allow_ironman = true;
        CHECK(view(s, s.plugins[0]).in_this_game);
    });
    run("a plugin with modules leaves the opt-ins to them", [] {
        Snapshot s;
        s.game = Game::Multiplayer;
        PluginSnap p = plugin("smithy");
        p.changes_game = true;
        p.modules = {module("drill")};
        s.plugins = {p};
        CHECK(view(s, s.plugins[0]).in_this_game);
    });
    run("a module in this game: runs, off, kept out, off with its plugin", [] {
        Snapshot s;
        s.game = Game::Ironman;
        PluginSnap p = plugin("smithy");
        p.changes_game = true;
        ModuleSnap drill = module("drill");
        drill.changes_game = true;
        ModuleSnap keys = module("keys");
        ModuleSnap merge = module("merge", false);
        p.modules = {drill, keys, merge};
        s.plugins = {p};
        const PluginSnap& sp = s.plugins[0];
        CHECK(module_in_game(s, sp, sp.modules[0]) == InGame::KeptOutOfIronman);
        CHECK(module_in_game(s, sp, sp.modules[1]) == InGame::Runs);
        CHECK(module_in_game(s, sp, sp.modules[2]) == InGame::Off);
        s.plugins[0].modules[0].in_effect.allow_ironman = true;
        CHECK(module_in_game(s, s.plugins[0], s.plugins[0].modules[0]) == InGame::Runs);
        // Saved off, still on in this game: the switch waits for the next start.
        s.plugins[0].saved.on = false;
        CHECK(module_in_game(s, s.plugins[0], s.plugins[0].modules[1]) == InGame::Runs);
        s.plugins[0].in_effect.on = false;
        CHECK(module_in_game(s, s.plugins[0], s.plugins[0].modules[1]) == InGame::OffWithPlugin);
    });
}

void pending_tests() {
    run("pending counts where the change was made", [] {
        PluginSnap p = plugin("p");
        p.modules = {module("a"), module("b")};
        p.settings = {setting("x", "1", "1")};
        CHECK(pending_count(p) == 0);
        p.saved.on = false;  // the plugin off: one change, not one per module
        CHECK(pending_count(p) == 1);
        p.modules[0].saved.allow_ironman = true;
        p.settings[0].saved = "2";
        CHECK(pending_count(p) == 3);
        CHECK(setting_pending(p.settings[0]));
        CHECK(setting_changed(p.settings[0]));
    });
    run("undo restores what is in effect, not approvals", [] {
        PluginSnap p = plugin("p");
        p.origin = Origin::FromMod;
        p.approved = true;
        p.modules = {module("a")};
        p.modules[0].saved.on = false;
        p.settings = {setting("x", "1", "1")};
        p.settings[0].saved = "5";
        const std::vector<Edit> edits = undo_pending(p);
        CHECK(edits.size() == 2);
        CHECK(edits[0].kind == Edit::Kind::Switch && edits[0].key() == "p.a" && edits[0].which == Which::On && edits[0].on);
        CHECK(edits[1].kind == Edit::Kind::Setting && edits[1].value == "1" && edits[1].old_value == "5");
    });
    run("undo has nothing to take back when only an approval or a locked setting waits", [] {
        PluginSnap p = plugin("p");
        p.origin = Origin::FromMod;
        p.approved = true;  // approved for the next start
        CHECK(pending_count(p) == 1 && undo_pending(p).empty());
        p.settings = {setting("x", "1", "1")};
        p.settings[0].saved = "5";
        p.settings[0].locked = true;  // in a multiplayer game
        CHECK(pending_count(p) == 2 && undo_pending(p).empty());
        p.settings[0].locked = false;
        CHECK(undo_pending(p).size() == 1);
    });
}

void entry_tests() {
    run("entries of one id: told apart in the list, counted once", [] {
        Snapshot s;
        PluginSnap local = plugin("lode");
        local.settings = {setting("x", "1", "1")};
        local.settings[0].saved = "5";  // waits for the next start, in data/lode/settings.toml
        PluginSnap shadowed = local;    // the copy in a mod: the same player's file
        shadowed.origin = Origin::FromMod;
        PluginSnap other = plugin("tongs");
        s.plugins = {local, other, shadowed};
        CHECK(entry_key(s, 0) == "lode" && entry_key(s, 1) == "tongs" && entry_key(s, 2) == "lode|2");
        CHECK(entry_key(s, 3).empty());
        CHECK(index_of_entry(s, "lode") == 0 && index_of_entry(s, "lode|2") == 2 && index_of_entry(s, "tongs") == 1);
        CHECK(index_of_entry(s, "lode|3") == SIZE_MAX && index_of_entry(s, "tongs|2") == SIZE_MAX);
        CHECK(first_of_id(s, 0) && first_of_id(s, 1) && !first_of_id(s, 2));
        CHECK(pending_count(s) == 1 && undo_pending(s).size() == 1);
    });
}

void where_tests() {
    run("where it runs, both ways", [] {
        CHECK(where(Switches{false, true, true}) == Where::Off);
        CHECK(where(Switches{true, false, false}) == Where::Normal);
        CHECK(where(Switches{true, true, false}) == Where::PlusIronman);
        CHECK(where(Switches{true, false, true}) == Where::PlusMultiplayer);
        CHECK(where(Switches{true, true, true}) == Where::Everywhere);
        for (const Where w : {Where::Normal, Where::PlusIronman, Where::PlusMultiplayer, Where::Everywhere}) {
            CHECK(where(with_where(Switches{}, w)) == w);
        }
    });
    run("off keeps the opt-ins", [] {
        const Switches s = with_where(Switches{true, true, false}, Where::Off);
        CHECK(!s.on && s.allow_ironman && !s.allow_multiplayer);
    });
    run("a choice the author does not offer", [] {
        const Offers no_ironman{false, true};
        CHECK(where_offered(no_ironman, Where::Normal));
        CHECK(where_offered(no_ironman, Where::PlusMultiplayer));
        CHECK(!where_offered(no_ironman, Where::PlusIronman));
        CHECK(!where_offered(no_ironman, Where::Everywhere));
    });
    run("set_where writes only what changes", [] {
        PluginSnap p = plugin("p");
        p.modules = {module("m")};
        const std::vector<Edit> edits = set_where(p, "m", Where::Everywhere);
        CHECK(edits.size() == 2);
        CHECK(edits[0].which == Which::Ironman && edits[0].on && edits[0].key() == "p.m");
        CHECK(edits[1].which == Which::Multiplayer && edits[1].on);
        CHECK(set_where(p, "m", Where::Normal).empty());
        CHECK(set_where(p, "nope", Where::Off).empty());
        CHECK(set_switch(p, "", Which::On, false).size() == 1);
        CHECK(set_switch(p, "", Which::On, true).empty());
    });
    run("set_setting carries the old value", [] {
        PluginSnap p = plugin("p");
        p.settings = {setting("x", "1", "1")};
        const std::vector<Edit> edits = set_setting(p, "x", "3");
        CHECK(edits.size() == 1 && edits[0].value == "3" && edits[0].old_value == "1");
        CHECK(set_setting(p, "x", "1").empty());
    });
}

void chain_tests() {
    run("turning on names the chain, dependencies first", [] {
        const Snapshot s = chain_snapshot();
        Snapshot off = s;
        off.plugins[1].saved.on = false;
        off.plugins[2].saved.on = false;
        const Chain c = turn_on_chain(off, "tongs");
        CHECK(c.steps.size() == 3);
        CHECK(c.steps[0].id == "mei" && c.steps[0].needed_by == "anvil-map");
        CHECK(c.steps[1].id == "anvil-map" && c.steps[1].needed_by == "tongs");
        CHECK(c.steps[2].id == "tongs" && c.steps[2].needed_by.empty());
        CHECK(c.missing.empty());
        const std::vector<Edit> edits = edits_of(off, c);
        CHECK(edits.size() == 3 && edits[0].key() == "mei" && edits[0].on);
    });
    run("a plugin that is on is not in the chain; a missing one is named", [] {
        Snapshot s;
        PluginSnap a = plugin("a", {"gone", "b"});
        a.saved.on = false;
        s.plugins = {a, plugin("b")};
        const Chain c = turn_on_chain(s, "a");
        CHECK(c.steps.size() == 1 && c.steps[0].id == "a");
        CHECK(c.missing == std::vector<std::string>{"gone"});
    });
}

void reset_tests() {
    run("reset of one setting, a module's and a plugin's", [] {
        PluginSnap p = plugin("p");
        p.modules = {module("m"), module("n", false)};
        p.modules[0].saved.allow_multiplayer = true;
        p.saved.on = false;
        p.saved.allow_ironman = true;
        p.settings = {setting("x", "2", "1"), setting("y", "a", "b", "m"), setting("z", "1", "1", "m")};
        CHECK(reset_setting(p, "x").size() == 1);
        CHECK(reset_setting(p, "z").empty());
        const std::vector<Edit> module_reset = reset_module_settings(p, "m");
        CHECK(module_reset.size() == 1 && module_reset[0].setting == "y" && module_reset[0].value == "b");
        const std::vector<Edit> all = reset_plugin(p);
        // x, y; m's multiplayer; n back on; the plugin's ironman. Never its own switch.
        CHECK(all.size() == 5);
        bool own_switch = false;
        for (const Edit& e : all) {
            own_switch = own_switch || (e.kind == Edit::Kind::Switch && e.key() == "p" && e.which == Which::On);
        }
        CHECK(!own_switch);
    });
    run("resets leave settings locked in a multiplayer game alone", [] {
        PluginSnap p = plugin("p");
        p.modules = {module("m")};
        p.settings = {setting("x", "2", "1"), setting("y", "a", "b", "m"), setting("z", "3", "1", "m")};
        p.settings[0].locked = true;
        p.settings[1].locked = true;
        const std::vector<Edit> module_reset = reset_module_settings(p, "m");
        CHECK(module_reset.size() == 1 && module_reset[0].setting == "z");
        // The module's row still counts the locked one, as its setting rows mark it changed.
        CHECK(changed_module_settings(p, "m") == 2 && changed_module_settings(p, "n") == 0);
        const std::vector<Edit> all = reset_plugin(p);
        CHECK(all.size() == 1 && all[0].setting == "z");
    });
}

void find_tests() {
    Snapshot s;
    PluginSnap p = plugin("bellows");
    p.name = "Bellows";
    p.modules = {module("pacing")};
    p.modules[0].name = "Frame pacing at speed 5";
    p.settings = {setting("cache", "1", "1")};
    p.settings[0].description = "Keeps the Trade cache warm";
    p.changes_game = true;
    s.plugins = {p, plugin("other")};
    run("search by name, module and setting, ignoring case", [&] {
        CHECK(find(s.plugins[0], "BELL").where == Match::Where::Name);
        const Match m = find(s.plugins[0], "speed 5");
        CHECK(m.where == Match::Where::Module && m.what == "Frame pacing at speed 5");
        CHECK(m.modules == std::vector<std::string>{"pacing"});
        const Match t = find(s.plugins[0], "trade");
        CHECK(t.where == Match::Where::Setting && t.settings == std::vector<std::string>{"cache"});
        CHECK(find(s.plugins[0], "zzz").where == Match::Where::None);
        CHECK(find(s.plugins[0], "").where == Match::Where::Name);
    });
    run("chips combine with each other and the search", [&] {
        CHECK(passes(s, s.plugins[0], "", ChipChangesGame));
        CHECK(!passes(s, s.plugins[1], "", ChipChangesGame));
        CHECK(!passes(s, s.plugins[0], "", ChipChangesGame | ChipChanged));
        CHECK(!passes(s, s.plugins[0], "zzz", ChipNone));
        CHECK(chip_count(s, ChipChangesGame) == 1);
        CHECK(chip_count(s, ChipOff) == 0);
    });
}

void layout_tests() {
    run("a yes/no label leaves its first line to the notes while it keeps its room", [] {
        CHECK(label_reserve(400.0f, 150.0f, 200.0f) == 150.0f);  // the notes stay beside the label
        CHECK(label_reserve(400.0f, 200.0f, 200.0f) == 200.0f);  // exactly the room the label needs
        CHECK(label_reserve(400.0f, 250.0f, 200.0f) == 0.0f);    // too narrow: the notes wrap under it
        CHECK(label_reserve(400.0f, 0.0f, 200.0f) == 0.0f);      // nothing follows the label
    });
    run("sections count what needs a look", [] {
        Snapshot s;
        PluginSnap mod = plugin("m1");
        mod.origin = Origin::FromMod;
        mod.blocked = Blocked::WaitsForApproval;
        PluginSnap bad = plugin("m2");
        bad.origin = Origin::FromMod;
        bad.blocked = Blocked::CannotLoad;
        PluginSnap ok = plugin("m3");
        ok.origin = Origin::FromMod;
        s.plugins = {plugin("own"), mod, bad, ok};
        const SectionCounts c = section_counts(s, Origin::FromMod);
        CHECK(c.plugins == 3 && c.attention == 1 && c.cannot_load == 1 && c.waits_for_approval == 1);
        CHECK(section_counts(s, Origin::Own).plugins == 1);
    });
    run("settings under their module, advanced apart, groups once", [] {
        PluginSnap p = plugin("p");
        p.settings = {setting("a", "", ""), setting("b", "", "", "m"), setting("c", "", ""), setting("d", "", "")};
        p.settings[0].group = "G";
        p.settings[2].advanced = true;
        const SettingsLayout plugin_level = settings_layout(p, "");
        CHECK(plugin_level.shown == (std::vector<std::size_t>{3, 0}));  // no group first
        CHECK(plugin_level.advanced == std::vector<std::size_t>{2});
        CHECK(settings_layout(p, "m").shown == std::vector<std::size_t>{1});
    });
    run("module groups in the order of their first module, ungrouped first", [] {
        PluginSnap p = plugin("p");
        p.modules = {module("a", true, "Trade"), module("b", false), module("c", false, "Trade"), module("d", true, "War")};
        const std::vector<ModuleGroup> g = module_groups(p);
        CHECK(g.size() == 3);
        CHECK(g[0].name.empty() && g[0].modules == std::vector<std::size_t>{1} && g[0].on == 0);
        CHECK(g[1].name == "Trade" && g[1].modules == (std::vector<std::size_t>{0, 2}) && g[1].on == 1);
        CHECK(g[2].name == "War");
    });
}

// CJK line breaks (src/ui/line_breaks.hpp), every character one unit wide.
std::vector<std::string> wrap(std::string_view text, float width) {
    const std::vector<size_t> starts = wrap_starts(text, width, [](const char*, const char*) { return 1.0f; });
    std::vector<std::string> lines;
    for (size_t k = 0; k < starts.size(); ++k) {
        size_t end = k + 1 < starts.size() ? starts[k + 1] : text.size();
        while (end > starts[k] && (text[end - 1] == ' ' || text[end - 1] == '\n')) --end;
        lines.emplace_back(text.substr(starts[k], end - starts[k]));
    }
    return lines;
}

void line_break_tests() {
    run("Japanese breaks between any two characters, not only at its punctuation", [] {
        CHECK(wrap("ゲームの起動を速くします", 5) == (std::vector<std::string>{"ゲームの起", "動を速くし", "ます"}));
    });
    run("kinsoku: no line starts with 。 or ー, none ends with 「", [] {
        // 。 would start the second line: the break moves one character back.
        CHECK(wrap("あいうえ。かき", 4) == (std::vector<std::string>{"あいう", "え。かき"}));
        CHECK(wrap("あいうー", 3) == (std::vector<std::string>{"あい", "うー"}));
        CHECK(wrap("あいう「え」", 4) == (std::vector<std::string>{"あいう", "「え」"}));
    });
    run("Latin words keep breaking at spaces, also next to CJK text", [] {
        CHECK(wrap("one two three", 7) == (std::vector<std::string>{"one two", "three"}));
        CHECK(wrap("Steam ワークショップ", 8) == (std::vector<std::string>{"Steam ワー", "クショップ"}));
        CHECK(wrap("anvil/plugins に入ります", 13) == (std::vector<std::string>{"anvil/plugins", "に入ります"}));
    });
    run("Korean breaks at spaces only", [] {
        CHECK(wrap("새 릴리스 확인", 5) == (std::vector<std::string>{"새 릴리스", "확인"}));
        CHECK(!has_cjk_breaks("새 릴리스 확인") && has_cjk_breaks("ゲーム") && !has_cjk_breaks("plain text"));
    });
    run("a path breaks after a separator, not inside a folder's name", [] {
        CHECK(wrap("C:\\Games\\fixture\\game", 12) == (std::vector<std::string>{"C:\\Games\\", "fixture\\game"}));
        CHECK(needs_own_breaks("C:\\Games") && !needs_own_breaks("anvil/plugins"));
    });
    run("a word longer than the line breaks where it overflows; newlines stay", [] {
        CHECK(wrap("abcdefgh", 3) == (std::vector<std::string>{"abc", "def", "gh"}));
        CHECK(wrap("あい\nうえ", 9) == (std::vector<std::string>{"あい", "うえ"}));
    });
}

}  // namespace

int main() {
    load_tests();
    game_tests();
    pending_tests();
    entry_tests();
    where_tests();
    chain_tests();
    reset_tests();
    find_tests();
    layout_tests();
    line_break_tests();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
