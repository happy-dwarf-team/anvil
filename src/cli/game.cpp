#include "cli/game.hpp"

#include "core/discovery.hpp"
#include "core/mod_plugins.hpp"
#include "core/game_build.hpp"
#include "core/pe_image.hpp"
#include "core/platform.hpp"
#include "core/resolver.hpp"
#include "core/start_plan.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

namespace anvil::cli {
namespace {

std::string shown(const std::filesystem::path& path) {
    const auto u8 = path.lexically_normal().generic_u8string();
    return std::string(u8.begin(), u8.end());
}

std::string name_of(const std::filesystem::path& path) {
    const auto u8 = path.filename().u8string();
    return std::string(u8.begin(), u8.end());
}

std::string hex(uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(value));
    return text;
}

const char* known_build(std::string_view sha256) {
    const KnownBuild* build = find_known_build_by_sha256(sha256);
    return build != nullptr ? build->id : nullptr;
}

bool ranges_overlap(const CodeRange& a, const CodeRange& b) {
    return a.rva < b.rva + b.size && b.rva < a.rva + a.size;
}

// Plugins that load together and declare overlapping code ranges (design §16).
// Ranges that start at the same address are the normal case of a chain, not a
// warning: their lengths only say how many bytes each plugin verifies. One
// warning per pair of plugins, at the earlier plugin's symbol.
std::vector<OverlapWarning> find_overlaps(const std::vector<const CheckReport*>& loaded) {
    std::vector<OverlapWarning> warnings;
    for (size_t i = 0; i < loaded.size(); ++i) {
        for (size_t j = i + 1; j < loaded.size(); ++j) {
            const CheckReport& a = *loaded[i];
            const CheckReport& b = *loaded[j];
            const CodeRange* first_a = nullptr;
            const CodeRange* first_b = nullptr;
            size_t hits = 0;
            for (const CodeRange& x : a.code_ranges) {
                for (const CodeRange& y : b.code_ranges) {
                    if (!ranges_overlap(x, y) || x.rva == y.rva) continue;
                    if (hits++ == 0) {
                        first_a = &x;
                        first_b = &y;
                    }
                }
            }
            if (hits == 0) continue;
            OverlapWarning w;
            w.plugin = a.plugin;
            w.other = b.plugin;
            w.file = shown(a.manifest->folder / std::filesystem::path(std::u8string(a.manifest->symbols.begin(), a.manifest->symbols.end())));
            w.line = first_a->line;
            w.message = "code range of symbol " + first_a->symbol + " (rva " + hex(first_a->rva) + ", " +
                        std::to_string(first_a->size) + " byte(s)) overlaps symbol " + first_b->symbol + " of plugin " +
                        b.plugin + " (rva " + hex(first_b->rva) + ", " + std::to_string(first_b->size) +
                        " byte(s)); if both change those bytes, the second change is refused at load";
            if (hits > 1) w.message += " (and " + std::to_string(hits - 1) + " more overlapping range(s) with " + b.plugin + ")";
            warnings.push_back(std::move(w));
        }
    }
    return warnings;
}

// The Anvil version the loader installed in the folder checks the plugins'
// `anvil` ranges against: --host-version, else its version.dll's
// ProductVersion, else this build's own (issue #544).
void take_host_version(const GameOptions& options, GameReport& report) {
    if (options.host_version) {
        report.host_version = options.host_version->to_string();
        report.host_source = HostVersionSource::Option;
        return;
    }
    const std::filesystem::path dll = options.game_dir / "version.dll";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(dll, ec)) {
        report.host_note = "no version.dll in " + report.game_dir;
    } else if (const auto product = read_dll_product(dll); !product || product->name != "Anvil") {
        report.host_note = shown(dll) + " is not Anvil's";
    } else if (const auto version = Version::parse(product->version); !version) {
        report.host_note = shown(dll) + " names no version (ProductVersion \"" + product->version + "\")";
    } else {
        report.host_version = version->to_string();
        report.host_source = HostVersionSource::VersionDll;
        return;
    }
    report.host_version = Version::parse(ANVIL_VERSION)->to_string();
    report.host_source = HostVersionSource::Tool;
}

}  // namespace

const char* host_source_name(HostVersionSource source) {
    switch (source) {
        case HostVersionSource::Option: return "option";
        case HostVersionSource::VersionDll: return "version.dll";
        case HostVersionSource::Tool: break;
    }
    return "tool";
}

GameReport check_game(const GameOptions& options) {
    GameReport report;
    report.game_dir = shown(options.game_dir);
    auto cannot = [&](const std::string& why) {
        report.exit_code = kExitCannotCheck;
        report.cannot_check = why;
        return report;
    };
    std::error_code ec;
    if (!std::filesystem::is_directory(options.game_dir, ec)) return cannot(report.game_dir + " is not a folder");
    const std::filesystem::path exe = options.game_dir / "eu4.exe";
    if (!std::filesystem::is_regular_file(exe, ec)) return cannot("no eu4.exe in " + report.game_dir);
    const std::filesystem::path anvil_dir = options.game_dir / "anvil";
    if (!std::filesystem::is_directory(anvil_dir, ec)) {
        return cannot("no anvil folder in " + report.game_dir + " (is Anvil installed?)");
    }

    std::string error;
    const auto reader = FileImageReader::load(exe, error);
    if (!reader) return cannot("cannot read eu4.exe: " + error);
    GameImage game;
    game.reader = &*reader;
    game.image = PeImage::parse(*reader, error);
    if (!game.image) return cannot("eu4.exe: " + error);
    report.exe_sha256 = sha256_file(exe);
    if (report.exe_sha256.empty()) return cannot("cannot hash eu4.exe");
    const auto& build_of = options.build_of ? options.build_of : known_build;
    game.build_id = build_of(report.exe_sha256);
    if (game.build_id != nullptr) report.exe_build = game.build_id;
    game.unknown_build = unknown_build_reason(options.game_dir);
    game.exe_sha256 = report.exe_sha256;

    // The inventory, the choices as saved and the plan of the next start.
    const std::filesystem::path user_dir = options.user_dir.empty() ? eu4_user_dir(options.game_dir) : options.user_dir;
    report.inventory = take_inventory(anvil_dir, user_dir, game);
    report.choices = read_player_choices(report.inventory);
    report.next_start = read_next_start(anvil_dir).record;
    take_host_version(options, report);
    StartConditions conditions;
    conditions.host_version = Version::parse(report.host_version);
    report.plan = plan_start(report.inventory, report.choices, conditions);
    const AnvilSettings& settings = report.choices.anvil_toml;
    if (!settings.error.empty()) {
        report.problems.push_back({shown(anvil_dir / "anvil.toml"), settings.line,
                                   settings.error + " (the loader loads no plugin until it is fixed)"});
    }
    report.user_dir = shown(user_dir);
    const Playset& playset = report.inventory.playset;
    if (!playset.error.empty()) {
        report.problems.push_back({shown(user_dir / "dlc_load.json"), 0,
                                   playset.error + " (the loader then loads no plugins from mods)"});
    }
    if (!report.choices.trust.error.empty()) {
        report.problems.push_back({shown(anvil_dir / "trust.toml"), report.choices.trust.line,
                                   report.choices.trust.error + " (the loader then approves no plugin from a mod)"});
    }
    for (const PlaysetMod& mod : playset.mods) {
        if (!mod.error.empty() && !mod.error.starts_with("a zipped mod")) {
            report.mod_warnings.push_back("mod " + mod.descriptor + ": " + mod.error);
        }
    }

    // Each plugin's report: the inventory's check with the checks for authors.
    CheckOptions check;
    check.exe = exe;
    check.write_lock = false;
    check.build_of = options.build_of;
    check.translations = false;
    for (const InventoryPlugin& local : report.inventory.local) {
        GamePlugin plugin;
        plugin.folder = name_of(local.folder);
        if (local.check.manifest_missing) {
            plugin.report.exit_code = kExitProblems;
            plugin.report.with_exe = true;
            plugin.report.problems.push_back({shown(local.folder / "plugin.toml"), 0, "plugin.toml not found"});
            plugin.report.skip_reason = i18n::english(local.check.skip);
            plugin.report.skip_why = local.check.skip;
        } else {
            check.plugin_dir = local.folder;
            plugin.report = check_report(local.check, check, &game);
        }
        report.plugins.push_back(std::move(plugin));
    }
    for (size_t i = 0; i < report.plan.mods.size(); ++i) {
        const ModPlugin& p = report.plan.mods[i];
        GameModPlugin gm;
        gm.id = p.id;
        gm.mod = p.mod.name;
        gm.descriptor = p.mod.descriptor;
        gm.workshop_id = p.mod.workshop_id;
        gm.folder = shown(p.folder);
        gm.state = p.state;
        gm.reason = p.reason;
        gm.why = p.why.empty() ? i18n::Message(p.reason) : p.why;
        gm.hash = p.hash;
        if (p.manifest) gm.version = p.manifest->version.to_string();
        gm.problem = p.problem;
        if (p.approval && p.bound) {
            gm.approved_version = p.approval->version;
            gm.approved_hash = p.approval->hash;
            gm.approved_date = p.approval->approved;
        }
        if (p.loads()) gm.loads_from = shown(p.load_folder);
        gm.copy_missing = p.copy_missing;
        gm.others = p.others;
        // The approved copy that loads while an update waits has the plan's
        // check: its own, or that of another mod's folder with its hash.
        const PluginCheck* loads = &report.inventory.mods[i].check;
        if (p.state == ModPluginState::UpdatePending && p.approval) {
            if (const PluginCheck* c = report.inventory.copy_check(p.id, p.approval->hash)) loads = c;
        }
        check.plugin_dir = p.loads() && !p.copy_missing ? p.load_folder : p.folder;
        gm.report = check_report(*loads, check, &game);
        if (gm.report.manifest) gm.report.manifest->folder = check.plugin_dir;
        if (p.state == ModPluginState::UpdatePending) {
            check.plugin_dir = p.folder;
            gm.update = check_report(report.inventory.mods[i].check, check, &game);
        }
        report.mod_plugins.push_back(std::move(gm));
    }

    std::vector<const CheckReport*> loaded;
    for (const PlannedPlugin& p : report.plan.load) {
        loaded.push_back(p.mod >= 0 ? &report.mod_plugins[static_cast<size_t>(p.mod)].report
                                    : &report.plugins[static_cast<size_t>(p.local)].report);
    }
    report.warnings = find_overlaps(loaded);

    // Exit code: anything that keeps a plugin from loading, or that is wrong in
    // a plugin that should, is a problem; what the player disabled is not, and
    // neither is a plugin from a mod that waits for the player (pending, a
    // conflict, shadowed, an update pending).
    bool problems = !report.problems.empty();
    for (const GamePlugin& plugin : report.plugins) {
        if (turned_off(report, plugin)) continue;
        if (plugin.report.exit_code == kExitCannotCheck) {
            report.exit_code = kExitCannotCheck;
            if (report.cannot_check.empty()) report.cannot_check = plugin.folder + ": " + plugin.report.cannot_check;
        } else if (!plugin.report.problems.empty()) {
            problems = true;
        }
    }
    for (const GameModPlugin& gm : report.mod_plugins) {
        if (gm.state == ModPluginState::Invalid) problems = true;
        const PlannedPlugin* p = planned(report, gm);
        if (gm.loads_from.empty() || (p != nullptr && p->disabled)) continue;
        if (gm.report.exit_code == kExitCannotCheck) {
            report.exit_code = kExitCannotCheck;
            if (report.cannot_check.empty()) report.cannot_check = gm.id + " from mod '" + gm.mod + "': " + gm.report.cannot_check;
        } else if (!gm.report.problems.empty()) {
            problems = true;
        }
    }
    for (const PlannedPlugin* s : listed_skipped(report)) problems = problems || !s->disabled;
    if (report.exit_code != kExitCannotCheck) report.exit_code = problems ? kExitProblems : kExitOk;
    return report;
}

// The plugin's index in the report: by address, or for a copy by its
// folder (and its mod).
const PlannedPlugin* planned(const GameReport& report, const GamePlugin& plugin) {
    for (size_t i = 0; i < report.plugins.size(); ++i) {
        if (&report.plugins[i] == &plugin || report.plugins[i].folder == plugin.folder) return report.plan.of_local(i);
    }
    return nullptr;
}

const PlannedPlugin* planned(const GameReport& report, const GameModPlugin& plugin) {
    for (size_t i = 0; i < report.mod_plugins.size(); ++i) {
        const GameModPlugin& m = report.mod_plugins[i];
        if (&m == &plugin || (m.id == plugin.id && m.descriptor == plugin.descriptor)) return report.plan.of_mod(i);
    }
    return nullptr;
}

const char* status_name(const PlannedPlugin* plugin) {
    if (plugin != nullptr && plugin->loads()) return "loaded";
    return plugin != nullptr && plugin->disabled ? "disabled" : "skipped";
}

bool turned_off(const GameReport& report, const GamePlugin& plugin) {
    if (!report.plan.enabled) return true;
    const auto& m = plugin.report.manifest;
    const auto& list = report.plan.lists.disabled_plugins;
    return m && std::find(list.begin(), list.end(), m->id) != list.end();
}

std::vector<const PlannedPlugin*> listed_skipped(const GameReport& report) {
    std::vector<const PlannedPlugin*> out;
    for (const PlannedPlugin& p : report.plan.skipped) {
        if (p.mod >= 0 && !report.plan.mods[static_cast<size_t>(p.mod)].loads()) continue;
        out.push_back(&p);
    }
    return out;
}

namespace {

// The report of a planned plugin, local or from a mod.
const CheckReport* report_of(const GameReport& report, const PlannedPlugin& p) {
    if (p.local >= 0) return &report.plugins[static_cast<size_t>(p.local)].report;
    if (p.mod >= 0) return &report.mod_plugins[static_cast<size_t>(p.mod)].report;
    return nullptr;
}

// The resolver's or the start's reason for a plugin from a mod whose state
// loads but which does not; "" otherwise.
std::string skip_reason(const GameReport& report, const GameModPlugin& p) {
    const PlannedPlugin* planned_plugin = planned(report, p);
    if (p.loads_from.empty() || planned_plugin == nullptr || planned_plugin->loads()) return {};
    return planned_plugin->reason;
}

// A plugin from a mod whose problems count: one that loads or should, unless disabled.
bool mod_problems_count(const GameReport& report, const GameModPlugin& p) {
    const PlannedPlugin* planned_plugin = planned(report, p);
    return !p.loads_from.empty() && (planned_plugin == nullptr || !planned_plugin->disabled);
}

size_t counted_problems(const GameReport& report) {
    size_t count = report.problems.size();
    for (const GamePlugin& plugin : report.plugins) {
        if (!turned_off(report, plugin)) count += plugin.report.problems.size();
    }
    for (const GameModPlugin& plugin : report.mod_plugins) {
        if (plugin.state == ModPluginState::Invalid) ++count;
        else if (mod_problems_count(report, plugin)) count += plugin.report.problems.size();
    }
    // A plugin that fails its own checks is already counted by them.
    for (const PlannedPlugin* s : listed_skipped(report)) {
        const CheckReport* checks = report_of(report, *s);
        if (!s->disabled && (checks == nullptr || checks->problems.empty())) ++count;
    }
    return count;
}

std::string mod_label(const GameModPlugin& p) {
    return "mod '" + p.mod + "' (" + (p.workshop_id.empty() ? std::string("local mod") : "Workshop " + p.workshop_id) + ")";
}

const char* phase_name(const PlannedPlugin& p) { return p.manifest && p.manifest->phase == Phase::Early ? "early" : "main"; }

// The folder's name of a local plugin; "" for one from a mod.
std::string folder_of(const GameReport& report, const PlannedPlugin& p) {
    return p.local >= 0 ? report.plugins[static_cast<size_t>(p.local)].folder : std::string();
}

// What the next start writes, one line each (design §16).
std::vector<std::string> write_lines(const GameReport& report) {
    std::vector<std::string> lines;
    const StartWrites& w = report.plan.writes;
    for (const auto& path : w.remove) lines.push_back("removes " + shown(path) + " (an approved copy trust.toml no longer names)");
    for (const PlannedCopy& c : w.copies) {
        lines.push_back("makes the approved copy of " + c.id + " from mod '" +
                        report.plan.mods[static_cast<size_t>(c.mod)].mod.name + "' at " + shown(c.to));
    }
    for (const PlannedRetiredMove& m : w.retired) {
        lines.push_back("moves the retired setting(s) " + m.move.removed + " of " + m.plugin + " into anvil.toml");
    }
    return lines;
}

std::string safe_mode_line(const GameReport& report) {
    return "the next start is in safe mode: the last " + std::to_string(report.next_start.unfinished) +
           " starts ended before the game was ready; it loads only the standard plugins the overlay needs";
}

// The Anvil version the plan checked the `anvil` ranges against, and where it
// comes from; "" when the folder could not be checked.
std::string host_line(const GameReport& report) {
    if (report.host_version.empty()) return {};
    const std::string head = "plugins checked against Anvil " + report.host_version;
    switch (report.host_source) {
        case HostVersionSource::Option: return head + " (--host-version)";
        case HostVersionSource::VersionDll: return head + " (the version.dll installed in " + report.game_dir + ")";
        case HostVersionSource::Tool: break;
    }
    return head + ", the version of this anvil check: " + report.host_note + " (--host-version picks another)";
}

std::string game_json(const GameReport& report) {
    const char* result = report.exit_code == kExitCannotCheck ? "cannot-check"
                         : report.exit_code == kExitOk         ? "ok"
                                                               : "problems";
    std::string out = "{\n  \"tool\": " + json_string(tool_name()) + ",\n  \"result\": \"" + result + "\",\n";
    out += "  \"exit_code\": " + std::to_string(report.exit_code) + ",\n";
    if (!report.cannot_check.empty()) out += "  \"cannot_check\": " + json_string(report.cannot_check) + ",\n";
    out += "  \"game\": {\"folder\": " + json_string(report.game_dir);
    if (!report.exe_sha256.empty()) {
        out += ", \"exe\": {\"name\": " + json_string(report.exe_name) + ", \"sha256\": " + json_string(report.exe_sha256) +
               ", \"build\": " + (report.exe_build.empty() ? std::string("null") : json_string(report.exe_build)) + "}";
    }
    const AnvilSettings& toml = report.choices.anvil_toml;
    out += "},\n";
    if (!report.host_version.empty()) {
        out += "  \"host\": {\"version\": " + json_string(report.host_version) + ", \"source\": " +
               json_string(host_source_name(report.host_source)) +
               (report.host_note.empty() ? std::string() : ", \"note\": " + json_string(report.host_note)) + "},\n";
    }
    out += "  \"anvil_toml\": {\"enabled\": " + std::string(toml.enabled ? "true" : "false") + ", \"disabled_plugins\": [";
    for (size_t i = 0; i < toml.disabled_plugins.size(); ++i) {
        out += (i == 0 ? "" : ", ") + json_string(toml.disabled_plugins[i]);
    }
    out += "]},\n  \"problems\": [";
    for (size_t i = 0; i < report.problems.size(); ++i) {
        const Problem& p = report.problems[i];
        out += std::string(i == 0 ? "\n" : ",\n") + "    {\"file\": " + json_string(p.file) +
               ", \"line\": " + std::to_string(p.line) + ", \"message\": " + json_string(p.message) + "}";
    }
    out += report.problems.empty() ? "],\n" : "\n  ],\n";

    out += "  \"plugins\": [";
    for (size_t i = 0; i < report.plugins.size(); ++i) {
        const GamePlugin& p = report.plugins[i];
        out += std::string(i == 0 ? "\n    " : ",\n    ") + report_json(p.report, "    ", p.folder, status_name(planned(report, p)));
    }
    out += report.plugins.empty() ? "],\n" : "\n  ],\n";

    const auto string_or_null = [](const std::string& text) { return text.empty() ? std::string("null") : json_string(text); };
    out += "  \"mods\": {\"user_dir\": " + json_string(report.user_dir) +
           ", \"dlc_load\": " + (report.inventory.playset.found ? "true" : "false") +
           ", \"enabled\": " + std::to_string(report.inventory.playset.mods.size()) + ", \"warnings\": [";
    for (size_t i = 0; i < report.mod_warnings.size(); ++i) out += (i == 0 ? "" : ", ") + json_string(report.mod_warnings[i]);
    out += "]},\n  \"mod_plugins\": [";
    for (size_t i = 0; i < report.mod_plugins.size(); ++i) {
        const GameModPlugin& p = report.mod_plugins[i];
        const std::string in = "      ";
        const std::string skipped_because = skip_reason(report, p);
        out += std::string(i == 0 ? "\n" : ",\n") + "    {\n";
        out += in + "\"id\": " + json_string(p.id) + ",\n";
        out += in + "\"mod\": {\"name\": " + json_string(p.mod) + ", \"descriptor\": " + json_string(p.descriptor) +
               ", \"workshop_id\": " + string_or_null(p.workshop_id) + ", \"folder\": " + json_string(p.folder) + "},\n";
        out += in + "\"state\": " + json_string(state_name(p.state)) + ",\n";
        out += in + "\"reason\": " + json_string(p.reason) + ",\n";
        out += in + "\"status\": " + json_string(status_name(planned(report, p))) + ",\n";
        if (!skipped_because.empty()) out += in + "\"skip_reason\": " + json_string(skipped_because) + ",\n";
        out += in + "\"version\": " + string_or_null(p.version) + ",\n";
        out += in + "\"hash\": " + string_or_null(p.hash) + ",\n";
        out += in + "\"approved\": " +
               (p.approved_hash.empty() ? std::string("null")
                                        : "{\"version\": " + json_string(p.approved_version) + ", \"hash\": " +
                                              json_string(p.approved_hash) + ", \"date\": " + json_string(p.approved_date) + "}") +
               ",\n";
        out += in + "\"loads_from\": " + string_or_null(p.loads_from) + ",\n";
        out += in + "\"copy_missing\": " + (p.copy_missing ? "true" : "false") + ",\n";
        out += in + "\"others\": [";
        for (size_t j = 0; j < p.others.size(); ++j) out += (j == 0 ? "" : ", ") + json_string(p.others[j]);
        out += "],\n" + in + "\"check\": " + report_json(p.report, in, p.report.manifest ? shown(p.report.manifest->folder) : p.folder) +
               "\n    }";
    }
    out += report.mod_plugins.empty() ? "],\n" : "\n  ],\n";

    out += "  \"load_order\": [";
    for (size_t i = 0; i < report.plan.load.size(); ++i) {
        const PlannedPlugin& p = report.plan.load[i];
        out += std::string(i == 0 ? "\n" : ",\n") + "    {\"id\": " + json_string(p.id) + ", \"version\": " +
               json_string(p.manifest->version.to_string()) + ", \"phase\": " + json_string(phase_name(p)) +
               ", \"folder\": " + json_string(folder_of(report, p)) +
               (p.mod_name.empty() ? std::string() : ", \"mod\": " + json_string(p.mod_name)) + "}";
    }
    out += report.plan.load.empty() ? "],\n" : "\n  ],\n";

    const std::vector<const PlannedPlugin*> skipped = listed_skipped(report);
    out += "  \"skipped\": [";
    for (size_t i = 0; i < skipped.size(); ++i) {
        const PlannedPlugin& s = *skipped[i];
        out += std::string(i == 0 ? "\n" : ",\n") + "    {\"id\": " + json_string(s.id) + ", \"folder\": " +
               json_string(folder_of(report, s)) + ", \"reason\": " + json_string(s.reason) +
               ", \"disabled\": " + (s.disabled ? "true" : "false") +
               (s.mod_name.empty() ? std::string() : ", \"mod\": " + json_string(s.mod_name)) + "}";
    }
    out += skipped.empty() ? "],\n" : "\n  ],\n";

    // The next start (ADR 0014): safe mode, and what it writes first.
    const StartWrites& w = report.plan.writes;
    out += "  \"next_start\": {\"safe_mode\": " + std::string(report.next_start.safe_mode ? "true" : "false") +
           ", \"unfinished_starts\": " + std::to_string(report.next_start.unfinished) + ", \"writes\": {\"remove\": [";
    for (size_t i = 0; i < w.remove.size(); ++i) out += (i == 0 ? "" : ", ") + json_string(shown(w.remove[i]));
    out += "], \"copies\": [";
    for (size_t i = 0; i < w.copies.size(); ++i) {
        const PlannedCopy& c = w.copies[i];
        out += std::string(i == 0 ? "" : ", ") + "{\"id\": " + json_string(c.id) + ", \"mod\": " +
               json_string(report.plan.mods[static_cast<size_t>(c.mod)].mod.name) + ", \"to\": " + json_string(shown(c.to)) + "}";
    }
    out += "], \"retired\": [";
    for (size_t i = 0; i < w.retired.size(); ++i) {
        const PlannedRetiredMove& m = w.retired[i];
        out += std::string(i == 0 ? "" : ", ") + "{\"plugin\": " + json_string(m.plugin) + ", \"settings\": " +
               json_string(m.move.removed) + ", \"anvil_toml\": [";
        for (size_t j = 0; j < m.move.edits.size(); ++j) {
            const ListEdit& e = m.move.edits[j];
            out += std::string(j == 0 ? "" : ", ") + "{\"key\": " + json_string(e.key) + ", \"id\": " + json_string(e.id) +
                   ", \"listed\": " + (e.listed ? "true" : "false") + "}";
        }
        out += "]}";
    }
    out += "]}},\n";

    out += "  \"warnings\": [";
    for (size_t i = 0; i < report.warnings.size(); ++i) {
        const OverlapWarning& o = report.warnings[i];
        out += std::string(i == 0 ? "\n" : ",\n") + "    {\"file\": " + json_string(o.file) + ", \"line\": " + std::to_string(o.line) +
               ", \"message\": " + json_string(o.message) + ", \"plugins\": [" + json_string(o.plugin) + ", " +
               json_string(o.other) + "]}";
    }
    out += report.warnings.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

}  // namespace

std::string format_game_report(const GameReport& report, Format format) {
    if (format == Format::Json) return game_json(report);
    const bool github = format == Format::Github;

    if (report.plugins.empty() && report.exit_code == kExitCannotCheck) {
        return github ? "::error title=anvil check::" + github_escape(report.cannot_check, false) + "\n"
                      : "error: cannot check: " + report.cannot_check + "\n";
    }
    std::string out = problem_lines(report.problems, format, "error");
    for (const GamePlugin& plugin : report.plugins) {
        const bool off = turned_off(report, plugin);
        out += problem_lines(plugin.report.problems, format, off ? "warning" : "error");
        if (plugin.report.exit_code == kExitCannotCheck) {
            out += github ? "::error title=anvil check::" + github_escape(plugin.folder + ": " + plugin.report.cannot_check, false) + "\n"
                          : "error: cannot check " + plugin.folder + ": " + plugin.report.cannot_check + "\n";
        } else {
            const std::string summary =
                plugin.report.plugin.empty()
                    ? plugin.folder + ": " + std::to_string(plugin.report.problems.size()) + " problem(s)"
                    : report_summary(plugin.report);
            out += summary + (off ? " (disabled)" : "") + "\n";
        }
    }
    std::vector<Problem> mod_warnings;
    for (const std::string& w : report.mod_warnings) mod_warnings.push_back({report.user_dir + "/dlc_load.json", 0, w});
    out += problem_lines(mod_warnings, format, "warning");
    const size_t enabled_mods = report.inventory.playset.mods.size();
    if (!report.mod_plugins.empty() || enabled_mods > 0) {
        out += "plugins in mods (" + std::to_string(report.mod_plugins.size()) + ", " + std::to_string(enabled_mods) +
               " enabled mod(s) in " + report.user_dir + "):\n";
    }
    for (const GameModPlugin& p : report.mod_plugins) {
        const bool counts = mod_problems_count(report, p);
        const bool loads = std::string_view(status_name(planned(report, p))) == "loaded";
        out += "  " + p.id + (p.version.empty() ? "" : " " + p.version) + " in " + mod_label(p) + ": " + state_name(p.state) +
               "; " + p.reason + (loads ? " (loads)" : "") + "\n";
        if (const std::string why = skip_reason(report, p); !why.empty()) out += "    skipped: " + why + "\n";
        if (p.state == ModPluginState::Invalid && github) {
            out += "::error file=" + github_escape(p.folder + "/plugin.toml", true) + ",title=anvil check::" +
                   github_escape(p.id + " in " + mod_label(p) + ": " + p.reason, false) + "\n";
        }
        out += problem_lines(p.report.problems, format, counts ? "error" : "warning");
    }
    if (const std::string line = host_line(report); !line.empty()) out += line + "\n";
    out += "load order (" + std::to_string(report.plan.load.size()) + "):\n";
    for (size_t i = 0; i < report.plan.load.size(); ++i) {
        const PlannedPlugin& p = report.plan.load[i];
        out += "  " + std::to_string(i + 1) + ". " + p.id + " " + p.manifest->version.to_string() + " (" + phase_name(p) + ")" +
               (p.mod_name.empty() ? "" : " from mod '" + p.mod_name + "'") + "\n";
    }
    const std::vector<const PlannedPlugin*> skipped = listed_skipped(report);
    out += "skipped (" + std::to_string(skipped.size()) + "):\n";
    for (const PlannedPlugin* s : skipped) {
        out += "  " + s->id + (s->mod_name.empty() ? "" : " from mod '" + s->mod_name + "'") + (s->disabled ? " (disabled)" : "") +
               ": " + s->reason + "\n";
        if (github && !s->disabled) {
            // The plugin's own problems already explain a plugin that fails its checks.
            const CheckReport* checks = report_of(report, *s);
            if (checks != nullptr && !checks->problems.empty()) continue;
            const std::string file = s->mod >= 0 ? report.mod_plugins[static_cast<size_t>(s->mod)].folder + "/plugin.toml"
                                                 : report.game_dir + "/anvil/plugins/" + folder_of(report, *s) + "/plugin.toml";
            out += "::error file=" + github_escape(file, true) + ",title=anvil check::" +
                   github_escape(s->id + " is skipped: " + s->reason, false) + "\n";
        }
    }
    std::vector<Problem> warnings;
    for (const OverlapWarning& w : report.warnings) warnings.push_back({w.file, w.line, w.message});
    out += problem_lines(warnings, format, "warning");
    // The next start (ADR 0014): safe mode, and what it writes before any plugin loads.
    if (report.next_start.safe_mode) {
        out += github ? "::warning title=anvil check::" + github_escape(safe_mode_line(report), false) + "\n"
                      : safe_mode_line(report) + "\n";
    }
    if (const std::vector<std::string> lines = write_lines(report); !lines.empty()) {
        out += "the next start writes first (" + std::to_string(lines.size()) + "):\n";
        for (const std::string& line : lines) out += "  " + line + "\n";
    }

    out += "game " + report.game_dir + ": " + std::to_string(report.plugins.size()) + " plugin(s), " +
           std::to_string(report.plan.load.size()) + " will load, " + std::to_string(skipped.size()) + " skipped";
    if (!report.mod_plugins.empty()) {
        size_t waiting = 0;
        for (const GameModPlugin& p : report.mod_plugins) {
            if (p.state == ModPluginState::Pending || p.state == ModPluginState::UpdatePending ||
                p.state == ModPluginState::Conflict) {
                ++waiting;
            }
        }
        out += ", " + std::to_string(report.mod_plugins.size()) + " in mods (" + std::to_string(waiting) +
               " waiting for the player)";
    }
    if (!report.warnings.empty()) out += ", " + std::to_string(report.warnings.size()) + " warning(s)";
    if (report.exit_code == kExitCannotCheck) {
        out += ": cannot check completely\n";
    } else {
        const size_t problems = counted_problems(report);
        out += problems == 0 ? ": OK\n" : ": " + std::to_string(problems) + " problem(s)\n";
    }
    return out;
}

}  // namespace anvil::cli
