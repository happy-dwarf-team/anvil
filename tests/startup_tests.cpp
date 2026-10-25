// The loader's start-up with plugins in mods (design §14, #60), end to end:
// each case runs the real anvil::startup() in a child process of this test on
// a made-up game folder whose userdir.txt names a made-up user folder with
// dlc_load.json and mods, then reads anvil.log. The plugin is mod-probe
// (tests/plugins/mod-probe), which has no symbols and loads in any process.
// Nothing here needs, or touches, a real game or EU4's user folder.
// Usage: anvil_startup_tests <folder of the built mod-probe plugin>
#include "core/log.hpp"
#include "core/mod_plugins.hpp"
#include "core/plugin_hash.hpp"
#include "core/runtime.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

using namespace anvil;
namespace fs = std::filesystem;

void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read(const fs::path& path) {
    // anvil.log stays open in the child while it runs.
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string text;
    char buffer[16384];
    DWORD n = 0;
    while (ReadFile(f, buffer, sizeof(buffer), &n, nullptr) && n > 0) text.append(buffer, n);
    CloseHandle(f);
    return text;
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::string generic(const fs::path& path) {
    const auto u8 = path.lexically_normal().generic_u8string();
    return std::string(u8.begin(), u8.end());
}

// The child: start-up as at the game's entry point, until the main phase has
// written its summary; then the exit the game makes.
int child(const fs::path& game_dir) {
    anvil::startup(game_dir);
    const fs::path log = game_dir / "anvil" / "anvil.log";
    const ULONGLONG deadline = GetTickCount64() + 30000;
    while (!contains(read(log), "[anvil] status: ") && GetTickCount64() < deadline) Sleep(5);
    // A copy of mod-probe published anvil-game.ready: wait for the benchmark's ready record too.
    if (GetEnvironmentVariableW(L"ANVIL_PROBE_PUBLISH", nullptr, 0) > 0 && GetEnvironmentVariableW(L"ANVIL_BENCHMARK", nullptr, 0) > 0) {
        while (!contains(read(log), "benchmark: start record written at ready") && GetTickCount64() < deadline) Sleep(5);
    }
    anvil::shutdown();  // not counted as an unfinished start (safe mode)
    return contains(read(log), "[anvil] status: ") ? 0 : 1;
}

struct Game {
    fs::path root, dir, anvil, user;
    fs::path probe;  // the built plugin
    std::vector<std::string> enabled;

    explicit Game(const fs::path& built_probe) : probe(built_probe) {
        root = fs::temp_directory_path() / ("anvil_startup_tests_" + std::to_string(GetCurrentProcessId()));
        fs::remove_all(root);
        dir = root / "eu4";
        anvil = dir / "anvil";
        user = root / "user";
        write(anvil / "anvil.toml", "enabled = true\n");
        fs::create_directories(anvil / "plugins");
        write(dir / "userdir.txt", generic(user) + "\n");
        fs::create_directories(user / "mod");
    }
    ~Game() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // A local mod with mod-probe in its anvil/mod-probe, enabled.
    fs::path mod(const std::string& name) {
        const fs::path folder = user / "mod" / name;
        fs::create_directories(folder / "anvil" / "mod-probe");
        fs::copy(probe, folder / "anvil" / "mod-probe", fs::copy_options::overwrite_existing | fs::copy_options::recursive);
        write(user / "mod" / (name + ".mod"), "name=\"" + name + "\"\npath=\"mod/" + name + "\"\n");
        enabled.push_back("mod/" + name + ".mod");
        std::string json = "{\"enabled_mods\":[";
        for (size_t i = 0; i < enabled.size(); ++i) json += (i ? ",\"" : "\"") + enabled[i] + "\"";
        write(user / "dlc_load.json", json + "],\"disabled_dlcs\":[]}");
        return folder / "anvil" / "mod-probe";
    }

    const ModPlugin* plugin(const ModDiscovery& d, const std::string& mod) const {
        for (const ModPlugin& p : d.plugins) {
            if (p.mod.name == mod) return &p;
        }
        return nullptr;
    }

    bool approve(const std::string& mod) const {
        const ModDiscovery d = discover_mod_plugins(anvil, user, {});
        const ModPlugin* p = plugin(d, mod);
        std::string error;
        if (p != nullptr && approve_mod_plugin(anvil, *p, p->hash, "", error)) return true;
        std::printf("  approve: %s\n", error.c_str());
        return false;
    }

    std::string approved_hash() const {
        const TrustFile trust = read_trust(anvil / "trust.toml");
        return trust.entries.empty() ? std::string() : trust.entries[0].hash;
    }

    // One start; the log it wrote.
    std::string start() const {
        fs::remove(anvil / "anvil.log");
        fs::remove(anvil / "anvil.prev.log");
        wchar_t self[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
        std::wstring command = L"\"" + std::wstring(self) + L"\" --child \"" + dir.wstring() + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return {};
        WaitForSingleObject(pi.hProcess, 60000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        const std::string log = read(anvil / "anvil.log");
        if (code != 0) std::printf("  the child failed (%lu); its log:\n%s\n", code, log.c_str());
        return log;
    }
};

const char* kRunning = "[mod-probe] mod probe is running";

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--child") return child(fs::path(widen(argv[2])));
    if (argc < 2) {
        std::printf("usage: anvil_startup_tests <mod-probe plugin folder>\n");
        return 2;
    }
    SetEnvironmentVariableW(L"ANVIL_DISABLE", nullptr);  // the child inherits it; its start-up must load
    SetEnvironmentVariableW(L"ANVIL_BENCHMARK", nullptr);
    const fs::path probe = fs::path(widen(argv[1]));

    run("start-up: a plugin in a mod waits for approval, then loads from its approved copy", [&] {
        Game game(probe);
        const fs::path in_mod = game.mod("Probe Mod");
        std::string log = game.start();
        CHECK(contains(log, "mod-probe: skipped — from mod 'Probe Mod' (local mod): waiting for approval (hash "));
        CHECK(contains(log, "found 1 plugin(s) in 1 enabled mod(s) at ") && !contains(log, kRunning));

        CHECK(game.approve("Probe Mod"));
        const fs::path copy = approved_copy(game.anvil, "mod-probe", game.approved_hash());
        log = game.start();
        CHECK(contains(log, "mod-probe from mod 'Probe Mod' (local mod): approved on ") &&
              contains(log, "; loads from " + narrow(copy.wstring())));
        CHECK(contains(log, kRunning) && contains(log, "mod-probe 1.0.0: loaded (main, "));

        // A missing copy of the approved hash is made again from the mod.
        fs::remove_all(copy);
        log = game.start();
        CHECK(contains(log, "mod-probe from mod 'Probe Mod' (local mod): made the approved copy") && contains(log, kRunning));
        CHECK(hash_plugin_folder(copy).sha256 == game.approved_hash());

        // The approved copy changed: checked right before LoadLibrary, not loaded.
        write(copy / "extra.txt", "changed");
        log = game.start();
        CHECK(contains(log, "mod-probe 1.0.0: not loaded; its approved copy changed (hash ") && !contains(log, kRunning));
        fs::remove(copy / "extra.txt");

        // Steam (or the author) changes the mod: the approved copy keeps loading.
        write(in_mod / "README.txt", "new");
        log = game.start();
        CHECK(contains(log, "which is not approved; the approved 1.0.0 loads until then; loads from " +
                                narrow(copy.wstring())));
        CHECK(contains(log, kRunning));
        // Approving the new version: the old copy goes at the next start.
        CHECK(game.approve("Probe Mod"));
        const fs::path new_copy = approved_copy(game.anvil, "mod-probe", game.approved_hash());
        log = game.start();
        CHECK(contains(log, "; loads from " + narrow(new_copy.wstring())) && contains(log, kRunning));
        CHECK(!fs::exists(copy) && fs::exists(new_copy));

        // Disabling the mod stops the plugin; the approval stays.
        write(game.user / "dlc_load.json", "{\"enabled_mods\":[],\"disabled_dlcs\":[]}");
        log = game.start();
        CHECK(!contains(log, kRunning) && !game.approved_hash().empty() && fs::exists(new_copy));
    });

    run("start-up: a local plugin wins; two mods with one id load neither until one is approved", [&] {
        Game game(probe);
        game.mod("Mod A");
        game.mod("Mod B");
        std::string log = game.start();
        CHECK(contains(log, "mod-probe: skipped — from mod 'Mod A' (local mod): 2 enabled mods have a plugin 'mod-probe'"));
        CHECK(contains(log, "mod-probe: skipped — from mod 'Mod B' (local mod): 2 enabled mods have a plugin 'mod-probe'"));
        CHECK(!contains(log, kRunning));

        CHECK(game.approve("Mod B"));
        log = game.start();
        CHECK(contains(log, "mod-probe from mod 'Mod B' (local mod): approved on ") && contains(log, kRunning));
        CHECK(contains(log, "mod-probe: skipped — from mod 'Mod A' (local mod): mod 'Mod B' has this plugin too"));

        fs::copy(probe, game.anvil / "plugins" / "mod-probe", fs::copy_options::recursive);
        log = game.start();
        CHECK(contains(log, "mod-probe: the local plugin in anvil/plugins wins over the same id in mod(s) 'Mod A', 'Mod B'"));
        CHECK(contains(log, kRunning) && !contains(log, "loads from"));
    });

    run("start-up: the benchmark arm leaves the compared plugin out without touching anvil.toml", [&] {
        Game game(probe);
        fs::copy(probe, game.anvil / "plugins" / "mod-probe", fs::copy_options::recursive);
        const std::string toml = read(game.anvil / "anvil.toml");

        SetEnvironmentVariableW(L"ANVIL_BENCHMARK", L"without:mod-probe");
        std::string log = game.start();
        CHECK(contains(log, "benchmark start: arm without:mod-probe"));
        CHECK(contains(log, "mod-probe: skipped — left out by the start-up benchmark (ANVIL_BENCHMARK=without:mod-probe)"));
        CHECK(!contains(log, kRunning));
        CHECK(read(game.anvil / "anvil.toml") == toml);

        SetEnvironmentVariableW(L"ANVIL_BENCHMARK", L"with:mod-probe");
        log = game.start();
        CHECK(contains(log, "benchmark start: arm with:mod-probe") && contains(log, kRunning));

        // Another plugin's arm leaves this one alone.
        SetEnvironmentVariableW(L"ANVIL_BENCHMARK", L"without:bellows");
        log = game.start();
        CHECK(contains(log, "benchmark start: arm without:bellows") && contains(log, kRunning));

        SetEnvironmentVariableW(L"ANVIL_BENCHMARK", L"off:mod-probe");
        log = game.start();
        CHECK(contains(log, "ANVIL_BENCHMARK=off:mod-probe ignored, so this is not a benchmark start: expected "));
        CHECK(contains(log, kRunning) && !contains(log, "benchmark start: arm"));

        SetEnvironmentVariableW(L"ANVIL_BENCHMARK", nullptr);
        log = game.start();
        CHECK(!contains(log, "ANVIL_BENCHMARK") && contains(log, kRunning));
        CHECK(read(game.anvil / "anvil.toml") == toml);
    });

    run("start-up: a benchmark start writes its record at ready and at exit", [&] {
        Game game(probe);
        fs::copy(probe, game.anvil / "plugins" / "mod-probe", fs::copy_options::recursive);
        // A stand-in for anvil-game: a copy of mod-probe whose id owns
        // anvil-game.ready, loaded after mod-probe has joined.
        const fs::path stand_in = game.anvil / "plugins" / "anvil-game";
        fs::copy(probe, stand_in, fs::copy_options::recursive);
        std::string toml = read(stand_in / "plugin.toml");
        toml.replace(toml.find("id = \"mod-probe\""), 16, "id = \"anvil-game\"");
        write(stand_in / "plugin.toml", toml + "load_after = [\"mod-probe\"]\n");
        const fs::path record_file = game.anvil / "data" / "anvil" / "benchmark-start.json";
        SetEnvironmentVariableW(L"ANVIL_PROBE_BENCHMARK", L"1");
        SetEnvironmentVariableW(L"ANVIL_PROBE_PUBLISH", L"anvil-game.ready");

        // Not a benchmark start: the fields are taken and checked, no record.
        std::string log = game.start();
        CHECK(contains(log, "[mod-probe] benchmark fields set (inactive)"));
        CHECK(contains(log, "game ready at ") && !fs::exists(record_file));
        CHECK(fs::exists(game.anvil / "data" / "anvil" / "last-start.txt"));

        SetEnvironmentVariableW(L"ANVIL_BENCHMARK", L"with:mod-probe");
        log = game.start();
        CHECK(contains(log, "[mod-probe] benchmark fields set (active)"));
        CHECK(contains(log, "benchmark: start record written at ready"));
        std::string record = read(record_file);
        CHECK(contains(record, "{\"format\":1,\"write\":\"exit\",\"anvil\":\""));
        CHECK(contains(record, "\"arm\":{\"with\":true,\"plugin\":\"mod-probe\"}"));
        CHECK(contains(record, "\"time_ms\":") && !contains(record, "\"ready_qpc\":null"));
        CHECK(contains(record, "{\"id\":\"anvil-game\",\"version\":\"1.0.0\",\"state\":\"loaded\",\"participant\":true"));
        CHECK(contains(record, "{\"id\":\"mod-probe\",\"version\":\"1.0.0\",\"state\":\"loaded\",\"participant\":true,"
                               "\"done\":true,\"learning\":null,\"open_cost\":{\"files\":42,\"ms\":2.25}"));
        CHECK(contains(record, "{\"name\":\"probe.mode\",\"kind\":\"enum\",\"unit\":null,\"label\":\"Mode \\\"quoted\\\"\","
                               "\"options\":[\"cold\",\"warm\"]}"));
        CHECK(contains(record, "\"fields\":{\"probe.files\":42,\"probe.time\":1.5,\"probe.mode\":\"warm\",\"probe.ok\":true,"
                               "\"unset\":null}"));
        CHECK(contains(record, "\"game_first_opens\":{\"files\":"));
        // The previous start left its history: this one is not the first since boot.
        CHECK(contains(record, "\"labels\":{\"first_start_since_boot\":false,\"definitions_changed\":"));
        CHECK(contains(record, "\"input_during_start\":"));

        // Without the plugin: it is disabled by the arm.
        SetEnvironmentVariableW(L"ANVIL_BENCHMARK", L"without:mod-probe");
        log = game.start();
        record = read(record_file);
        CHECK(contains(record, "\"arm\":{\"with\":false,\"plugin\":\"mod-probe\"}"));
        CHECK(contains(record, "{\"id\":\"mod-probe\",\"state\":\"disabled\",\"by\":\"arm\",\"participant\":false}"));

        // No ready: the old record is gone and no new one is written.
        SetEnvironmentVariableW(L"ANVIL_PROBE_PUBLISH", nullptr);
        log = game.start();
        CHECK(!fs::exists(record_file) && !contains(log, "start record written"));
        SetEnvironmentVariableW(L"ANVIL_PROBE_BENCHMARK", nullptr);
        SetEnvironmentVariableW(L"ANVIL_BENCHMARK", nullptr);
    });

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
