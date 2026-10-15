// Unit tests for the Anvil core. Usage: anvil_tests <path-to-built-version.dll>
#include "core/manifest.hpp"
#include "core/patching.hpp"
#include "core/resolver.hpp"
#include "core/semver.hpp"
#include "core/services.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                              \
    } while (0)

void run(const char* name, const std::function<void()>& body) {
    const int before = g_failures;
    body();
    std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", name);
}

using namespace anvil;

Manifest manifest(const std::string& toml) {
    auto r = parse_manifest_text(toml, L"C:\\plugins\\x");
    if (!r.manifest) std::printf("  manifest error: %s\n", r.error.c_str());
    return *r.manifest;
}

std::string plugin_toml(const std::string& id, const std::string& extra = "", const std::string& version = "1.0.0",
                        const std::string& phase = "main") {
    return "id = \"" + id + "\"\nname = \"" + id + "\"\nversion = \"" + version + "\"\napi = 1\nentry = \"" + id +
           ".dll\"\nphase = \"" + phase + "\"\ngame_builds = [\"*\"]\n" + extra;
}

std::vector<std::string> ids(const ResolveResult& r) {
    std::vector<std::string> out;
    for (const auto& m : r.order) out.push_back(m.id);
    return out;
}

bool skipped(const ResolveResult& r, const std::string& id) {
    for (const auto& s : r.skipped) {
        if (s.id == id) return true;
    }
    return false;
}

// Executable scratch "image" for patch tests.
struct Scratch {
    uint8_t* base;
    explicit Scratch(size_t size) {
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        std::memset(base, 0xCC, size);
    }
    ~Scratch() { VirtualFree(base, 0, MEM_RELEASE); }
};

int detour_returns_42() { return 42; }
DWORD WINAPI fake_tick_count() { return 12345u; }

}  // namespace

int main(int argc, char** argv) {
    run("semver parse/compare", [] {
        CHECK(Version::parse("1.2.3").has_value());
        CHECK(!Version::parse("1.2").has_value());
        CHECK(!Version::parse("01.2.3").has_value());
        CHECK(*Version::parse("1.2.3-alpha") < *Version::parse("1.2.3"));
        CHECK(*Version::parse("1.2.3-alpha.2") < *Version::parse("1.2.3-alpha.10"));
        CHECK(compare(*Version::parse("1.10.0"), *Version::parse("1.9.9")) > 0);
        CHECK(Version::parse("2.0.0+build.5")->to_string() == "2.0.0");
    });

    run("semver ranges", [] {
        auto v = [](const char* s) { return *Version::parse(s); };
        CHECK(VersionRange::parse(">=1.2.0, <2")->matches(v("1.9.0")));
        CHECK(!VersionRange::parse(">=1.2.0, <2")->matches(v("2.0.0")));
        CHECK(VersionRange::parse("^1.2")->matches(v("1.4.0")));
        CHECK(!VersionRange::parse("^1.2")->matches(v("2.0.0")));
        CHECK(VersionRange::parse("~1.2")->matches(v("1.2.9")));
        CHECK(!VersionRange::parse("~1.2")->matches(v("1.3.0")));
        CHECK(VersionRange::parse("1.2")->matches(v("1.2.7")));
        CHECK(VersionRange::parse("*")->matches(v("0.0.1")));
        CHECK(!VersionRange::parse(">=x").has_value());
    });

    run("manifest parsing", [] {
        auto ok = parse_manifest_text(plugin_toml("demo", "custom = 1\n[[dependencies]]\nid = \"base\"\nversion = \"^1\"\n"),
                                      L"C:\\p");
        CHECK(ok.manifest.has_value());
        CHECK(ok.manifest->dependencies.size() == 1 && ok.manifest->dependencies[0].id == "base");
        CHECK(ok.manifest->notes.size() == 1);  // unknown key 'custom'
        CHECK(!parse_manifest_text("id = \"Bad_Id\"\nname=\"x\"\nversion=\"1.0.0\"\napi=1\nentry=\"x.dll\"\ngame_builds=[\"*\"]",
                                   L"C:\\p").manifest);
        CHECK(!parse_manifest_text("id = \"a\"\nname=\"a\"\nversion=\"1.0\"\napi=1\nentry=\"a.dll\"\ngame_builds=[\"*\"]",
                                   L"C:\\p").manifest);  // not SemVer
        CHECK(!parse_manifest_text("id = \"a\"\nname=\"a\"\nversion=\"1.0.0\"\napi=1\nentry=\"..\\\\a.dll\"\ngame_builds=[\"*\"]",
                                   L"C:\\p").manifest);  // path in entry
        CHECK(!parse_manifest_text("id = \"a\"\nname=\"a\"\nversion=\"1.0.0\"\napi=1\nentry=\"a.dll\"", L"C:\\p").manifest);
        CHECK(!parse_manifest_text("id = = broken", L"C:\\p").manifest);
    });

    run("resolver: order, dependencies, builds", [] {
        ResolveInput in;
        in.host_api = 1;
        in.game_build = "eu4-1.37.5-steam-win64";
        in.manifests = {
            manifest(plugin_toml("ui", "[[dependencies]]\nid = \"core\"\nversion = \">=1.0.0\"\n")),
            manifest(plugin_toml("core")),
            manifest(plugin_toml("boost", "", "1.0.0", "early")),
            manifest(plugin_toml("extra", "load_after = [\"ui\"]\n[[dependencies]]\nid = \"missing\"\noptional = true\n")),
        };
        auto r = resolve(in);
        CHECK((ids(r) == std::vector<std::string>{"boost", "core", "ui", "extra"}));
        CHECK(r.skipped.empty());
    });

    run("resolver: failures cascade", [] {
        ResolveInput in;
        in.host_api = 1;
        in.game_build = nullptr;  // unknown build
        auto pinned = manifest(plugin_toml("pinned"));
        pinned.game_builds = {"eu4-1.37.5-steam-win64"};
        in.manifests = {
            pinned,
            manifest(plugin_toml("needs-pinned", "[[dependencies]]\nid = \"pinned\"\n")),
            manifest(plugin_toml("old-api")),
            manifest(plugin_toml("wants-v2", "[[dependencies]]\nid = \"lib\"\nversion = \">=2\"\n")),
            manifest(plugin_toml("lib", "", "1.5.0")),
            manifest(plugin_toml("lib", "", "9.9.9")),  // duplicate id
            manifest(plugin_toml("off")),
            manifest(plugin_toml("early-on-main", "[[dependencies]]\nid = \"lib\"\n", "1.0.0", "early")),
            manifest(plugin_toml("cyc-a", "load_after = [\"cyc-b\"]\n")),
            manifest(plugin_toml("cyc-b", "load_after = [\"cyc-a\"]\n")),
            manifest(plugin_toml("after-cycle", "[[dependencies]]\nid = \"cyc-a\"\n", "1.0.0", "main")),
        };
        in.manifests[2].api = 2;
        in.disabled = {"off"};
        auto r = resolve(in);
        CHECK(skipped(r, "pinned"));
        CHECK(skipped(r, "needs-pinned"));
        CHECK(skipped(r, "old-api"));
        CHECK(skipped(r, "wants-v2"));
        CHECK(skipped(r, "off"));
        CHECK(skipped(r, "early-on-main"));
        CHECK(skipped(r, "cyc-a") && skipped(r, "cyc-b") && skipped(r, "after-cycle"));
        CHECK((ids(r) == std::vector<std::string>{"lib"}));
        CHECK(r.order.size() == 1 && r.order[0].version.to_string() == "1.5.0");
    });

    run("patch registry: bytes, conflicts, rollback", [] {
        Scratch s(0x1000);
        PatchRegistry reg(reinterpret_cast<uintptr_t>(s.base), 0x1000);
        const uint8_t expected[4] = {0xCC, 0xCC, 0xCC, 0xCC};
        const uint8_t replacement[4] = {1, 2, 3, 4};
        const auto addr = reinterpret_cast<uintptr_t>(s.base) + 0x100;
        CHECK(reg.patch_bytes("a", addr, expected, replacement, 4) == ANVIL_OK);
        CHECK(std::memcmp(s.base + 0x100, replacement, 4) == 0);
        CHECK(reg.patch_bytes("b", addr + 2, expected, replacement, 4) == ANVIL_ERROR_CONFLICT);
        CHECK(reg.patch_bytes("b", addr + 0x10, replacement, expected, 4) == ANVIL_ERROR_BYTES_MISMATCH);
        CHECK(reg.patch_bytes("b", reinterpret_cast<uintptr_t>(s.base) + 0xFFE, expected, replacement, 4) ==
              ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(reg.count("a") == 1);
        CHECK(reg.rollback("a") == 1);
        CHECK(std::memcmp(s.base + 0x100, expected, 4) == 0);
        CHECK(reg.patch_bytes("b", addr + 2, expected, replacement, 4) == ANVIL_OK);  // free again
    });

    run("patch registry: redirect_call", [] {
        Scratch s(0x1000);
        PatchRegistry reg(reinterpret_cast<uintptr_t>(s.base), 0x1000);
        // call +0x200 at offset 0x10: E8 rel32 with rel = 0x200 - (0x10 + 5)
        const int32_t rel = 0x200 - 0x15;
        s.base[0x10] = 0xE8;
        std::memcpy(s.base + 0x11, &rel, 4);
        const uintptr_t base = reinterpret_cast<uintptr_t>(s.base);
        CHECK(reg.redirect_call("a", base + 0x10, base + 0x300, reinterpret_cast<void*>(&detour_returns_42)) ==
              ANVIL_ERROR_BYTES_MISMATCH);
        CHECK(reg.redirect_call("a", base + 0x10, base + 0x200, reinterpret_cast<void*>(&detour_returns_42)) == ANVIL_OK);
        int32_t now = 0;
        std::memcpy(&now, s.base + 0x11, 4);
        const uintptr_t target = base + 0x15 + static_cast<intptr_t>(now);
        // Either directly the detour or a near stub that jumps to it.
        CHECK(target == reinterpret_cast<uintptr_t>(&detour_returns_42) ||
              (reinterpret_cast<const uint8_t*>(target)[0] == 0xFF && reinterpret_cast<const uint8_t*>(target)[1] == 0x25));
        CHECK(reg.rollback("a") == 1);
        std::memcpy(&now, s.base + 0x11, 4);
        CHECK(now == rel);
    });

    run("patch registry: hook_function (SafetyHook)", [] {
        Scratch s(0x1000);
        // int f() { return 1; } padded so a 14-byte jump also fits: mov eax,1; 9x nop; ret
        const uint8_t code[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xC3};
        std::memcpy(s.base + 0x40, code, sizeof(code));
        FlushInstructionCache(GetCurrentProcess(), s.base, 0x1000);
        using Fn = int (*)();
        const auto f = reinterpret_cast<Fn>(s.base + 0x40);
        CHECK(f() == 1);
        PatchRegistry reg(reinterpret_cast<uintptr_t>(s.base), 0x1000);
        void* original = nullptr;
        const uint8_t wrong[] = {0xB8, 0x02};
        CHECK(reg.hook_function("a", reinterpret_cast<uintptr_t>(f), wrong, sizeof(wrong),
                                reinterpret_cast<void*>(&detour_returns_42), &original) == ANVIL_ERROR_BYTES_MISMATCH);
        CHECK(reg.hook_function("a", reinterpret_cast<uintptr_t>(f), code, 5, reinterpret_cast<void*>(&detour_returns_42),
                                &original) == ANVIL_OK);
        CHECK(f() == 42);
        CHECK(original != nullptr && reinterpret_cast<Fn>(original)() == 1);
        CHECK(reg.patch_bytes("b", reinterpret_cast<uintptr_t>(f) + 1, code + 1, code + 1, 2) == ANVIL_ERROR_CONFLICT);
        CHECK(reg.rollback("a") == 1);
        CHECK(f() == 1);
    });

    run("patch registry: replace_import", [] {
        HMODULE self = GetModuleHandleW(nullptr);
        PatchRegistry reg(reinterpret_cast<uintptr_t>(self), 0x7FFFFFFF);
        const DWORD real = GetTickCount();
        void* original = nullptr;
        CHECK(reg.replace_import("a", self, "kernel32.dll", "GetTickCount", reinterpret_cast<void*>(&fake_tick_count),
                                 &original) == ANVIL_OK);
        CHECK(GetTickCount() == 12345u);
        CHECK(original != nullptr);
        CHECK(reg.replace_import("b", self, "KERNEL32.dll", "GetTickCount", reinterpret_cast<void*>(&fake_tick_count),
                                 nullptr) == ANVIL_ERROR_CONFLICT);
        CHECK(reg.replace_import("a", self, "kernel32.dll", "NoSuchFunction", reinterpret_cast<void*>(&fake_tick_count),
                                 nullptr) == ANVIL_ERROR_NOT_FOUND);
        CHECK(reg.rollback("a") == 1);
        CHECK(GetTickCount() - real < 60000u);
    });

    run("services and events", [] {
        ServiceBus bus;
        struct Svc { uint32_t size; int value; } svc{sizeof(Svc), 7};
        CHECK(bus.register_service("ui", "ui.overlay", 1, &svc) == ANVIL_OK);
        CHECK(bus.register_service("ui", "ui.overlay", 1, &svc) == ANVIL_ERROR_ALREADY_EXISTS);
        CHECK(bus.register_service("other", "ui.overlay", 2, &svc) == ANVIL_ERROR_INVALID_ARGUMENT);  // foreign prefix
        CHECK(bus.get_service("ui.overlay", 1) == &svc);
        CHECK(bus.get_service("ui.overlay", 2) == nullptr);
        static int calls = 0;
        CHECK(bus.subscribe("ui", "anvil.game_ready", [](const char*, const void*, void*) { ++calls; }, nullptr) == ANVIL_OK);
        CHECK(bus.publish("anvil.game_ready", nullptr) == 1 && calls == 1);
        bus.remove_owner("ui");
        CHECK(bus.get_service("ui.overlay", 1) == nullptr);
        CHECK(bus.publish("anvil.game_ready", nullptr) == 0);
    });

    run("proxy forwards version.dll", [&] {
        if (argc < 2) {
            std::printf("  (skipped: pass the built version.dll path)\n");
            return;
        }
        HMODULE proxy = LoadLibraryExA(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        CHECK(proxy != nullptr);
        if (proxy == nullptr) return;
        using SizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
        const auto viaProxy = reinterpret_cast<SizeFn>(GetProcAddress(proxy, "GetFileVersionInfoSizeW"));
        // By full path: a bare "version.dll" would resolve to the already loaded proxy.
        wchar_t systemPath[MAX_PATH] = {};
        GetSystemDirectoryW(systemPath, MAX_PATH);
        lstrcatW(systemPath, L"\\version.dll");
        HMODULE system = LoadLibraryW(systemPath);
        CHECK(system != nullptr && system != proxy);
        const auto direct = reinterpret_cast<SizeFn>(GetProcAddress(system, "GetFileVersionInfoSizeW"));
        CHECK(viaProxy != nullptr && direct != nullptr && viaProxy != direct);
        wchar_t kernel[MAX_PATH] = {};
        GetSystemDirectoryW(kernel, MAX_PATH);
        lstrcatW(kernel, L"\\kernel32.dll");
        DWORD h1 = 0, h2 = 0;
        const DWORD a = viaProxy(kernel, &h1), b = direct(kernel, &h2);
        CHECK(a != 0 && a == b);
        const char* names[] = {"GetFileVersionInfoA", "GetFileVersionInfoByHandle", "GetFileVersionInfoExA",
                               "GetFileVersionInfoExW", "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeExA",
                               "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW", "GetFileVersionInfoW",
                               "VerFindFileA", "VerFindFileW", "VerInstallFileA", "VerInstallFileW",
                               "VerLanguageNameA", "VerLanguageNameW", "VerQueryValueA", "VerQueryValueW"};
        for (const char* n : names) CHECK(GetProcAddress(proxy, n) != nullptr);
        using LangFn = DWORD(WINAPI*)(DWORD, LPWSTR, DWORD);
        wchar_t lang[64] = {};
        CHECK(reinterpret_cast<LangFn>(GetProcAddress(proxy, "VerLanguageNameW"))(0x0415, lang, 64) > 0);

        // The DLL's version resource carries version.txt, like the log line.
        using InfoFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
        using QueryFn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
        const auto info = reinterpret_cast<InfoFn>(GetProcAddress(system, "GetFileVersionInfoW"));
        const auto query = reinterpret_cast<QueryFn>(GetProcAddress(system, "VerQueryValueW"));
        wchar_t proxyPath[MAX_PATH] = {};
        GetModuleFileNameW(proxy, proxyPath, MAX_PATH);
        DWORD ignored = 0;
        std::vector<uint8_t> block(direct(proxyPath, &ignored));
        VS_FIXEDFILEINFO* fixed = nullptr;
        UINT fixedSize = 0;
        CHECK(!block.empty() && info(proxyPath, 0, static_cast<DWORD>(block.size()), block.data()) &&
              query(block.data(), L"\\", reinterpret_cast<LPVOID*>(&fixed), &fixedSize) && fixed != nullptr);
        const auto expected = Version::parse(ANVIL_VERSION);
        CHECK(expected.has_value());
        if (fixed != nullptr && expected) {
            CHECK(HIWORD(fixed->dwFileVersionMS) == expected->major && LOWORD(fixed->dwFileVersionMS) == expected->minor &&
                  HIWORD(fixed->dwFileVersionLS) == expected->patch);
        }
    });

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
