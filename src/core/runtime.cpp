#include "runtime.hpp"

#include "discovery.hpp"
#include "game_build.hpp"
#include "gameplay_policy.hpp"
#include "host.hpp"
#include "log.hpp"
#include "manifest.hpp"
#include "changes.hpp"
#include "plugin_symbols.hpp"
#include "resolver.hpp"
#include "seh_guard.hpp"
#include "services.hpp"
#include "settings.hpp"
#include "symbol_check.hpp"

#include <anvil/anvil_game.h>
#include <anvil/anvil_plugin.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace anvil {
namespace {

struct Runtime {
    std::filesystem::path game_dir;
    std::filesystem::path anvil_dir;
    std::filesystem::path exe_path;
    PeIdentity identity;
    const KnownBuild* build = nullptr;
    AnvilGameInfo game{};
    std::unique_ptr<ChangeRegistry> changes;
    bool profile = false;  // anvil.toml: count calls per change
    ServiceBus bus;
    std::unique_ptr<GameplayPolicy> policy;  // design §13, fed by anvil-game's session event
    HostContext context;  // what every plugin's host calls act on
    AnvilHost host{};
    std::vector<std::unique_ptr<AnvilPlugin>> plugins;  // resolved, in load order
    std::vector<Skipped> skipped;
    LARGE_INTEGER start{};
    LARGE_INTEGER frequency{};
};

Runtime* g_runtime = nullptr;

double elapsed_ms() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - g_runtime->start.QuadPart) * 1000.0 /
           static_cast<double>(g_runtime->frequency.QuadPart ? g_runtime->frequency.QuadPart : 1);
}

std::string fmt_ms(double ms) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f ms", ms);
    return buffer;
}

// ---- loading ---------------------------------------------------------------

// Empty when every required dependency loaded (they come earlier in the
// load order); otherwise why the plugin is not loaded either.
std::string failed_dependency(const AnvilPlugin& plugin) {
    for (const auto& d : plugin.manifest.dependencies) {
        if (d.optional) continue;
        for (const auto& p : g_runtime->plugins) {
            if (p->manifest.id == d.id && p->state != "loaded") {
                return "requires plugin '" + d.id + "', which failed to load";
            }
        }
    }
    return {};
}

void load_plugin(AnvilPlugin& plugin) {
    const double begin = elapsed_ms();
    const std::filesystem::path dll = std::filesystem::path(plugin.plugin_dir) / plugin.manifest.entry;
    const std::string label = plugin.manifest.id + " " + plugin.manifest.version.to_string();
    if (std::string reason = failed_dependency(plugin); !reason.empty()) {
        plugin.state = "failed";
        g_runtime->policy->forget(plugin.owner);
        log_error(label + ": not loaded; " + reason);
        return;
    }
    plugin.module = LoadLibraryExW(dll.c_str(), nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (plugin.module == nullptr) {
        plugin.state = "failed";
        g_runtime->policy->forget(plugin.owner);
        log_error(label + ": cannot load " + narrow(dll.wstring()) + " (error " + std::to_string(GetLastError()) + ")");
        return;
    }
    const auto fn = reinterpret_cast<PluginLoadFn>(GetProcAddress(plugin.module, "anvil_plugin_load"));
    if (fn == nullptr) {
        plugin.state = "failed";
        g_runtime->policy->forget(plugin.owner);
        log_error(label + ": entry DLL does not export anvil_plugin_load");
        FreeLibrary(plugin.module);
        plugin.module = nullptr;
        return;
    }
    // Changes requested during anvil_plugin_load are staged and enabled together
    // after it returns ANVIL_OK; on failure none are (design §8.4).
    ChangeRegistry& changes = *g_runtime->changes;
    const LoadOutcome outcome = run_plugin_load(plugin, g_runtime->host, fn);
    if (outcome.commit_failed) {
        log_error(label + ": its " + std::to_string(outcome.staged) + " change(s) could not be enabled");
    }
    if (outcome.result != ANVIL_OK) {
        plugin.state = "failed";
        g_runtime->policy->forget(plugin.owner);
        char code[32] = {};
        if (outcome.exception != 0) std::snprintf(code, sizeof(code), "0x%08lX", outcome.exception);
        log_error(label + ": anvil_plugin_load " +
                  (outcome.exception != 0 ? std::string("crashed with exception ") + code
                                          : std::string("returned ") + result_name(outcome.result)) +
                  "; " + std::to_string(outcome.staged) + " change(s) rolled back; plugin stays inactive");
        // The DLL is not unloaded: threads it may have started could still run.
        return;
    }
    plugin.state = "loaded";
    std::string summary = label + ": loaded (" + (plugin.manifest.phase == Phase::Early ? "early" : "main") + ", " +
                          fmt_ms(elapsed_ms() - begin) + ", " + std::to_string(changes.count(plugin.owner)) +
                          " change(s))";
    if (plugin.manifest.affects_gameplay) summary += " — declares gameplay changes";
    log_info(summary);
    for (const auto& line : changes.describe(plugin.owner)) {
        Log::instance().write(ANVIL_LOG_DEBUG, plugin.manifest.id, line);
    }
}

void write_summary() {
    size_t loaded = 0, failed = 0;
    for (const auto& p : g_runtime->plugins) {
        if (p->state == "loaded") ++loaded;
        else if (p->state == "failed") ++failed;
    }
    log_info("status: " + std::to_string(loaded) + " loaded, " + std::to_string(failed) + " failed, " +
             std::to_string(g_runtime->skipped.size()) + " skipped");
    for (const auto& s : g_runtime->bus.describe()) log_info("service " + s);
}

DWORD WINAPI main_phase(LPVOID) {
    for (auto& p : g_runtime->plugins) {
        if (p->manifest.phase == Phase::Main) load_plugin(*p);
    }
    // The end of the core start-up of design §17 (entry point -> end of loading).
    log_info("plugins loaded at " + fmt_ms(elapsed_ms()));
    g_runtime->bus.publish(ANVIL_EVENT_PLUGINS_LOADED, nullptr);
    write_summary();
    if (g_runtime->build != nullptr) {
        const double begin = elapsed_ms();
        std::string actual;
        const bool ok = verify_build_file(*g_runtime->build, g_runtime->exe_path,
                                          g_runtime->anvil_dir / "data" / "anvil" / "build-cache.txt", actual);
        if (ok) {
            std::atomic_ref<const char*>(g_runtime->game.sha256).store(g_runtime->build->sha256);
            log_info("eu4.exe SHA-256 verified (" + fmt_ms(elapsed_ms() - begin) + ", after loading)");
        } else {
            log_warning("eu4.exe SHA-256 " + (actual.empty() ? std::string("could not be computed") : actual) +
                        " differs from the known build; every change is still checked against expected bytes");
        }
    }
    return 0;
}

void startup_impl(const std::filesystem::path& game_dir) {
    auto runtime = std::make_unique<Runtime>();
    // Time 0 of every "at ... ms" in the log: AnvilEntry, before the game's
    // C runtime starts (the start-up budget of design §17 counts from here).
    QueryPerformanceFrequency(&runtime->frequency);
    QueryPerformanceCounter(&runtime->start);
    runtime->game_dir = game_dir;
    runtime->anvil_dir = game_dir / "anvil";
    g_runtime = runtime.release();  // lives for the process

    if (!std::filesystem::is_directory(g_runtime->anvil_dir)) return;  // not installed: pure proxy, no files
    Log::instance().open(g_runtime->anvil_dir / "anvil.log");
    log_info(std::string("Anvil ") + kAnvilVersion + " (plugin API " + std::to_string(kManifestApi) + ")");

    wchar_t disable[8] = {};
    if (GetEnvironmentVariableW(L"ANVIL_DISABLE", disable, 8) == 1 && disable[0] == L'1') {
        log_info("ANVIL_DISABLE=1: no plugins loaded");
        return;
    }
    const AnvilSettings settings = read_anvil_settings(g_runtime->anvil_dir / "anvil.toml");
    if (!settings.error.empty()) log_warning("anvil.toml ignored: " + settings.error);
    Log::instance().set_min_level(settings.log_level);
    if (!settings.enabled) {
        log_info("disabled in anvil.toml: no plugins loaded");
        return;
    }

    HMODULE exe = GetModuleHandleW(nullptr);
    wchar_t exePath[MAX_PATH * 2] = {};
    GetModuleFileNameW(exe, exePath, static_cast<DWORD>(std::size(exePath)));
    g_runtime->exe_path = exePath;
    const auto identity = read_pe_identity(exe);
    if (!identity) {
        log_error("cannot read the game executable header; no plugins loaded");
        return;
    }
    g_runtime->identity = *identity;
    g_runtime->build = find_known_build(*identity);
    g_runtime->changes = std::make_unique<ChangeRegistry>(identity->image_base, identity->size_of_image);
    g_runtime->context.changes = g_runtime->changes.get();
    g_runtime->context.bus = &g_runtime->bus;
    g_runtime->context.image = exe;
    if (settings.profile) {
        g_runtime->profile = true;
        g_runtime->changes->set_profile(true);
        log_info("profile = true: calls per change are counted and written here when the game exits");
    }
    g_runtime->changes->set_refusal_sink([](OwnerId owner, const std::string& message) {
        // Names both owners of a conflict; the host call adds its own line.
        // Owner ids are the indices of g_runtime->plugins (both in load order).
        const auto& plugins = g_runtime->plugins;
        Log::instance().write(ANVIL_LOG_WARNING, owner < plugins.size() ? plugins[owner]->manifest.id : "?", message);
    });
    g_runtime->changes->set_notice_sink([](OwnerId owner, const std::string& message) {
        // On the game's thread, e.g. the first mid hook per address that changes rip.
        const auto& plugins = g_runtime->plugins;
        Log::instance().write(ANVIL_LOG_INFO, owner < plugins.size() ? plugins[owner]->manifest.id : "?", message);
    });
    AnvilGameInfo& game = g_runtime->game;
    game.size = sizeof(AnvilGameInfo);
    game.build_id = g_runtime->build ? g_runtime->build->id : nullptr;
    game.image_base = identity->image_base;
    game.image_size = identity->size_of_image;
    game.sha256 = nullptr;  // set when the file hash is verified, after loading
    g_runtime->host = make_host(&game);
    // The game's readiness and its sessions come from anvil-game (design §4,
    // §13); the core only listens. Its subscriptions come first, so the
    // policy acts before any plugin hears of a session.
    g_runtime->policy = std::make_unique<GameplayPolicy>(
        *g_runtime->changes, g_runtime->bus, GameplayOptIns{settings.allow_ironman, settings.allow_multiplayer},
        [](AnvilLogLevel level, const std::string& id, const std::string& message) {
            Log::instance().write(level, id, message);
        });
    g_runtime->policy->subscribe();
    g_runtime->bus.subscribe(
        "anvil", ANVIL_GAME_EVENT_READY,
        [](const char*, const void*, void*) { log_info("game ready at " + fmt_ms(elapsed_ms())); }, nullptr);
    char header[160];
    std::snprintf(header, sizeof(header), "timestamp 0x%08X, image 0x%X, checksum 0x%08X, entry 0x%X",
                  identity->pe_timestamp, identity->size_of_image, identity->pe_checksum, identity->entry_point_rva);
    log_info(std::string("game build: ") + (game.build_id ? game.build_id : "unknown") + " (" + header + ")");

    // Discovery: anvil/plugins/<folder>/plugin.toml
    ResolveInput input;
    input.host_api = kManifestApi;
    input.host_version = Version::parse(kAnvilVersion);
    input.game_build = game.build_id;
    input.disabled = settings.disabled_plugins;
    // symbols.toml: its build and every symbol are checked here, before any
    // plugin is loaded. The image is read through the registry, so bytes a
    // plugin has already changed read in their original form (design §7).
    // The verified symbols are kept per plugin folder for address_of and the
    // symbol change calls.
    std::map<std::wstring, PluginSymbols> verified_symbols;
    // settings.toml: a plugin whose declarations do not parse is skipped, as
    // it could not read its settings (design §11).
    std::map<std::wstring, SettingsFile> declared_settings;
    input.verify = [&](const Manifest& m) {
        if (!m.symbols.empty()) {
            SymbolVerification verified;
            SymbolFile declared;
            std::string reason = check_plugin_symbols(m.folder / m.symbols, game.build_id, *g_runtime->changes,
                                                      identity->image_base, &verified, &declared);
            if (!reason.empty()) return reason;
            verified_symbols[m.folder.wstring()] = PluginSymbols(declared, verified, identity->image_base);
            log_info(m.id + ": " + std::to_string(verified.symbols.size()) + " symbol(s) verified");
        }
        if (!m.settings.empty()) {
            SettingsFileResult parsed = load_settings(m.folder / m.settings);
            if (!parsed.file) return parsed.error;
            declared_settings[m.folder.wstring()] = std::move(*parsed.file);
        }
        return std::string();
    };
    const auto plugins_dir = g_runtime->anvil_dir / "plugins";
    const std::vector<std::filesystem::path> folders = plugin_folders(plugins_dir);
    for (const auto& folder : folders) {
        const auto parsed = load_manifest(folder);
        if (!parsed.manifest) {
            g_runtime->skipped.push_back({narrow(folder.filename().wstring()), parsed.error, folder});
            continue;
        }
        if (std::string reason = missing_entry_reason(*parsed.manifest); !reason.empty()) {
            g_runtime->skipped.push_back({parsed.manifest->id, std::move(reason), folder});
            continue;
        }
        input.manifests.push_back(*parsed.manifest);
    }
    log_info("discovered " + std::to_string(folders.size()) + " plugin folder(s) in " + narrow(plugins_dir.wstring()));
    ResolveResult resolved = resolve(input);
    g_runtime->skipped.insert(g_runtime->skipped.end(), resolved.skipped.begin(), resolved.skipped.end());
    for (const auto& s : g_runtime->skipped) log_warning(s.id + ": skipped — " + s.reason);
    log_info("resolved " + std::to_string(resolved.order.size()) + " plugin(s) to load at " + fmt_ms(elapsed_ms()));
    for (auto& m : resolved.order) {
        auto plugin = std::make_unique<AnvilPlugin>();
        plugin->context = &g_runtime->context;
        // Owners in load order: the earlier plugin is outermost in every chain.
        plugin->owner = g_runtime->changes->add_owner(m.id);
        if (const auto it = verified_symbols.find(m.folder.wstring()); it != verified_symbols.end()) {
            plugin->symbols = std::move(it->second);
        }
        plugin->plugin_dir = m.folder.wstring();
        plugin->data_dir = (g_runtime->anvil_dir / "data" / m.id).wstring();
        // The player's values and the ANVIL_<ID>_<NAME> overrides, before any plugin loads.
        if (const auto it = declared_settings.find(m.folder.wstring()); it != declared_settings.end()) {
            const auto notes = plugin->settings.init(std::move(it->second),
                                                     std::filesystem::path(plugin->data_dir) / "settings.toml", m.id);
            for (const auto& note : notes) Log::instance().write(note.level, m.id, note.message);
        }
        plugin->manifest = std::move(m);
        g_runtime->policy->add(plugin->manifest, plugin->owner);
        g_runtime->plugins.push_back(std::move(plugin));
    }

    // Early plugins: here, before the game's runtime starts.
    for (auto& p : g_runtime->plugins) {
        if (p->manifest.phase == Phase::Early) load_plugin(*p);
    }
    log_info("early phase done at " + fmt_ms(elapsed_ms()));

    if (HANDLE t = CreateThread(nullptr, 0, main_phase, nullptr, 0, nullptr)) {
        CloseHandle(t);
    } else {
        log_error("cannot start the main-phase thread; main plugins not loaded");
    }
}

// The call counters (profile = true), one line per change under its plugin.
void write_call_counts() {
    std::vector<CallCount> counts;
    if (!g_runtime->changes->call_counts(counts, false)) {
        Log::instance().try_write(ANVIL_LOG_WARNING, "anvil", "call counters: the registry was busy at exit; not written");
        return;
    }
    if (!Log::instance().try_write(ANVIL_LOG_INFO, "anvil",
                                   "call counters at exit (profile = true): " + std::to_string(counts.size()) +
                                       " counted change(s)")) {
        return;
    }
    const auto& plugins = g_runtime->plugins;
    for (const CallCount& c : counts) {
        const std::string& id = c.owner < plugins.size() ? plugins[c.owner]->manifest.id : std::string("?");
        if (!Log::instance().try_write(ANVIL_LOG_INFO, id, "calls: " + describe_call_count(c))) return;
    }
}

}  // namespace

PluginSettings* plugin_settings(std::string_view plugin_id) {
    if (g_runtime == nullptr) return nullptr;
    for (const auto& p : g_runtime->plugins) {
        if (p->manifest.id == plugin_id) return &p->settings;
    }
    return nullptr;
}

void shutdown() {
    try {
        if (g_runtime != nullptr && g_runtime->changes && g_runtime->profile) write_call_counts();
    } catch (...) {
        // Never throw out of DllMain.
    }
}

void startup(const std::filesystem::path& game_dir) {
    try {
        startup_impl(game_dir);
    } catch (const std::exception& e) {
        log_error(std::string("startup aborted: ") + e.what());
    } catch (...) {
        log_error("startup aborted by an unknown exception");
    }
}

}  // namespace anvil
