#include "cli/game.hpp"

#include "core/discovery.hpp"
#include "core/game_build.hpp"
#include "core/pe_image.hpp"
#include "core/resolver.hpp"

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
// The same range at the same address is the normal case of a chain, not a
// warning. One warning per pair of plugins, at the earlier plugin's symbol.
std::vector<OverlapWarning> find_overlaps(const std::vector<const GamePlugin*>& loaded,
                                          const std::filesystem::path& plugins_dir) {
    std::vector<OverlapWarning> warnings;
    for (size_t i = 0; i < loaded.size(); ++i) {
        for (size_t j = i + 1; j < loaded.size(); ++j) {
            const CheckReport& a = loaded[i]->report;
            const CheckReport& b = loaded[j]->report;
            const CodeRange* first_a = nullptr;
            const CodeRange* first_b = nullptr;
            size_t hits = 0;
            for (const CodeRange& x : a.code_ranges) {
                for (const CodeRange& y : b.code_ranges) {
                    if (!ranges_overlap(x, y) || (x.rva == y.rva && x.size == y.size)) continue;
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
            w.file = shown(plugins_dir / loaded[i]->folder / a.manifest->symbols);
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

}  // namespace

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
    if (!PeImage::parse(*reader, error)) return cannot("eu4.exe: " + error);
    report.exe_sha256 = sha256_file(exe);
    if (report.exe_sha256.empty()) return cannot("cannot hash eu4.exe");
    const auto& build_of = options.build_of ? options.build_of : known_build;
    if (const char* id = build_of(report.exe_sha256)) report.exe_build = id;

    const AnvilSettings settings = read_anvil_settings(anvil_dir / "anvil.toml");
    report.enabled = settings.enabled;
    report.disabled_plugins = settings.disabled_plugins;
    if (!settings.error.empty()) {
        report.problems.push_back({shown(anvil_dir / "anvil.toml"), settings.line,
                                   settings.error + " (the loader ignores this file and uses the defaults)"});
    }
    const std::set<std::string> disabled_ids(settings.disabled_plugins.begin(), settings.disabled_plugins.end());

    // Every plugin folder, checked like `anvil check <folder> --exe <eu4.exe>`.
    const std::filesystem::path plugins_dir = anvil_dir / "plugins";
    const std::vector<std::filesystem::path> folders = plugin_folders(plugins_dir);
    for (const auto& folder : folders) {
        GamePlugin plugin;
        plugin.folder = name_of(folder);
        if (!std::filesystem::is_regular_file(folder / "plugin.toml", ec)) {
            plugin.report.exit_code = kExitProblems;
            plugin.report.with_exe = true;
            plugin.report.problems.push_back({shown(folder / "plugin.toml"), 0, "plugin.toml not found"});
            plugin.report.skip_reason = load_manifest(folder).error;
        } else {
            CheckOptions check;
            check.plugin_dir = folder;
            check.exe = exe;
            check.write_lock = false;
            check.build_of = options.build_of;
            plugin.report = check_plugin(check);
        }
        const bool by_id = plugin.report.manifest && disabled_ids.contains(plugin.report.manifest->id);
        plugin.disabled = !settings.enabled || by_id;
        report.plugins.push_back(std::move(plugin));
    }

    // Discovery as the loader does it: a broken plugin.toml or a missing entry
    // skips the plugin before the resolver sees it.
    ResolveInput input;
    input.host_api = kManifestApi;
    input.host_version = Version::parse(ANVIL_VERSION);
    input.disabled = settings.disabled_plugins;
    std::map<std::filesystem::path, size_t> by_folder;
    std::vector<Skipped> skipped;
    for (size_t i = 0; i < report.plugins.size(); ++i) {
        const GamePlugin& plugin = report.plugins[i];
        const std::filesystem::path folder = folders[i];
        if (!plugin.report.manifest) {
            skipped.push_back({plugin.folder, plugin.report.skip_reason, folder, !settings.enabled});
            continue;
        }
        if (std::string reason = missing_entry_reason(*plugin.report.manifest); !reason.empty()) {
            skipped.push_back({plugin.report.manifest->id, std::move(reason), folder, plugin.disabled});
            continue;
        }
        by_folder[folder] = i;
        input.manifests.push_back(*plugin.report.manifest);
    }
    // The symbols are verified (and their build compared) by check_plugin already.
    input.verify = [&](const Manifest& m) {
        const auto it = by_folder.find(m.folder);
        return it == by_folder.end() ? std::string() : report.plugins[it->second].report.skip_reason;
    };

    ResolveResult resolved;
    if (settings.enabled) {
        resolved = resolve(input);
    } else {
        for (const Manifest& m : input.manifests) {
            resolved.skipped.push_back({m.id, "anvil.toml has enabled = false: no plugins load", m.folder, true});
        }
    }
    skipped.insert(skipped.end(), resolved.skipped.begin(), resolved.skipped.end());

    std::set<std::filesystem::path> loaded_folders;
    std::vector<const GamePlugin*> loaded;
    for (const Manifest& m : resolved.order) {
        loaded_folders.insert(m.folder);
        report.load_order.push_back({m.id, m.version.to_string(), m.phase == Phase::Early ? "early" : "main", name_of(m.folder)});
        loaded.push_back(&report.plugins[by_folder.at(m.folder)]);
    }
    for (const Skipped& s : skipped) report.skipped.push_back({s.id, name_of(s.folder), s.reason, s.disabled});
    for (size_t i = 0; i < report.plugins.size(); ++i) {
        GamePlugin& plugin = report.plugins[i];
        plugin.status = loaded_folders.contains(folders[i]) ? "loaded" : plugin.disabled ? "disabled" : "skipped";
    }
    report.warnings = find_overlaps(loaded, plugins_dir);

    // Exit code: anything that keeps a plugin from loading, or that is wrong in
    // a plugin that should, is a problem; what the player disabled is not.
    bool problems = !report.problems.empty();
    for (const GamePlugin& plugin : report.plugins) {
        if (plugin.disabled) continue;
        if (plugin.report.exit_code == kExitCannotCheck) {
            report.exit_code = kExitCannotCheck;
            if (report.cannot_check.empty()) report.cannot_check = plugin.folder + ": " + plugin.report.cannot_check;
        } else if (!plugin.report.problems.empty()) {
            problems = true;
        }
    }
    for (const SkippedPlugin& s : report.skipped) problems = problems || !s.disabled;
    if (report.exit_code != kExitCannotCheck) report.exit_code = problems ? kExitProblems : kExitOk;
    return report;
}

namespace {

const GamePlugin* plugin_in(const GameReport& report, const std::string& folder) {
    for (const GamePlugin& plugin : report.plugins) {
        if (plugin.folder == folder) return &plugin;
    }
    return nullptr;
}

size_t counted_problems(const GameReport& report) {
    size_t count = report.problems.size();
    for (const GamePlugin& plugin : report.plugins) {
        if (!plugin.disabled) count += plugin.report.problems.size();
    }
    // A plugin that fails its own checks is already counted by them.
    for (const SkippedPlugin& s : report.skipped) {
        const GamePlugin* plugin = plugin_in(report, s.folder);
        if (!s.disabled && (plugin == nullptr || plugin->report.problems.empty())) ++count;
    }
    return count;
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
    out += "},\n  \"anvil_toml\": {\"enabled\": " + std::string(report.enabled ? "true" : "false") + ", \"disabled_plugins\": [";
    for (size_t i = 0; i < report.disabled_plugins.size(); ++i) {
        out += (i == 0 ? "" : ", ") + json_string(report.disabled_plugins[i]);
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
        out += std::string(i == 0 ? "\n    " : ",\n    ") + report_json(p.report, "    ", p.folder, p.status);
    }
    out += report.plugins.empty() ? "],\n" : "\n  ],\n";

    out += "  \"load_order\": [";
    for (size_t i = 0; i < report.load_order.size(); ++i) {
        const LoadedPlugin& p = report.load_order[i];
        out += std::string(i == 0 ? "\n" : ",\n") + "    {\"id\": " + json_string(p.id) + ", \"version\": " + json_string(p.version) +
               ", \"phase\": " + json_string(p.phase) + ", \"folder\": " + json_string(p.folder) + "}";
    }
    out += report.load_order.empty() ? "],\n" : "\n  ],\n";

    out += "  \"skipped\": [";
    for (size_t i = 0; i < report.skipped.size(); ++i) {
        const SkippedPlugin& s = report.skipped[i];
        out += std::string(i == 0 ? "\n" : ",\n") + "    {\"id\": " + json_string(s.id) + ", \"folder\": " + json_string(s.folder) +
               ", \"reason\": " + json_string(s.reason) + ", \"disabled\": " + (s.disabled ? "true" : "false") + "}";
    }
    out += report.skipped.empty() ? "],\n" : "\n  ],\n";

    out += "  \"warnings\": [";
    for (size_t i = 0; i < report.warnings.size(); ++i) {
        const OverlapWarning& w = report.warnings[i];
        out += std::string(i == 0 ? "\n" : ",\n") + "    {\"file\": " + json_string(w.file) + ", \"line\": " + std::to_string(w.line) +
               ", \"message\": " + json_string(w.message) + ", \"plugins\": [" + json_string(w.plugin) + ", " +
               json_string(w.other) + "]}";
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
        out += problem_lines(plugin.report.problems, format, plugin.disabled ? "warning" : "error");
        if (plugin.report.exit_code == kExitCannotCheck) {
            out += github ? "::error title=anvil check::" + github_escape(plugin.folder + ": " + plugin.report.cannot_check, false) + "\n"
                          : "error: cannot check " + plugin.folder + ": " + plugin.report.cannot_check + "\n";
        } else {
            const std::string summary =
                plugin.report.plugin.empty()
                    ? plugin.folder + ": " + std::to_string(plugin.report.problems.size()) + " problem(s)"
                    : report_summary(plugin.report);
            out += summary + (plugin.disabled ? " (disabled)" : "") + "\n";
        }
    }
    out += "load order (" + std::to_string(report.load_order.size()) + "):\n";
    for (size_t i = 0; i < report.load_order.size(); ++i) {
        const LoadedPlugin& p = report.load_order[i];
        out += "  " + std::to_string(i + 1) + ". " + p.id + " " + p.version + " (" + p.phase + ")\n";
    }
    out += "skipped (" + std::to_string(report.skipped.size()) + "):\n";
    for (const SkippedPlugin& s : report.skipped) {
        out += "  " + s.id + (s.disabled ? " (disabled)" : "") + ": " + s.reason + "\n";
        if (github && !s.disabled) {
            // The plugin's own problems already explain a plugin that fails its checks.
            const GamePlugin* plugin = plugin_in(report, s.folder);
            if (plugin != nullptr && !plugin->report.problems.empty()) continue;
            out += "::error file=" + github_escape(report.game_dir + "/anvil/plugins/" + s.folder + "/plugin.toml", true) +
                   ",title=anvil check::" + github_escape(s.id + " is skipped: " + s.reason, false) + "\n";
        }
    }
    std::vector<Problem> warnings;
    for (const OverlapWarning& w : report.warnings) warnings.push_back({w.file, w.line, w.message});
    out += problem_lines(warnings, format, "warning");

    out += "game " + report.game_dir + ": " + std::to_string(report.plugins.size()) + " plugin(s), " +
           std::to_string(report.load_order.size()) + " will load, " + std::to_string(report.skipped.size()) + " skipped";
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
