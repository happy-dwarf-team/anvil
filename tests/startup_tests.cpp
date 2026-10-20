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

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
