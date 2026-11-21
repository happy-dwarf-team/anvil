#include "runtime.hpp"

#include "benchmark.hpp"
#include "benchmark_record.hpp"
#include "discovery.hpp"
#include "files.hpp"
#include "first_opens.hpp"
#include "mod_plugins.hpp"
#include "plugin_hash.hpp"
#include "game_build.hpp"
#include "gameplay_policy.hpp"
#include "host.hpp"
#include "last_start.hpp"
#include "log.hpp"
#include "manifest.hpp"
#include "platform.hpp"
#include "plugin_inventory.hpp"
#include "plugin_set.hpp"
#include "changes.hpp"
#include "control.hpp"
#include "crash.hpp"
#include "crash_history.hpp"
#include "plugin_symbols.hpp"
#include "resolver.hpp"
#include "retired_move.hpp"
#include "safe_mode.hpp"
#include "seh_guard.hpp"
#include "start_plan.hpp"
#include "services.hpp"
#include "plugin_translation.hpp"
#include "settings.hpp"
#include "symbol_check.hpp"
#include "trust.hpp"

#include <anvil/anvil_game.h>
#include <anvil/anvil_plugin.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

extern "C" IMAGE_DOS_HEADER __ImageBase;  // version.dll

namespace anvil {
namespace {

struct Runtime {
    std::filesystem::path game_dir;
    std::filesystem::path anvil_dir;
    std::filesystem::path exe_path;
    std::string exe_name;  // e.g. eu4.exe, for crash reports
    std::string last_start;  // data/anvil/last-start.toml as this start wrote it; "" when it could not
    PeIdentity identity;
    const KnownBuild* build = nullptr;
    AnvilGameInfo game{};
    std::unique_ptr<ChangeRegistry> changes;
    bool profile = false;  // anvil.toml: count calls per change
    ServiceBus bus;
    std::unique_ptr<GameplayPolicy> policy;  // design §13, fed by anvil-game's session event
    HostContext context;  // what every plugin's host calls act on
    AnvilHost host{};
    std::unique_ptr<PluginSet> plugins;  // every plugin of this start, once resolved
    StartMarker start_marker;  // safe mode: starts that ended before game_ready
    ModDiscovery mods;  // plugins in the playset's mods (design §14), as the plan decided them
    // What this start found (ADR 0014): anvil.control plans the next start from it.
    std::unique_ptr<PluginInventory> inventory;
    std::unique_ptr<Control> control;  // anvil.control, the overlay's view of the core (design §15)
    std::unique_ptr<Benchmark> benchmark;  // anvil.benchmark (docs/spec/benchmark-v1.md §5)
    LARGE_INTEGER start{};
    LARGE_INTEGER frequency{};
    uint64_t entry_filetime = 0;  // the system time at `start`
    uint64_t entry_tick_ms = 0;   // GetTickCount64 at `start`
    DWORD entry_tick = 0;         // GetTickCount at `start`, the clock of GetLastInputInfo

    // The start-up benchmark (docs/spec/benchmark-v1.md §2, §6); `arm` is set
    // only in a benchmark start.
    std::optional<BenchmarkArm> arm;
    FirstOpens first_opens;
    bool first_opens_metered = false;
    // The previous start's history, read and replaced by the main phase.
    std::optional<StartHistory> previous_start;
    std::atomic<bool> history_read{false};
    LARGE_INTEGER ready{};             // anvil-game.ready
    std::optional<bool> input_during_start;
    std::optional<bool> first_start_since_boot, definitions_changed;  // computed for the ready record
    std::atomic<bool> plugins_loaded{false};  // the main phase has processed every plugin
};

Runtime* g_runtime = nullptr;

// Whose change a registry message is about: a plugin's id, a module's key,
// or the core's.
std::string owner_name(OwnerId owner) {
    const AnvilPlugin* h = g_runtime->plugins ? g_runtime->plugins->handle_of(owner) : nullptr;
    return h != nullptr ? h->name : "anvil";
}

double elapsed_ms() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - g_runtime->start.QuadPart) * 1000.0 /
           static_cast<double>(g_runtime->frequency.QuadPart ? g_runtime->frequency.QuadPart : 1);
}

// The ms from the entry point to `at` (a QueryPerformanceCounter value).
double ms_since_entry(LARGE_INTEGER at) {
    return static_cast<double>(at.QuadPart - g_runtime->start.QuadPart) * 1000.0 /
           static_cast<double>(g_runtime->frequency.QuadPart ? g_runtime->frequency.QuadPart : 1);
}

std::string fmt_ms(double ms) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f ms", ms);
    return buffer;
}

// ---- the start-up benchmark's record (docs/spec/benchmark-v1.md §6) ------------

std::filesystem::path benchmark_record_file() {
    return g_runtime->anvil_dir / "data" / "anvil" / "benchmark-start.json";
}

// What the record says now.
BenchmarkRecord benchmark_record(const char* write) {
    BenchmarkRecord r;
    r.write = write;
    r.anvil_version = kAnvilVersion;
    r.arm = *g_runtime->arm;
    r.process_id = GetCurrentProcessId();
    r.qpc_frequency = g_runtime->frequency.QuadPart;
    r.entry_qpc = g_runtime->start.QuadPart;
    r.ready_qpc = g_runtime->ready.QuadPart;
    r.entry_filetime = g_runtime->entry_filetime;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        r.creation_filetime = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    }
    // The plugin set, the same list anvil.control reads.
    r.plugins = record_plugins(*g_runtime->plugins, g_runtime->benchmark->participants());
    if (g_runtime->first_opens_metered) r.game_first_opens = g_runtime->first_opens.total(r.qpc_frequency);
    r.first_start_since_boot = g_runtime->first_start_since_boot;
    r.definitions_changed = g_runtime->definitions_changed;
    r.input_during_start = g_runtime->input_during_start;
    return r;
}

bool write_benchmark_record(const BenchmarkRecord& record, std::string& error) {
    return write_file_atomic(benchmark_record_file(), benchmark_record_json(record), error);
}

// Keyboard or mouse input since the entry point (GetLastInputInfo, no
// elevation); nullopt when it cannot be asked.
std::optional<bool> input_since_entry() {
    using Fn = BOOL(WINAPI*)(PLASTINPUTINFO);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const auto fn = user32 != nullptr ? reinterpret_cast<Fn>(GetProcAddress(user32, "GetLastInputInfo")) : nullptr;
    LASTINPUTINFO info{sizeof(info), 0};
    if (fn == nullptr || !fn(&info)) return std::nullopt;
    return static_cast<int32_t>(info.dwTime - g_runtime->entry_tick) > 0;  // 32-bit ticks wrap
}

// After ready: waits until every plugin is loaded and every participant has
// called done(), at most ANVIL_BENCHMARK_QUIT_LIMIT_MS after ready; then the
// end record and ANVIL_BENCHMARK_EVENT_QUIT, on which anvil-game quits the
// game (docs/spec/benchmark-v1.md §2).
void quit_when_done() {
    const double ready_ms = ms_since_entry(g_runtime->ready);
    std::vector<std::string> waiting;
    for (;;) {
        waiting.clear();
        if (!g_runtime->plugins_loaded.load(std::memory_order_acquire)) waiting.push_back("(plugins still loading)");
        for (const auto& p : g_runtime->benchmark->participants()) {
            if (!p.done) waiting.push_back(p.id);
        }
        if (waiting.empty() || elapsed_ms() - ready_ms >= ANVIL_BENCHMARK_QUIT_LIMIT_MS) break;
        Sleep(20);
    }
    // The last write: the game ends its process without Anvil's exit handler
    // running (two lab starts), so nothing is written at exit.
    if (std::string error; !write_benchmark_record(benchmark_record("end"), error)) {
        log_warning("benchmark: cannot write the start record: " + error);
    }
    const std::string after = fmt_ms(elapsed_ms() - ready_ms) + " after ready";
    if (waiting.empty()) {
        log_info("benchmark: every participant is done " + after + "; quitting");
    } else {
        std::string list;
        for (const auto& id : waiting) list += (list.empty() ? "" : ", ") + id;
        log_warning("benchmark: still not done " + after + ": " + list + "; quitting anyway");
    }
    g_runtime->bus.publish(ANVIL_BENCHMARK_EVENT_QUIT, nullptr);
}

// The ready record, off the game's main thread: the labels need the event log.
// Then the wait for the participants, on the same thread.
DWORD WINAPI write_ready_record(LPVOID) {
    try {
        if (g_runtime->history_read.load(std::memory_order_acquire) && g_runtime->previous_start) {
            const StartHistory& previous = *g_runtime->previous_start;
            const StartHistory now = current_start_history(g_runtime->entry_filetime, g_runtime->entry_tick_ms);
            g_runtime->first_start_since_boot = rebooted_since(previous, now);
            if (const auto update = newest_definitions_update()) {
                g_runtime->definitions_changed = *update > previous.entry_filetime;
            }
        }
        std::string error;
        if (write_benchmark_record(benchmark_record("ready"), error)) {
            log_info("benchmark: start record written at ready (" + fmt_ms(elapsed_ms()) + ")");
        } else {
            log_warning("benchmark: cannot write the start record: " + error);
        }
    } catch (const std::exception& e) {
        log_warning(std::string("benchmark: cannot write the start record: ") + e.what());
    }
    try {
        quit_when_done();
    } catch (const std::exception& e) {
        log_warning(std::string("benchmark: ") + e.what() + "; quitting now");
        g_runtime->bus.publish(ANVIL_BENCHMARK_EVENT_QUIT, nullptr);
    }
    return 0;
}

// anvil-game.ready in a benchmark start: the time, the end of the first
// opens and of the input check, then the record.
void benchmark_ready() {
    QueryPerformanceCounter(&g_runtime->ready);
    g_runtime->first_opens.stop();
    g_runtime->input_during_start = input_since_entry();
    if (HANDLE t = CreateThread(nullptr, 0, write_ready_record, nullptr, 0, nullptr)) {
        CloseHandle(t);
    } else {
        log_warning("benchmark: cannot start the thread that writes the start record");
    }
}

// The game is ready (anvil-game.ready): logged, and this start no longer
// counts for safe mode, unless it already wrote a crash report. Without
// anvil-game there is no such event.
void on_game_ready(const char*, const void*, void*) {
    if (g_runtime->arm) benchmark_ready();
    log_info("game ready at " + fmt_ms(elapsed_ms()));
    const bool reported = crash::reported();
    if (reported && g_runtime->start_marker.active()) {
        log_warning("the game is ready after a crash report; this start still counts for safe mode");
    }
    g_runtime->start_marker.game_ready(reported);
    crash::set_game_ready();
    std::string by;
    if (!crash::check_filter(by)) {
        log_warning("the unhandled-exception filter was replaced by " + by +
                    "; Anvil's crash reports depend on that filter calling the one it replaced");
    }
}

// ---- loading ---------------------------------------------------------------

// A phase of the plugin set, then what the runtime keeps besides: the
// benchmark forgets the plugins that failed.
void load_phase(Phase phase) {
    g_runtime->plugins->load(phase);
    for (const PluginRecord& p : *g_runtime->plugins) {
        const Outcome* o = p.outcome();
        if (p.handle() != nullptr && p.manifest().phase == phase && o != nullptr && o->kind == Outcome::Kind::failed) {
            g_runtime->benchmark->forget(p.handle());
        }
    }
}

void write_summary() {
    const PluginSet& plugins = *g_runtime->plugins;
    log_info("status: " + std::to_string(plugins.count(Outcome::Kind::loaded)) + " loaded, " +
             std::to_string(plugins.count(Outcome::Kind::failed)) + " failed, " +
             std::to_string(plugins.size() - plugins.resolved_count()) + " skipped");
    for (const auto& s : g_runtime->bus.describe()) log_info("service " + s);
}

DWORD WINAPI main_phase(LPVOID) {
    // Every start replaces the previous start's history, so the first
    // benchmark start after a normal one is not taken for the first since
    // boot (docs/spec/benchmark-v1.md §3). Off the game's main thread.
    const std::filesystem::path history = g_runtime->anvil_dir / "data" / "anvil" / "last-start.txt";
    g_runtime->previous_start = read_start_history(history);
    g_runtime->history_read.store(true, std::memory_order_release);
    load_phase(Phase::Main);
    // The end of the core start-up of design §17 (entry point -> end of loading).
    log_info("plugins loaded at " + fmt_ms(elapsed_ms()));
    g_runtime->plugins_loaded.store(true, std::memory_order_release);
    g_runtime->control->loading_done();
    g_runtime->bus.publish(ANVIL_EVENT_PLUGINS_LOADED, nullptr);
    if (std::string error; !write_file_atomic(
            history, start_history_text(current_start_history(g_runtime->entry_filetime, g_runtime->entry_tick_ms)),
            error)) {
        log_warning("cannot write " + narrow(history.wstring()) + ": " + error);
    }
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

using ExitProcessFn = void(WINAPI*)(UINT);
void* g_exit_next = nullptr;  // the rest of the chain, written by the registry

// The game's import of ExitProcess: its C runtime calls it when main returns
// (the menu's Exit, SDL_QUIT). The exit work runs here, while every thread
// still runs: ExitProcess then stops the other threads and detaches the DLLs,
// and a DLL that detaches before Anvil can end the process there (in the lab,
// a graphics driver behind the Epic overlay's detach; anvil#164).
void WINAPI exit_process(UINT code) {
    shutdown();
    reinterpret_cast<ExitProcessFn>(g_exit_next)(code);
}

void startup_impl(const std::filesystem::path& game_dir) {
    auto runtime = std::make_unique<Runtime>();
    // Time 0 of every "at ... ms" in the log: AnvilEntry, before the game's
    // C runtime starts (the start-up budget of design §17 counts from here).
    QueryPerformanceFrequency(&runtime->frequency);
    QueryPerformanceCounter(&runtime->start);
    // The same moment on the system clock and the tick counters, for the
    // start-up benchmark's record and labels (docs/spec/benchmark-v1.md §6).
    FILETIME entry{};
    GetSystemTimePreciseAsFileTime(&entry);
    runtime->entry_filetime = (static_cast<uint64_t>(entry.dwHighDateTime) << 32) | entry.dwLowDateTime;
    runtime->entry_tick_ms = GetTickCount64();
    runtime->entry_tick = GetTickCount();
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
    if (!settings.error.empty()) {
        // Fail safe (K3, #537): the lists are unknown, so a plugin the player
        // turned off would load again. Nothing loads and nothing is written,
        // last-start.toml included, so the Manager still shows the last start.
        log_error("anvil.toml cannot be read (line " + std::to_string(settings.line) + "): " + settings.error +
                  "; no plugins loaded until it is fixed");
        return;
    }
    Log::instance().set_min_level(settings.log_level);
    if (!settings.enabled) {
        log_info("disabled in anvil.toml: no plugins loaded");
        return;
    }

    // A benchmark start (docs/spec/benchmark-v1.md §2): the arm is in the
    // environment the Manager started the game with; anvil.toml is never
    // written for it.
    std::optional<BenchmarkArm>& arm = g_runtime->arm;
    {
        wchar_t text[96] = {};
        const DWORD n = GetEnvironmentVariableW(kBenchmarkVariable, text, static_cast<DWORD>(std::size(text)));
        if (n > 0) {
            std::string error;
            const std::string value = n < std::size(text) ? narrow(text) : std::string("(too long)");
            arm = n < std::size(text) ? parse_benchmark_arm(value, error) : std::nullopt;
            if (arm) {
                log_info("benchmark start: arm " + arm->text());
                // A start that never reaches ready leaves no record, not the last one's.
                std::error_code ignored;
                std::filesystem::remove(benchmark_record_file(), ignored);
            } else {
                log_warning("ANVIL_BENCHMARK=" + value + " ignored, so this is not a benchmark start: " +
                            (error.empty() ? std::string("too long") : error));
            }
        }
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
        Log::instance().write(ANVIL_LOG_WARNING, owner_name(owner), message);
    });
    g_runtime->changes->set_notice_sink([](OwnerId owner, const std::string& message) {
        // On the game's thread, e.g. the first mid hook per address that changes rip.
        Log::instance().write(ANVIL_LOG_INFO, owner_name(owner), message);
    });

    // Safe mode (design §10): counted before anything can crash.
    const StartRecord record =
        g_runtime->start_marker.begin(g_runtime->anvil_dir / "data" / "anvil" / "unfinished-starts.txt");
    if (record.safe_mode) {
        log_warning("safe mode: the last " + std::to_string(record.unfinished) +
                    " starts ended before the game was ready (a crash, a hang or a killed process); no plugins "
                    "are loaded on this start, the next one loads them again (reports: anvil/crashes)");
    } else if (record.unfinished > 0) {
        log_warning(std::to_string(record.unfinished) + " previous start(s) ended before the game was ready; safe mode "
                    "after " + std::to_string(kSafeModeAfter));
    }
    // The newest crash report of an earlier start, for the overlay; and in
    // safe mode the reports of the starts that led to it: a standard plugin
    // the crash was in is left out, so the safe start cannot end the same way.
    const std::filesystem::path crash_dir = g_runtime->anvil_dir / "crashes";
    const std::vector<std::string> crash_names = crash_report_names(crash_dir);
    std::vector<CrashSummary> safe_mode_reports;
    if (record.safe_mode) safe_mode_reports = safe_mode_crashes(crash_dir, record.unfinished);
    // Crash capture (design §12): the vectored handler, the filter ahead of
    // the game's and the helper thread, before any plugin code runs.
    crash::Setup capture;
    capture.crash_dir = crash_dir;
    capture.log_file = g_runtime->anvil_dir / "anvil.log";
    capture.image = exe;
    capture.sources.changes = g_runtime->changes.get();
    capture.sources.image_base = identity->image_base;
    capture.sources.image_size = identity->size_of_image;
    g_runtime->exe_name = narrow(g_runtime->exe_path.filename().wstring());
    capture.sources.image_name = g_runtime->exe_name.c_str();
    capture.sources.build_id = g_runtime->build ? g_runtime->build->id : nullptr;
    capture.sources.anvil_base = reinterpret_cast<uintptr_t>(&__ImageBase);
    capture.anvil_version = kAnvilVersion;
    capture.start = g_runtime->start;
    capture.unfinished_starts = record.unfinished;
    capture.safe_mode = record.safe_mode;
    if (std::string error; !crash::install(capture, error)) log_warning("crash capture: " + error);
    if (const ULONG guarantee = crash::thread_guarantee(); guarantee != 0) {
        log_info("crash capture: the CPU state saved with an exception does not fit in a thread's default stack "
                 "guarantee (AMX); new threads with 256 KB of stack or more get " +
                 std::to_string(guarantee / 1024) + " KB");
    }
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
    g_runtime->bus.subscribe("anvil", ANVIL_GAME_EVENT_READY, on_game_ready, nullptr);
    char header[160];
    std::snprintf(header, sizeof(header), "timestamp 0x%08X, image 0x%X, checksum 0x%08X, entry 0x%X",
                  identity->pe_timestamp, identity->size_of_image, identity->pe_checksum, identity->entry_point_rva);
    log_info(std::string("game build: ") + (game.build_id ? game.build_id : "unknown") + " (" + header + ")");
    // For reports from Linux: the Wine and the Direct3D 9 the game runs on
    // (eu4.exe imports d3d9.dll, so it is loaded by now). Log only.
    log_info(wine_log_line(detect_wine()));
    log_info(d3d9_log_line(GetModuleHandleW(L"d3d9.dll"), game_dir));
    const i18n::Message unknown_build = game.build_id ? i18n::Message() : unknown_build_reason(game_dir);
    if (unknown_build.code == "build_unknown_gog") log_warning(i18n::english(unknown_build));

    // The plugin inventory (ADR 0014): every plugin of anvil/plugins and of
    // the playset's mods with its check against the game. The image is read
    // through the registry, so bytes a plugin has already changed read in
    // their original form (design §7). The game's own choice of user folder:
    // -userdir first (a private profile, as Etch's live game has).
    // A -userdir folder that does not exist yet is this start's, with an empty playset (T2, #542).
    const StartUserDir start_dir = start_user_dir(game_dir, GetCommandLineW());
    const std::filesystem::path user_dir = start_dir.dir;
    if (!start_dir.line.empty()) log_info(start_dir.line);
    {
        const OriginalImageReader image(*g_runtime->changes, identity->image_base);
        g_runtime->inventory = std::make_unique<PluginInventory>(
            take_inventory(g_runtime->anvil_dir, user_dir, game_image(image, game.build_id, unknown_build)));
    }
    const PluginInventory& inventory = *g_runtime->inventory;
    log_info("discovered " + std::to_string(inventory.local.size()) + " plugin folder(s) in " +
             narrow(inventory.plugins_dir.wstring()));
    for (const auto& w : inventory.mod_warnings) log_warning(w);

    // The start plan: what loads, in which order, with which modules, what
    // does not and why, and what this start writes first.
    const PlayerChoices choices = read_player_choices(inventory, settings);
    StartConditions conditions;
    conditions.safe_mode = record.safe_mode;
    conditions.crashes = safe_mode_reports;
    conditions.benchmark = arm;
    conditions.host_version = Version::parse(kAnvilVersion);
    const StartPlan plan = plan_start(inventory, choices, conditions);
    for (const auto& w : plan.warnings) log_warning(w);
    g_runtime->mods.playset = inventory.playset;
    g_runtime->mods.trust = choices.trust;
    g_runtime->mods.plugins = plan.mods;
    g_runtime->mods.warnings = inventory.mod_warnings;
    g_runtime->mods.warnings.insert(g_runtime->mods.warnings.end(), plan.warnings.begin(), plan.warnings.end());
    const auto label_of = [](const ModPlugin& p) {
        return p.id + " from mod '" + p.mod.name + "' (" +
               (p.mod.workshop_id.empty() ? std::string("local mod") : "Workshop " + p.mod.workshop_id) + ")";
    };

    // The plan's writes, before any plugin loads: the approved copies
    // trust.toml no longer names, the copies to make, the retired settings.
    for (const std::filesystem::path& gone : plan.writes.remove) remove_tree(gone);
    std::map<int, std::string> copy_errors;  // by mod plugin
    for (const PlannedCopy& c : plan.writes.copies) {
        const std::string label = label_of(plan.mods[static_cast<size_t>(c.mod)]);
        if (std::string error; !copy_approved(g_runtime->anvil_dir, c.id, c.from, c.hash, error)) {
            log_warning(label + ": cannot make its approved copy: " + error);
            copy_errors[c.mod] = error;
        } else {
            log_info(label + ": made the approved copy");
        }
    }
    // A retired value whose player's file cannot be written once anvil.toml has its move
    // goes into the record of moved retired settings, so the next start does not move it
    // again over a switch the player makes meanwhile; it leaves once its file lost it.
    MovedRetiredRecord moved_retired = choices.moved_retired;
    for (const PlannedRetiredMove& m : plan.writes.retired) {
        for (const RetiredMoveNote& note : carry_out_retired_move(m.move, m.player_file, g_runtime->anvil_dir / "anvil.toml",
                                                                  nullptr, &moved_retired[m.plugin])) {
            Log::instance().write(note.level, m.plugin, note.message);
        }
    }
    for (const std::string& id : plan.writes.forget_moved) moved_retired.erase(id);
    if (moved_retired_text(moved_retired) != moved_retired_text(choices.moved_retired)) {
        if (std::string error; !write_moved_retired(moved_retired_file(g_runtime->anvil_dir), moved_retired, error)) {
            log_warning("cannot write data/anvil/retired-moved.toml: " + error +
                        "; the next start moves the retired values still in a player's settings.toml again");
        }
    }
    for (const auto& [id, note] : plan.notes) Log::instance().write(note.level, id, note.message);

    std::map<std::string, std::vector<std::string>> shadowed;  // id -> the mods' names
    for (const ModPlugin& p : plan.mods) {
        if (p.state == ModPluginState::Shadowed) {
            shadowed[p.id].push_back(p.mod.name);
        } else if (p.loads() && !plan.safe_mode) {
            log_info(label_of(p) + ": " + p.reason + "; loads from " + narrow(p.load_folder.wstring()));
        }
    }
    for (const auto& [id, names] : shadowed) {
        std::string list;
        for (const auto& n : names) list += (list.empty() ? "'" : ", '") + n + "'";
        log_info(id + ": the local plugin in anvil/plugins wins over the same id in mod(s) " + list);
    }
    if (!plan.mods.empty() || !inventory.playset.mods.empty()) {
        log_info("found " + std::to_string(plan.mods.size()) + " plugin(s) in " +
                 std::to_string(inventory.playset.mods.size()) + " enabled mod(s) at " + fmt_ms(elapsed_ms()));
    }
    for (const PlannedPlugin& s : plan.skipped) log_warning(s.id + ": skipped — " + s.reason);
    log_info("resolved " + std::to_string(plan.load.size()) + " plugin(s) to load at " + fmt_ms(elapsed_ms()));
    // The overlay speaks the language of EU4's settings, or with Mei, which
    // shows a translation mod's Chinese, Japanese or Korean, Steam's or
    // Windows' (#158).
    const bool cjk_text = std::any_of(plan.load.begin(), plan.load.end(),
                                      [](const PlannedPlugin& p) { return p.id == i18n::kCjkTextPlugin; });
    const i18n::LanguageChoice language = i18n::choose_language(user_dir, cjk_text);
    const std::string player_language = i18n::language_code(language.language);
    log_info("the player's language: " + player_language + " (" + language.source + ")");
    // The plugin set: what the plan says of each plugin, in load order, then
    // the skipped ones. It adds the owners (the core's own last) and the
    // gameplay policy entries, before any plugin's code runs.
    std::vector<ResolvedPlugin> to_load;
    to_load.reserve(plan.load.size());
    for (const PlannedPlugin& p : plan.load) {
        ResolvedPlugin r;
        const PluginCheck* check = check_of(inventory, plan, p);
        if (check != nullptr && check->symbols && check->verified) {
            SymbolFile declared = *check->symbols;
            adopt_verified_bytes(declared, check->verification);
            r.symbols = PluginSymbols(declared, check->verification, identity->image_base);
            log_info(p.id + ": " + std::to_string(check->verification.symbols.size()) + " symbol(s) verified");
        }
        if (check != nullptr && check->settings_fit) r.settings = check->settings;
        r.approved_hash = p.approved_hash;
        r.data_dir = g_runtime->anvil_dir / "data" / p.id;
        r.modules_on = p.modules_on;
        if (const auto failed = copy_errors.find(p.mod); failed != copy_errors.end()) {
            r.cannot_load = i18n::Message("copy_failed", {failed->second});
        }
        r.manifest = *p.manifest;
        to_load.push_back(std::move(r));
    }
    // A local plugin that is off at this start keeps what its plugin.toml and
    // settings.toml declare, so anvil.control shows its modules, settings and
    // dependencies (ADR 0013).
    std::vector<Skipped> skipped;
    skipped.reserve(plan.skipped.size());
    for (const PlannedPlugin& p : plan.skipped) {
        Skipped s;
        s.id = p.id;
        s.reason = p.reason;
        s.why = p.why;
        s.folder = p.folder;
        s.disabled = p.disabled;
        s.benchmark_arm = p.benchmark_arm;
        s.mod = p.mod_name;
        s.awaiting_approval = p.awaiting_approval;
        s.kind = p.kind;
        if (p.local >= 0 && p.manifest) {
            s.manifest = p.manifest;
            s.data_dir = g_runtime->anvil_dir / "data" / p.id;
            const PluginCheck& check = inventory.local[static_cast<size_t>(p.local)].check;
            if (check.settings_fit) s.settings = check.settings;
        }
        skipped.push_back(std::move(s));
    }
    // The opt-ins as the retired settings left them, before the plugin set
    // gives the policy its plugins.
    g_runtime->policy->set_opt_ins(GameplayOptIns{plan.lists.allow_ironman, plan.lists.allow_multiplayer});
    PluginSet::Dependencies deps;
    deps.context = &g_runtime->context;
    deps.policy = g_runtime->policy.get();
    deps.host = &g_runtime->host;
    g_runtime->plugins = std::make_unique<PluginSet>(std::move(to_load), std::move(skipped), std::move(deps));
    // The plugin set has its own symbols now; the inventory keeps only what
    // the next start's plan reads (#461).
    release_symbols(*g_runtime->inventory);
    crash::attach_plugins(g_runtime->plugins.get());
    // What this start used (ADR 0013, point 8), now that the retired settings
    // moved and every plugin's settings were read: the Manager marks what
    // changes at the next start from it.
    {
        LastStart last;
        last.anvil_version = kAnvilVersion;
        last.written = utc_now();
        last.safe_mode = record.safe_mode;
        last.disabled_plugins = plan.lists.disabled_plugins;
        last.allow_ironman = plan.lists.allow_ironman;
        last.allow_multiplayer = plan.lists.allow_multiplayer;
        last.enabled_modules = plan.lists.enabled_modules;
        last.disabled_modules = plan.lists.disabled_modules;
        for (const TrustEntry& e : choices.trust.entries) last.approvals.push_back({e.id, e.mod, e.version, e.hash});
        for (size_t i = 0; i < g_runtime->plugins->resolved_count(); ++i) {
            const PluginRecord& p = g_runtime->plugins->at(i);
            const PluginSettings& values = p.settings();
            if (values.size() == 0) continue;
            auto& out = last.settings[p.id()];
            for (const Setting& s : values.schema().settings) {
                if (const auto v = values.saved(s.id)) out[s.name] = setting_toml(s, *v);
            }
        }
        g_runtime->last_start = last_start_text(last);
        std::string error;
        if (!replace_text_file(g_runtime->anvil_dir / "data" / "anvil" / "last-start.toml", g_runtime->last_start, error)) {
            log_warning("cannot write data/anvil/last-start.toml: " + error);
            g_runtime->last_start.clear();
        }
    }
    const OwnerId core = g_runtime->plugins->core_owner();
    if (const ChangeStatus s = g_runtime->changes->replace_import(core, exe, "KERNEL32.dll", "ExitProcess",
                                                                  reinterpret_cast<void*>(&exit_process), &g_exit_next);
        s != ChangeStatus::Ok) {
        log_warning(std::string("cannot replace the game's ExitProcess (") + result_name(to_result(s)) +
                    "); the exit work waits for DLL_PROCESS_DETACH, which a normal exit may skip");
    }
    // A benchmark start times the game's first opens of its files until
    // ready; the innermost link times the call itself.
    if (arm) {
        const ChangeStatus s = install_first_open_meter(*g_runtime->changes, core, exe, g_runtime->first_opens);
        g_runtime->first_opens_metered = s == ChangeStatus::Ok;
        if (!g_runtime->first_opens_metered) {
            log_warning(std::string("benchmark: cannot time the game's first opens (CreateFileW): ") +
                        result_name(to_result(s)));
        }
    }

    // anvil.control (src/control/anvil_control.h): what the overlay shows and
    // changes. Registered before any plugin loads; the overlay is early.
    ControlSources sources;
    sources.anvil_dir = g_runtime->anvil_dir;
    sources.anvil_version = kAnvilVersion;
    sources.game_build = game.build_id;
    sources.plugins = g_runtime->plugins.get();
    sources.policy = g_runtime->policy.get();
    sources.disabled = plan.lists.disabled_plugins;
    sources.allow_ironman = plan.lists.allow_ironman;
    sources.allow_multiplayer = plan.lists.allow_multiplayer;
    sources.enabled_modules = plan.lists.enabled_modules;
    sources.disabled_modules = plan.lists.disabled_modules;
    sources.inventory = g_runtime->inventory.get();
    sources.last_start = g_runtime->last_start;
    sources.safe_mode = record.safe_mode;
    sources.unfinished_starts = record.unfinished;
    if (!crash_names.empty()) sources.previous_crash = crash_names.front();
    sources.mods = &g_runtime->mods;
    sources.language = player_language;
    // The plugins' own texts in that language (design §5.4). A file that
    // does not parse is left out with a warning; the plugin loads as before.
    const auto translation = [&](const std::filesystem::path& folder, const std::string& id) {
        PluginTranslationResult t = load_plugin_translation(folder, player_language);
        if (!t.error.empty()) {
            log_warning(id + ": " + kTranslationsFolder + "/" + player_language + ".toml" +
                        (t.line != 0 ? ":" + std::to_string(t.line) : std::string()) + " ignored: " + t.error);
        }
        return t.translation;
    };
    if (player_language != "en") {
        // Also of a plugin that is off, whose plugin.toml parses (ADR 0013).
        for (const PluginRecord& p : *g_runtime->plugins) {
            if (!p.declared()) continue;
            const Manifest& m = p.manifest();
            if (auto t = translation(m.folder, m.id)) sources.translations.emplace(m.id, std::move(*t));
        }
        for (uint32_t i = 0; i < g_runtime->mods.plugins.size(); ++i) {
            const ModPlugin& p = g_runtime->mods.plugins[i];
            if (!p.manifest) continue;
            if (auto t = translation(p.folder, p.id)) sources.mod_translations.emplace(i, std::move(*t));
        }
    }
    g_runtime->control = std::make_unique<Control>(std::move(sources));
    if (g_runtime->bus.register_service("anvil", ANVIL_CONTROL_SERVICE, ANVIL_CONTROL_VERSION,
                                        g_runtime->control->service()) != ANVIL_OK) {
        log_warning("cannot register " ANVIL_CONTROL_SERVICE);
    }
    // anvil.benchmark: plugins declare their fields inside anvil_plugin_load,
    // so it is there before any plugin loads too.
    g_runtime->benchmark = std::make_unique<Benchmark>(arm.has_value(), [](const AnvilPlugin& plugin, const std::string& message) {
        Log::instance().write(ANVIL_LOG_WARNING, plugin.name, message);
    });
    if (g_runtime->bus.register_service("anvil", ANVIL_BENCHMARK_SERVICE, ANVIL_BENCHMARK_SERVICE_VERSION,
                                        g_runtime->benchmark->service()) != ANVIL_OK) {
        log_warning("cannot register " ANVIL_BENCHMARK_SERVICE);
    }

    // Early plugins: here, before the game's runtime starts.
    load_phase(Phase::Early);
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
    for (const CallCount& c : counts) {
        // Under the plugin, or the module, whose change it is.
        const AnvilPlugin* h = g_runtime->plugins ? g_runtime->plugins->handle_of(c.owner) : nullptr;
        if (!Log::instance().try_write(ANVIL_LOG_INFO, h != nullptr ? h->name : std::string("?"), "calls: " + describe_call_count(c))) {
            return;
        }
    }
}

}  // namespace

PluginSettings* plugin_settings(std::string_view plugin_id) {
    if (g_runtime == nullptr || !g_runtime->plugins) return nullptr;
    const PluginRecord* p = g_runtime->plugins->find(plugin_id);
    return p != nullptr && p->handle() != nullptr ? &p->settings() : nullptr;
}

void shutdown() {
    // Once: from the game's ExitProcess, and again from DLL_PROCESS_DETACH if it comes.
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    try {
        if (g_runtime != nullptr && g_runtime->changes && g_runtime->profile) write_call_counts();
        // Closed before the game was ready, without a crash: not counted.
        if (g_runtime != nullptr && g_runtime->start_marker.active() && !crash::crashed()) {
            g_runtime->start_marker.clean_exit();
        }
    } catch (...) {
        // Never throw out of ExitProcess or DllMain.
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
