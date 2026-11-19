// anvil-model-reload: the standard plugin that reloads what a model modder
// saved, in the running game (docs/model-reload.md; anvil#470).
//
// Off by default: with the `window` setting off there is no thread and nothing
// runs. On, one thread of the plugin looks at the enabled mods' files (first
// when it starts, then every two seconds while the window is shown) and lists
// the ones saved since the last reload. "Reload the changed files" runs one
// piece of main-thread work (anvil-game's run_on_main_thread) with the steps
// of plan(): the game's console for textures, animations, particles and
// entities, and the plugin's own reload of mesh types for .mesh and .gfx files
// (reload.hpp, game.hpp). It is refused in ironman and multiplayer.
//
// For the lab: ANVIL_MODEL_RELOAD_CONTROL names a file of one line, "<n> look"
// or "<n> reload"; a new <n> is a new command. Its answer goes to <file>.state.
#include <anvil/anvil.hpp>
#include <anvil/anvil_game.h>
#include <anvil/anvil_panel.h>

#include <windows.h>
#include <shlobj.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "settings.gen.hpp"
#include "symbols.gen.hpp"

#include "files.hpp"
#include "game.hpp"
#include "reload.hpp"
#include "report.hpp"
#include "texts.hpp"

namespace mr = model_reload;
namespace fs = std::filesystem;

namespace {

std::string utf8(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

const AnvilGameService* g_game = nullptr;
const AnvilPanelService* g_panels = nullptr;
AnvilPanel* g_panel = nullptr;  // the thread's
mr::GameAddresses g_addresses;
bool g_has_addresses = false;
fs::path g_game_dir;

std::atomic<bool> g_on{false};
std::atomic<bool> g_show{false};
std::atomic<bool> g_started{false};
std::atomic<bool> g_reload_asked{false};
std::atomic<bool> g_look_asked{false};

std::mutex g_wake_mutex;
std::condition_variable g_wake_cv;
bool g_wake = false;

// The state the thread, the main thread and the clicks share.
std::mutex g_mutex;
bool g_mods_read = false;
mr::Mods g_mods;
mr::Snapshot g_baseline;
mr::Snapshot g_current;
mr::PanelState g_state;
std::uint64_t g_control_seq = 0;  // the control command being answered, 0 for none
bool g_control_reload = false;    // that command is a reload: only the reload answers it

fs::path g_control;

void wake() {
    {
        std::lock_guard lock(g_wake_mutex);
        g_wake = true;
    }
    g_wake_cv.notify_one();
}

mr::Texts texts() {
    return mr::Texts{mr::lang_of(g_panels != nullptr && g_panels->language != nullptr ? g_panels->language() : "en")};
}

// EU4's user folder: anvil-game's, else -userdir= or Documents.
fs::path user_dir() {
    if (g_game != nullptr && ANVIL_GAME_HAS(g_game, user_dir) && g_game->user_dir != nullptr) {
        if (const wchar_t* dir = g_game->user_dir(); dir != nullptr && *dir != 0) return fs::path(dir);
    }
    const std::wstring line = GetCommandLineW();
    const std::wstring key = L"-userdir=";
    if (const std::size_t at = line.find(key); at != std::wstring::npos) {
        std::size_t from = at + key.size();
        const bool quoted = from < line.size() && line[from] == L'"';
        if (quoted) ++from;
        std::size_t end = line.find(quoted ? L'"' : L' ', from);
        if (end == std::wstring::npos) end = line.size();
        return fs::path(line.substr(from, end - from));
    }
    PWSTR docs = nullptr;
    fs::path out;
    if (SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs) == S_OK) {
        out = fs::path(docs) / L"Paradox Interactive" / L"Europa Universalis IV";
    }
    CoTaskMemFree(docs);
    return out;
}

// The session's kind, from anvil-game's session event (any thread may read it).
std::atomic<int> g_mode{static_cast<int>(mr::Mode::menu)};

mr::Mode mode_of(const AnvilGameSession& s) {
    if (s.multiplayer != 0) return mr::Mode::multiplayer;
    if (s.ironman != 0) return mr::Mode::ironman;
    return mr::Mode::game;
}

mr::Mode mode_now() { return static_cast<mr::Mode>(g_mode.load(std::memory_order_acquire)); }

// On the main thread: the session as of the last frame.
mr::Mode mode_on_main_thread() {
    AnvilGameSession s{};
    s.size = sizeof s;
    if (g_game == nullptr || g_game->session(&s) == 0) return mr::Mode::menu;
    return mode_of(s);
}

void on_session(const void* payload) {
    const auto* s = static_cast<const AnvilGameSession*>(payload);
    if (s == nullptr) return;
    g_mode.store(static_cast<int>(s->change == ANVIL_GAME_SESSION_ENDED ? mr::Mode::menu : mode_of(*s)),
                 std::memory_order_release);
    wake();
}

bool write_file(const fs::path& path, const std::string& text) {
    const fs::path tmp = path.native() + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out) return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// Under g_mutex.
void answer_control_locked(const std::string& text) {
    if (g_control.empty() || g_control_seq == 0) return;
    write_file(g_control.native() + L".state", std::to_string(g_control_seq) + " " + text);
    g_control_seq = 0;
}

std::string changed_text_locked() {
    std::string out = "changed " + std::to_string(g_state.changed.size()) + "\n";
    for (const mr::Change& c : g_state.changed) out += c.key.relative + " " + mr::action_of(c.kind) + "\n";
    return out;
}

// The thread: a look at the files. Not under g_mutex.
void look() {
    mr::Mods mods;
    bool read_mods = false;
    {
        std::lock_guard lock(g_mutex);
        read_mods = !g_mods_read;
    }
    if (read_mods) {
        mods = mr::enabled_mods(user_dir());
        const anvil::Host host;
        for (const std::string& p : mods.problems) host.warn("{}", p);
        std::string names;
        for (const mr::ModFolder& m : mods.folders) names += (names.empty() ? "" : ", ") + m.name;
        host.info("watching {} mods: {}", mods.folders.size(), names.empty() ? "none" : names);
        std::lock_guard lock(g_mutex);
        g_mods = mods;
        g_mods_read = true;
        g_state.mods.clear();
        for (const mr::ModFolder& m : mods.folders) g_state.mods.push_back(m.name);
        g_state.mod_problems = mods.problems.size();
    } else {
        std::lock_guard lock(g_mutex);
        mods = g_mods;
    }
    bool cut = false;
    mr::Snapshot now = mr::scan(mods.folders, mr::kScanLimit, &cut);
    std::lock_guard lock(g_mutex);
    if (!g_state.looked) {
        g_baseline = now;
        g_state.looked = true;
    }
    g_state.cut = cut;
    g_current = std::move(now);
    g_state.changed = mr::changes(g_baseline, g_current);
}

struct Work {
    mr::Snapshot snapshot;
    std::vector<mr::Step> steps;
    std::vector<mr::ModFolder> mods;
};

std::string now_text() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char out[16];
    std::snprintf(out, sizeof out, "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
    return out;
}

// On the main thread, after a frame.
void reload_work(void* context) {
    std::unique_ptr<Work> work(static_cast<Work*>(context));
    const anvil::Host host;
    try {
        const mr::Mode mode = mode_on_main_thread();
        if (mode != mr::Mode::game || !g_has_addresses) {
            std::lock_guard lock(g_mutex);
            g_state.running = false;
            g_state.mode = mode;
            answer_control_locked(!g_has_addresses ? "refused not available on this game build\n"
                                                    : "refused not in a single-player, non-ironman game\n");
            return;
        }
        mr::GameMemory game(g_addresses);
        mr::RunInput in;
        in.steps = work->steps;
        in.mods = work->mods;
        in.read_file = [&](const mr::Step& step, std::string& out) {
            if (step.mod >= work->mods.size()) return false;
            std::ifstream f(work->mods[step.mod].root / fs::path(std::u8string(step.path.begin(), step.path.end())),
                            std::ios::binary);
            if (!f) return false;
            std::ostringstream ss;
            ss << f.rdbuf();
            out = ss.str();
            return true;
        };
        in.file_exists = [&](std::string_view path) {
            const fs::path rel(std::u8string(path.begin(), path.end()));
            std::error_code ec;
            for (const mr::ModFolder& m : work->mods) {
                if (fs::is_regular_file(m.root / rel, ec)) return true;
            }
            return !g_game_dir.empty() && fs::is_regular_file(g_game_dir / rel, ec);
        };
        const auto start = std::chrono::steady_clock::now();
        mr::RunResult result = mr::run(game, in);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const std::vector<std::string> lines = mr::log_lines(result);
        host.info("reload of {} steps in {:.1f} s{}", work->steps.size(), seconds,
                  result.faulted ? ": an exception in the game's code stopped it" : "");
        for (const std::string& l : lines) host.info("  {}", l);
        std::lock_guard lock(g_mutex);
        g_baseline = std::move(work->snapshot);
        g_state.changed = mr::changes(g_baseline, g_current);
        g_state.running = false;
        g_state.has_last = true;
        g_state.last = std::move(result);
        g_state.last_time = now_text();
        std::string text = std::string(g_state.last.faulted ? "faulted" : "done") + "\n";
        for (const std::string& l : lines) text += l + "\n";
        answer_control_locked(text);
    } catch (...) {
        std::lock_guard lock(g_mutex);
        g_state.running = false;
        answer_control_locked("failed\n");
    }
    wake();
}

// The thread: a fresh look, then the work for the main thread.
void start_reload() {
    look();
    auto work = std::make_unique<Work>();
    {
        std::lock_guard lock(g_mutex);
        g_state.mode = mode_now();
        if (g_state.running || g_state.mode != mr::Mode::game) {
            if (!g_state.running) answer_control_locked("refused not in a single-player, non-ironman game\n");
            return;
        }
        if (g_state.changed.empty()) {
            answer_control_locked("done\nnothing changed\n");
            return;
        }
        work->snapshot = g_current;
        work->steps = mr::plan(g_state.changed, g_current);
        work->mods = g_mods.folders;
        g_state.running = true;
    }
    Work* raw = work.release();
    if (g_game->run_on_main_thread(&reload_work, raw) != ANVIL_OK) {
        delete raw;
        std::lock_guard lock(g_mutex);
        g_state.running = false;
        answer_control_locked("failed cannot queue the main-thread work\n");
    }
}

std::vector<mr::Row> g_published;

void publish() {
    std::vector<mr::Row> now;
    {
        std::lock_guard lock(g_mutex);
        now = mr::rows(g_state, texts());
    }
    auto same = [](const std::vector<mr::Row>& a, const std::vector<mr::Row>& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a[i].kind != b[i].kind || a[i].label != b[i].label || a[i].value != b[i].value || a[i].id != b[i].id ||
                a[i].has_value != b[i].has_value) {
                return false;
            }
        }
        return true;
    };
    if (same(now, g_published)) return;
    std::vector<AnvilPanelRow> out(now.size());
    for (std::size_t i = 0; i < now.size(); ++i) {
        out[i].kind = now[i].kind;
        out[i].label = now[i].kind == mr::kSeparator ? nullptr : now[i].label.c_str();
        out[i].value = now[i].has_value ? now[i].value.c_str() : nullptr;
        out[i].id = now[i].id;
    }
    if (g_panels->set_rows(g_panel, out.data(), static_cast<std::uint32_t>(out.size()), sizeof(AnvilPanelRow)) ==
        ANVIL_OK) {
        g_published = std::move(now);
    }
}

void click(std::uint64_t id) {
    if (id != mr::kReload) return;
    g_reload_asked.store(true);
    wake();
}

// The overlay calls this through a C pointer on the main thread, after a
// frame: no exception may leave it.
void on_click(void*, std::uint64_t id) {
    try {
        click(id);
    } catch (...) {
    }
}

void run() {
    auto last_look = std::chrono::steady_clock::now() - std::chrono::hours(1);
    for (;;) {
        {
            std::unique_lock lock(g_wake_mutex);
            g_wake_cv.wait_for(lock, std::chrono::seconds(1), [] { return g_wake; });
            g_wake = false;
        }
        try {
            const bool on = g_on.load(std::memory_order_acquire);
            bool shown = false;
            if (g_panels != nullptr) {
                if (!on) {
                    if (g_panel != nullptr && g_panels->shown(g_panel)) g_panels->show(g_panel, 0);
                } else {
                    if (g_panel == nullptr) {
                        if (g_panels->create(texts()("Model reload"), &g_panel) != ANVIL_OK) {
                            anvil::Host{}.warn("the overlay has no room for another panel");
                            g_panels = nullptr;
                        } else {
                            g_panels->on_click(g_panel, &on_click, nullptr);
                        }
                    }
                    if (g_panel != nullptr) {
                        if (g_show.exchange(false)) g_panels->show(g_panel, 1);
                        shown = g_panels->shown(g_panel) != 0;
                    }
                }
            }
            if (!on && g_control.empty()) continue;
            bool first = false;
            {
                std::lock_guard lock(g_mutex);
                first = !g_state.looked;
                g_state.mode = mode_now();
            }
            const auto now = std::chrono::steady_clock::now();
            if (g_reload_asked.exchange(false)) {
                start_reload();
                last_look = now;
            } else if (first || g_look_asked.exchange(false) || (shown && now - last_look >= std::chrono::seconds(2))) {
                look();
                last_look = now;
                std::lock_guard lock(g_mutex);
                if (g_control_seq != 0 && !g_control_reload) answer_control_locked(changed_text_locked());
            }
            if (shown) publish();
        } catch (...) {
            // A failed allocation or a file system error: the next wake-up tries again.
        }
    }
}

void start_thread() {
    if (g_started.exchange(true)) return;
    try {
        std::thread(run).detach();
    } catch (const std::exception&) {
        g_started.store(false);
        anvil::Host{}.warn("cannot start its thread");
    }
}

void control_loop() {
    std::string seen;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        FILE* f = nullptr;
        if (_wfopen_s(&f, g_control.c_str(), L"rb") != 0 || f == nullptr) continue;
        char buf[128] = {};
        const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        try {
            std::string text(buf, n);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
            if (text.empty() || text == seen) continue;
            seen = text;
            const std::uint64_t seq = std::strtoull(text.c_str(), nullptr, 10);
            const bool reload = text.find(" reload") != std::string::npos;
            {
                // Under the lock with the flag, so that a periodic look never
                // answers a reload command before the reload ran.
                std::lock_guard lock(g_mutex);
                g_control_seq = seq;
                g_control_reload = reload;
                (reload ? g_reload_asked : g_look_asked).store(true);
            }
            wake();
        } catch (...) {
        }
    }
}

struct ModelReload : anvil::Plugin {
    void load(anvil::Host& host) override {
        g_game = host.service<AnvilGameService>(ANVIL_GAME_SERVICE, ANVIL_GAME_SERVICE_VERSION);
        if (g_game == nullptr) {
            host.info("no anvil-game: model reload is not available");
            return;
        }
        host.subscribe(ANVIL_GAME_EVENT_SESSION, &on_session);
        try {
            const auto addr = [&](const auto& s) { return reinterpret_cast<std::uintptr_t>(host.address(s)); };
            g_addresses.graphics = reinterpret_cast<std::uintptr_t>(host.data(sym::graphics));
            g_addresses.bucket_count = static_cast<std::uint32_t>(host.value(sym::graphics_types, "bucket_count"));
            g_addresses.buckets = static_cast<std::uint32_t>(host.value(sym::graphics_types, "buckets"));
            g_addresses.mesh_type_id = static_cast<std::uint32_t>(host.value(sym::mesh_type_id, "type_id"));
            g_addresses.mesh_vtable = addr(sym::mesh_type_vtable);
            g_addresses.reload_type = addr(sym::mesh_type_reload);
            g_addresses.string_assign = addr(sym::string_assign);
            g_addresses.console_manager = reinterpret_cast<std::uintptr_t>(host.data(sym::manager));
            g_addresses.console_dispatch = addr(sym::console_dispatch);
            g_addresses.string_free = addr(sym::string_free);
            g_addresses.game_state = reinterpret_cast<std::uintptr_t>(host.data(sym::game_state));
            g_addresses.graphics_owner = static_cast<std::uint32_t>(host.value(sym::province_units, "graphics_owner"));
            g_addresses.application = static_cast<std::uint32_t>(host.value(sym::province_units, "application"));
            g_addresses.map = static_cast<std::uint32_t>(host.value(sym::province_units, "map"));
            g_addresses.units = static_cast<std::uint32_t>(host.value(sym::province_units, "units"));
            g_addresses.unit_slots = static_cast<std::uint32_t>(host.value(sym::province_units, "unit_slots"));
            g_addresses.provinces = static_cast<std::uint32_t>(host.value(sym::province_graphics, "provinces"));
            g_addresses.province_count =
                static_cast<std::uint32_t>(host.value(sym::province_graphics_count, "province_count"));
            g_addresses.province_clear = addr(sym::province_graphics_clear);
            g_addresses.province_sprites = addr(sym::create_province_sprites);
            g_addresses.envoy_graphics = static_cast<std::uint32_t>(host.value(sym::province_envoys, "envoy_graphics"));
            g_addresses.envoys = static_cast<std::uint32_t>(host.value(sym::envoy_recreate, "envoys"));
            g_addresses.envoy_recreate = addr(sym::envoy_recreate);
            g_addresses.trade_routes = static_cast<std::uint32_t>(host.value(sym::map_trade_routes, "trade_routes"));
            g_addresses.trade_goods = static_cast<std::uint32_t>(host.value(sym::trade_goods_recreate, "trade_goods"));
            g_addresses.trade_goods_recreate = addr(sym::trade_goods_recreate);
            g_addresses.ambient_objects =
                static_cast<std::uint32_t>(host.value(sym::map_ambient_objects, "ambient_objects"));
            g_has_addresses = true;
        } catch (const std::exception& e) {
            host.warn("not available on this game build: {}", e.what());
        }
        wchar_t exe[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0) g_game_dir = fs::path(exe).parent_path();
        g_panels = host.service<AnvilPanelService>(ANVIL_PANEL_SERVICE, ANVIL_PANEL_SERVICE_VERSION);
        if (g_panels != nullptr && g_panels->size < sizeof(AnvilPanelService)) g_panels = nullptr;
        wchar_t control[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"ANVIL_MODEL_RELOAD_CONTROL", control, MAX_PATH) > 0) {
            g_control = control;
            host.info("ANVIL_MODEL_RELOAD_CONTROL: commands from {}", utf8(g_control));
            try {
                std::thread(control_loop).detach();
                start_thread();
            } catch (const std::exception&) {
                host.warn("ANVIL_MODEL_RELOAD_CONTROL: cannot start its thread");
            }
        }
        // A setting's callback runs inside the overlay's drawing: the thread
        // shows the window after the frame.
        host.subscribe_setting(cfg::window, [](bool on) {
            g_on.store(on);
            if (on) {
                g_show.store(true);
                start_thread();
            }
            wake();
        });
        if (!cfg::window.get()) {
            host.info("off: the window setting is off");
            return;
        }
        g_on.store(true);
        g_show.store(true);
        start_thread();
        host.info("on{}", g_panels == nullptr ? ", without a window (no anvil-overlay.panel)" : "");
    }
};

}  // namespace

ANVIL_PLUGIN(ModelReload)
