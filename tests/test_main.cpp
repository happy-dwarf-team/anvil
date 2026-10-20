// Unit tests for the Anvil core. Usage: anvil_tests <path-to-built-version.dll>
#include "core/manifest.hpp"
#include "core/resolver.hpp"
#include "core/semver.hpp"
#include "core/services.hpp"
#include "test_support.hpp"

#include <anvil/anvil_plugin.h>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

using namespace anvil;

std::string skip_reason(const ResolveResult& r, const std::string& id) {
    for (const auto& s : r.skipped) {
        if (s.id == id) return s.reason;
    }
    return {};
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

    manifest_tests();
    resolver_tests();

    run("resolver: order, dependencies, builds", [] {
        ResolveInput in;
        in.host_api = kManifestApi;
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
        in.host_api = kManifestApi;
        in.game_build = nullptr;  // unknown build
        in.manifests = {
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
        in.manifests[0].api = 1;
        in.disabled = {"off"};
        auto r = resolve(in);
        CHECK(skipped(r, "old-api"));
        CHECK(skipped(r, "wants-v2"));
        CHECK(skipped(r, "off"));
        CHECK(skipped(r, "early-on-main"));
        CHECK(skipped(r, "cyc-a") && skipped(r, "cyc-b") && skipped(r, "after-cycle"));
        CHECK((ids(r) == std::vector<std::string>{"lib"}));
        CHECK(r.order.size() == 1 && r.order[0].version.to_string() == "1.5.0");
    });

    run("byte patterns", [] {
        std::vector<uint8_t> bytes, mask;
        std::string error;
        CHECK(parse_byte_pattern("48 8B ?? 24", bytes, mask, error));
        CHECK((bytes == std::vector<uint8_t>{0x48, 0x8B, 0x00, 0x24}));
        CHECK((mask == std::vector<uint8_t>{0xFF, 0xFF, 0x00, 0xFF}));
        CHECK(parse_byte_pattern("488b??24", bytes, mask, error) && bytes.size() == 4 && mask[2] == 0);
        CHECK(parse_byte_pattern("48 8B\n\t?? 24\r\n", bytes, mask, error) && bytes.size() == 4);
        CHECK(!parse_byte_pattern("", bytes, mask, error));
        CHECK(!parse_byte_pattern("4", bytes, mask, error));      // half a byte
        CHECK(!parse_byte_pattern("4G", bytes, mask, error));     // not hex
        CHECK(!parse_byte_pattern("?4", bytes, mask, error));     // nibble wildcards are not supported
        CHECK(!parse_byte_pattern("?? ??", bytes, mask, error));  // nothing to compare
        CHECK(parse_byte_pattern(std::string(2 * kMaxPatternBytes, '0'), bytes, mask, error));
        CHECK(!parse_byte_pattern(std::string(2 * kMaxPatternBytes + 2, '0'), bytes, mask, error));
    });

    change_tests();
    gameplay_tests();
    settings_tests();
    control_tests();

    run("resolver: verify skips plugins and dependents", [] {
        ResolveInput in;
        in.host_api = kManifestApi;
        in.game_build = "eu4-1.37.5-steam-win64";
        in.manifests = {
            manifest(plugin_toml("bad")),
            manifest(plugin_toml("needs-bad", "[[dependencies]]\nid = \"bad\"\n")),
            manifest(plugin_toml("fine")),
            manifest(plugin_toml("off")),
        };
        in.disabled = {"off"};
        std::vector<std::string> verified;
        in.verify = [&](const Manifest& m) {
            verified.push_back(m.id);
            return m.id == "bad" ? std::string("bytes differ") : std::string();
        };
        auto r = resolve(in);
        CHECK(skip_reason(r, "bad") == "bytes differ");
        CHECK(skipped(r, "needs-bad"));
        CHECK((ids(r) == std::vector<std::string>{"fine"}));
        CHECK((verified == std::vector<std::string>{"bad", "needs-bad", "fine"}));  // not called for disabled plugins
    });

    run("manifest + resolver: minimum Anvil version", [] {
        auto ok = parse_manifest_text(plugin_toml("needs-new", "anvil = \">=0.2.0, <1\"\n"), L"C:\\p");
        CHECK(ok.manifest && ok.manifest->anvil && ok.manifest->anvil->text() == ">=0.2.0, <1");
        CHECK(!parse_manifest_text(plugin_toml("bad", "anvil = \">=x\"\n"), L"C:\\p").manifest);
        CHECK(!parse_manifest_text(plugin_toml("bad", "anvil = 2\n"), L"C:\\p").manifest);

        ResolveInput in;
        in.host_api = kManifestApi;
        in.game_build = "eu4-1.37.5-steam-win64";
        in.manifests = {*ok.manifest, manifest(plugin_toml("any")),
                        manifest(plugin_toml("needs-new-dep", "[[dependencies]]\nid = \"needs-new\"\n"))};
        in.host_version = Version::parse("0.1.0");
        auto old = resolve(in);
        CHECK(skip_reason(old, "needs-new") == "requires Anvil >=0.2.0, <1, this is Anvil 0.1.0");
        CHECK(skipped(old, "needs-new-dep"));
        CHECK((ids(old) == std::vector<std::string>{"any"}));
        in.host_version = Version::parse("0.2.3");
        CHECK((ids(resolve(in)) == std::vector<std::string>{"any", "needs-new", "needs-new-dep"}));
        in.host_version = Version::parse("1.0.0");
        CHECK(skipped(resolve(in), "needs-new"));
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
