// The start plan (ADR 0014) on plugin inventories built in memory: what a
// start would load, in which order and with which modules on, what it skips
// and why, and what it writes first. plan_start reads and writes nothing, so
// nothing here touches the disk. Part of anvil_tests.
#include "core/log.hpp"
#include "core/start_plan.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace anvil;
namespace fs = std::filesystem;

namespace {

const fs::path kAnvil = L"C:\\game\\anvil";

std::string requires_plugin(const std::string& id) { return "\n[[dependencies]]\nid = \"" + id + "\"\nversion = \"*\"\n"; }

// A folder of anvil/plugins whose check passed (or failed with `skip`).
InventoryPlugin local(const std::string& id, const std::string& extra = "", i18n::Message skip = {}) {
    InventoryPlugin p;
    p.id = id;
    p.folder = kAnvil / "plugins" / widen(id);
    p.check.manifest = parse_manifest_text(plugin_toml(id, extra), p.folder);
    p.check.skip = std::move(skip);
    if (!p.check.manifest.manifest) std::printf("  manifest error: %s\n", p.check.manifest.error.c_str());
    return p;
}

// A plugin in mod `mod` (descriptor mod/<mod>.mod) with the hash `hash`.
InventoryModPlugin in_mod(const std::string& mod, const std::string& id, const std::string& hash) {
    InventoryModPlugin p;
    p.found.id = id;
    p.found.mod.descriptor = "mod/" + mod + ".mod";
    p.found.mod.name = mod;
    p.found.mod.folder = fs::path(L"C:\\user\\mod") / widen(mod);
    p.found.folder = p.found.mod.folder / "anvil" / widen(id);
    p.found.hash = hash;
    p.check.manifest = parse_manifest_text(plugin_toml(id), p.found.folder);
    p.found.manifest = p.check.manifest.manifest;
    return p;
}

TrustEntry approval(const std::string& mod, const std::string& id, const std::string& hash) {
    TrustEntry e;
    e.id = id;
    e.version = "1.0.0";
    e.mod = "mod/" + mod + ".mod";
    e.mod_name = mod;
    e.hash = hash;
    e.approved = "2026-11-15T10:00:00Z";
    return e;
}

PluginInventory inventory(std::vector<InventoryPlugin> plugins) {
    PluginInventory i;
    i.anvil_dir = kAnvil;
    i.plugins_dir = kAnvil / "plugins";
    i.local = std::move(plugins);
    return i;
}

std::string loaded(const StartPlan& plan) {
    std::string out;
    for (const PlannedPlugin& p : plan.load) out += (out.empty() ? "" : ",") + p.id;
    return out;
}

const PlannedPlugin& entry(const StartPlan& plan, const std::string& id) {
    static const PlannedPlugin none;
    const PlannedPlugin* p = plan.find(id);
    if (p == nullptr) std::printf("  no plan entry for %s\n", id.c_str());
    return p != nullptr ? *p : none;
}

// lode: a plugin with the module flows, whose settings.toml retires
// flows_mode for the module's switch (ADR 0013, point 4).
InventoryPlugin lode() {
    InventoryPlugin p = local("lode", "\n[[modules]]\nid = \"flows\"\nname = \"Flows\"\naffects_gameplay = false\n");
    p.check.settings = parse_settings_text("[retired.flows_mode]\nmodule = \"flows\"\noff = [\"off\"]\n").file;
    p.check.settings_fit = p.check.settings.has_value();
    return p;
}

}  // namespace

void start_plan_tests() {
    run("start plan: the load order, and dependencies that do not load as the player reads them", [] {
        PluginInventory inv = inventory({local("anvil-map", requires_plugin("mei")), local("mei"),
                                         local("tongs", requires_plugin("anvil-map")), local("orphan", requires_plugin("gone")),
                                         local("k-one", "load_after = [\"k-two\"]\n"), local("k-two", "load_after = [\"k-one\"]\n"),
                                         local("plain")});
        PlayerChoices choices;
        choices.anvil_toml.disabled_plugins = {"mei"};
        StartPlan plan = plan_start(inv, choices, {});
        CHECK(plan.enabled && !plan.safe_mode && loaded(plan) == "plain");
        CHECK(entry(plan, "mei").not_loading == NotLoading::Off && entry(plan, "mei").disabled);
        CHECK(entry(plan, "anvil-map").not_loading == NotLoading::NeedsPlugin && entry(plan, "anvil-map").missing == "mei");
        CHECK(entry(plan, "anvil-map").through.empty());
        // The path to the plugin that is off: Tongs needs Anvil Map, which needs Mei.
        CHECK(entry(plan, "tongs").not_loading == NotLoading::NeedsPlugin && entry(plan, "tongs").missing == "mei");
        CHECK(entry(plan, "tongs").through == std::vector<std::string>{"anvil-map"});
        CHECK(entry(plan, "orphan").not_loading == NotLoading::NotInstalled && entry(plan, "orphan").missing == "gone");
        // A cycle is the plugins' own fault, and planning it does not hang.
        CHECK(entry(plan, "k-one").not_loading == NotLoading::CannotLoad && entry(plan, "k-two").not_loading == NotLoading::CannotLoad);
        CHECK(entry(plan, "tongs").local == 2 && plan.of_local(2) == &entry(plan, "tongs"));

        // The nearest plugin that is off is the one named.
        choices.anvil_toml.disabled_plugins = {"mei", "anvil-map"};
        plan = plan_start(inv, choices, {});
        CHECK(entry(plan, "anvil-map").not_loading == NotLoading::Off);
        CHECK(entry(plan, "tongs").missing == "anvil-map" && entry(plan, "tongs").through.empty());

        // Early plugins first, then main, each by id; implied dependencies are in the plan's manifest.
        inv = inventory({local("b"), local("anvil-game"), local("a", "", {})});
        inv.local[1].check.manifest.manifest->phase = Phase::Early;
        inv.local[0].check.manifest.manifest->affects_gameplay = true;
        plan = plan_start(inv, {}, {});
        CHECK(loaded(plan) == "anvil-game,a,b");
        const auto& deps = entry(plan, "b").manifest->dependencies;
        CHECK(std::any_of(deps.begin(), deps.end(), [](const Dependency& d) { return d.id == "anvil-game" && d.implied; }));
    });

    run("start plan: a failed check cannot load, unless the player turned the plugin off", [] {
        const PluginInventory inv = inventory({local("bad", "", i18n::Message("bytes differ")),
                                               local("bad-off", "", i18n::Message("bytes differ")),
                                               local("needs-bad", requires_plugin("bad"))});
        PlayerChoices choices;
        choices.anvil_toml.disabled_plugins = {"bad-off"};
        const StartPlan plan = plan_start(inv, choices, {});
        CHECK(loaded(plan).empty());
        CHECK(entry(plan, "bad").not_loading == NotLoading::CannotLoad && entry(plan, "bad").reason == "bytes differ");
        CHECK(entry(plan, "bad-off").not_loading == NotLoading::Off && entry(plan, "bad-off").reason == "disabled in anvil.toml");
        CHECK(entry(plan, "needs-bad").not_loading == NotLoading::NeedsPlugin && entry(plan, "needs-bad").missing == "bad");

        // A plugin.toml that does not parse, and a missing entry DLL: skipped as discovered.
        PluginInventory broken = inventory({local("ok")});
        InventoryPlugin unparsed;
        unparsed.id = "unparsed";
        unparsed.folder = kAnvil / "plugins" / "unparsed";
        unparsed.check.manifest.error = "bogus";
        unparsed.check.skip = i18n::Message("bogus");
        broken.local.push_back(unparsed);
        broken.local.push_back(local("no-entry"));
        broken.local.back().check.entry_missing = i18n::Message("entry no-entry.dll not found");
        const StartPlan b = plan_start(broken, {}, {});
        CHECK(loaded(b) == "ok" && entry(b, "unparsed").not_loading == NotLoading::CannotLoad && !entry(b, "unparsed").manifest);
        CHECK(entry(b, "no-entry").not_loading == NotLoading::CannotLoad && entry(b, "no-entry").reason == "entry no-entry.dll not found");
    });

    run("start plan: off with enabled = false or ANVIL_DISABLE=1 marks every plugin off and writes nothing", [] {
        PluginInventory inv = inventory({local("a"), lode()});
        inv.copy_ids = {kAnvil / "data" / "anvil" / "approved" / "revoked"};
        PlayerChoices choices;
        choices.settings_files["lode"] = "flows_mode = \"off\"\n";
        for (int how = 0; how < 2; ++how) {
            PlayerChoices c = choices;
            StartConditions conditions;
            if (how == 0) c.anvil_toml.enabled = false;
            if (how == 1) conditions.anvil_disabled = true;
            const StartPlan plan = plan_start(inv, c, conditions);
            CHECK(!plan.enabled && plan.load.empty() && plan.skipped.size() == 2 && plan.writes.empty());
            CHECK(std::all_of(plan.skipped.begin(), plan.skipped.end(), [](const PlannedPlugin& p) {
                return p.not_loading == NotLoading::Off && p.disabled && p.why.code == "all_disabled";
            }));
        }
        // On, the same folder writes: the revoked copy goes, the retired setting moves.
        const StartPlan on = plan_start(inv, choices, {});
        CHECK(on.writes.remove.size() == 1 && on.writes.retired.size() == 1);
    });

    run("start plan: safe mode loads only the standard plugins, and leaves out one a crash was in", [] {
        const PluginInventory inv = inventory({local("anvil-game"), local("anvil-overlay"), local("bellows")});
        StartConditions conditions;
        conditions.safe_mode = true;
        StartPlan plan = plan_start(inv, {}, conditions);
        CHECK(plan.safe_mode && loaded(plan) == "anvil-game,anvil-overlay");
        CHECK(entry(plan, "bellows").kind == SkipKind::safe_mode && entry(plan, "bellows").not_loading == NotLoading::CannotLoad);
        CrashSummary crash;
        crash.name = "2026-11-15_10-00-00";
        crash.level = "in_plugin_code";
        crash.plugins = {"anvil-overlay"};
        conditions.crashes = {crash};
        plan = plan_start(inv, {}, conditions);
        CHECK(loaded(plan) == "anvil-game" && entry(plan, "anvil-overlay").kind == SkipKind::safe_mode);
    });

    run("start plan: the benchmark arm without:<id> leaves the compared plugin out", [] {
        const PluginInventory inv = inventory({local("anvil-game"), local("bellows"), local("needs-bellows", requires_plugin("bellows"))});
        StartConditions without;
        without.benchmark = BenchmarkArm{false, "bellows"};
        StartPlan plan = plan_start(inv, {}, without);
        CHECK(loaded(plan) == "anvil-game" && entry(plan, "bellows").benchmark_arm && entry(plan, "bellows").disabled);
        CHECK(entry(plan, "needs-bellows").not_loading == NotLoading::NeedsPlugin);
        StartConditions with;
        with.benchmark = BenchmarkArm{true, "bellows"};
        CHECK(loaded(plan_start(inv, {}, with)) == "anvil-game,bellows,needs-bellows");
    });

    run("start plan: plugins from mods wait for approval, load from their approved copy, and the copy is planned", [] {
        PluginInventory inv = inventory({local("a-local")});
        inv.mods = {in_mod("One", "m-ok", "h1"), in_mod("One", "m-new", "h2"), in_mod("One", "a-local", "h3")};
        inv.copy_ids = {kAnvil / "data" / "anvil" / "approved" / "m-ok", kAnvil / "data" / "anvil" / "approved" / "gone"};
        InventoryCopy old;
        old.id = "m-ok";
        old.name = "h0";
        old.folder = approved_copy(kAnvil, "m-ok", "h0");
        inv.copies = {old};
        PlayerChoices choices;
        choices.trust.entries = {approval("One", "m-ok", "h1")};
        StartPlan plan = plan_start(inv, choices, {});
        CHECK(loaded(plan) == "a-local,m-ok");
        CHECK(plan.mods.size() == 3 && plan.mods[0].state == ModPluginState::Approved && plan.mods[0].copy_missing);
        CHECK(plan.mods[2].state == ModPluginState::Shadowed && plan.of_mod(2) == nullptr);
        const PlannedPlugin& ok = entry(plan, "m-ok");
        CHECK(ok.mod == 0 && ok.mod_name == "One" && ok.makes_copy && ok.approved_hash == "h1");
        CHECK(ok.folder == approved_copy(kAnvil, "m-ok", "h1") && ok.manifest->folder == ok.folder);
        const PlannedPlugin& waits = entry(plan, "m-new");
        CHECK(waits.not_loading == NotLoading::WaitsForApproval && waits.awaiting_approval && waits.why.code == "from_mod");
        CHECK(plan.writes.copies.size() == 1 && plan.writes.copies[0].to == ok.folder && plan.writes.copies[0].from == inv.mods[0].found.folder);
        // The copy of an older hash and the copies of an id trust.toml no longer names go first.
        CHECK(plan.writes.remove.size() == 2 && plan.writes.remove[0] == old.folder && plan.writes.remove[1] == inv.copy_ids[1]);

        // The copy exists: nothing to make. Approved now in the overlay: the next plan loads it.
        InventoryCopy made;
        made.id = "m-ok";
        made.name = "h1";
        made.folder = ok.folder;
        inv.copies.push_back(made);
        choices.trust.entries.push_back(approval("One", "m-new", "h2"));
        plan = plan_start(inv, choices, {});
        CHECK(plan.writes.copies.size() == 1 && plan.writes.copies[0].id == "m-new" && !entry(plan, "m-ok").makes_copy);
        CHECK(loaded(plan) == "a-local,m-new,m-ok");

        // In safe mode a mod's plugin does not load; a trust.toml that does not parse approves nothing and prunes nothing.
        StartConditions safe;
        safe.safe_mode = true;
        CHECK(entry(plan_start(inv, choices, safe), "m-ok").kind == SkipKind::safe_mode);
        choices.trust = TrustFile{};  // read_trust approves nothing from a file that does not parse
        choices.trust.error = "line 1: broken";
        plan = plan_start(inv, choices, {});
        CHECK(loaded(plan) == "a-local" && plan.writes.remove.empty() && plan.writes.copies.empty() && plan.warnings.size() == 1);
    });

    run("start plan: a retired setting turns its module off in the plan before the start that moves it", [] {
        const PluginInventory inv = inventory({lode()});
        PlayerChoices choices;
        choices.settings_files["lode"] = "flows_mode = \"off\"\n";
        StartPlan plan = plan_start(inv, choices, {});
        // What the Manager and anvil check --game show before that start, and what the start does: the same.
        CHECK(plan.lists.disabled_modules == std::vector<std::string>{"lode.flows"});
        CHECK(choices.anvil_toml.disabled_modules.empty());  // the saved file is as it was
        CHECK(entry(plan, "lode").modules_on == std::vector<bool>{false});
        CHECK(plan.writes.retired.size() == 1 && plan.writes.retired[0].plugin == "lode" &&
              plan.writes.retired[0].player_file == kAnvil / "data" / "lode" / "settings.toml");
        // A value not in `off` keeps the module on; the value still leaves the player's file.
        choices.settings_files["lode"] = "flows_mode = \"on\"\n";
        plan = plan_start(inv, choices, {});
        CHECK(entry(plan, "lode").modules_on == std::vector<bool>{true} && plan.writes.retired.size() == 1);
        // A benchmark start: the module is off for that start only, and nothing is written (benchmark-v1.md §2).
        choices.settings_files["lode"] = "flows_mode = \"off\"\n";
        StartConditions bench;
        bench.benchmark = BenchmarkArm{true, "lode"};
        plan = plan_start(inv, choices, bench);
        CHECK(entry(plan, "lode").modules_on == std::vector<bool>{false} && plan.writes.empty());
        CHECK(plan.notes.size() == 2 && plan.notes[0].second.message.find("at this benchmark start") != std::string::npos &&
              plan.notes[1].second.message.find("stay for the next start") != std::string::npos);
        // A second folder with the same id moves nothing: its data/<id> is the first one's.
        InventoryPlugin winner = local("lode", "\n[[modules]]\nid = \"flows\"\nname = \"Flows\"\naffects_gameplay = false\n");
        InventoryPlugin second = lode();
        second.folder = kAnvil / "plugins" / "lode-copy";
        if (second.check.manifest.manifest) second.check.manifest.manifest->folder = second.folder;
        plan = plan_start(inventory({winner, second}), choices, {});
        CHECK(plan.writes.empty() && entry(plan, "lode").modules_on == std::vector<bool>{true});
        CHECK(plan.skipped.size() == 1 && plan.skipped[0].why.code == "duplicate_id");
        // The first folder of the id fails another check and a later one loads: the first one's
        // retired rules leave the loaded one's data/<id> alone too.
        InventoryPlugin failing = lode();
        failing.check.skip = i18n::Message("needs a newer Anvil");
        winner.folder = kAnvil / "plugins" / "lode-new";
        if (winner.check.manifest.manifest) winner.check.manifest.manifest->folder = winner.folder;
        plan = plan_start(inventory({failing, winner}), choices, {});
        CHECK(loaded(plan) == "lode" && entry(plan, "lode").folder == winner.folder);
        CHECK(plan.writes.empty() && entry(plan, "lode").modules_on == std::vector<bool>{true});
        // Neither folder loads: the first one moves, once.
        failing.check.skip = i18n::Message("needs a newer Anvil");
        InventoryPlugin failing_too = lode();
        failing_too.folder = kAnvil / "plugins" / "lode-old";
        failing_too.check.skip = i18n::Message("needs a newer Anvil");
        plan = plan_start(inventory({failing, failing_too}), choices, {});
        CHECK(plan.load.empty() && plan.writes.retired.size() == 1);
        // No player's file: nothing to move.
        choices.settings_files["lode"] = std::nullopt;
        plan = plan_start(inv, choices, {});
        CHECK(plan.writes.empty() && entry(plan, "lode").modules_on == std::vector<bool>{true});
        // A player's file that does not parse: a note, nothing moves.
        choices.settings_files["lode"] = "flows_mode = \n";
        plan = plan_start(inv, choices, {});
        CHECK(plan.writes.empty() && plan.notes.size() == 1 && plan.notes[0].first == "lode");
    });
}
