// Unit tests for the Anvil core. Usage: anvil_tests <path-to-built-version.dll>
#include "core/expectations.hpp"
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

// Manifest pinned to one build, for [[expect]] tests.
std::string pinned_toml(const std::string& extra) {
    return "id = \"p\"\nname = \"p\"\nversion = \"1.0.0\"\napi = 1\nentry = \"p.dll\"\n"
           "game_builds = [\"eu4-1.37.5-steam-win64\"]\n" + extra;
}

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
        CHECK(parse_byte_pattern(std::string(2 * ANVIL_MAX_EXPECT_BYTES, '0'), bytes, mask, error));
        CHECK(!parse_byte_pattern(std::string(2 * ANVIL_MAX_EXPECT_BYTES + 2, '0'), bytes, mask, error));
    });

    run("manifest: [[expect]]", [] {
        auto ok = parse_manifest_text(pinned_toml("[[expect]]\nbuild = \"eu4-1.37.5-steam-win64\"\nrva = 0x210000\n"
                                                  "bytes = \"48 89 5C 24 ??\"\nname = \"ResetGame\"\ncomment = 1\n"
                                                  "[[expect]]\nbuild = \"*\"\nrva = 0\nbytes = '''\n4D 5A\n'''\n"),
                                      L"C:\\p");
        CHECK(ok.manifest.has_value());
        if (ok.manifest) {
            const auto& e = ok.manifest->expectations;
            CHECK(e.size() == 2);
            CHECK(e[0].rva == 0x210000 && e[0].name == "ResetGame" && e[0].bytes.size() == 5 && e[0].mask[4] == 0);
            CHECK(e[1].build == "*" && e[1].rva == 0 && e[1].bytes == (std::vector<uint8_t>{0x4D, 0x5A}));
            CHECK(ok.manifest->notes.size() == 1);  // unknown key 'comment' inside [[expect]]
        }
        // Any build id is accepted when the plugin itself supports "*".
        CHECK(parse_manifest_text(plugin_toml("any", "[[expect]]\nbuild = \"eu4-1.38.0-steam-win64\"\nrva = 1\nbytes = \"90\"\n"),
                                  L"C:\\p").manifest);
        auto fails = [](const std::string& expect) {
            return !parse_manifest_text(pinned_toml(expect), L"C:\\p").manifest.has_value();
        };
        CHECK(fails("[[expect]]\nbuild = \"eu4-1.38.0-steam-win64\"\nrva = 1\nbytes = \"90\"\n"));  // not in game_builds
        CHECK(fails("[[expect]]\nrva = 1\nbytes = \"90\"\n"));                                       // no build
        CHECK(fails("[[expect]]\nbuild = \"*\"\nrva = -1\nbytes = \"90\"\n"));
        CHECK(fails("[[expect]]\nbuild = \"*\"\nrva = \"0x10\"\nbytes = \"90\"\n"));  // rva as string
        CHECK(fails("[[expect]]\nbuild = \"*\"\nrva = 0x100000000\nbytes = \"90\"\n"));
        CHECK(fails("[[expect]]\nbuild = \"*\"\nrva = 0xFFFFFFFF\nbytes = \"90 90\"\n"));  // past 4 GB
        CHECK(fails("[[expect]]\nbuild = \"*\"\nrva = 1\n"));                              // no bytes
        CHECK(fails("[[expect]]\nbuild = \"*\"\nrva = 1\nbytes = \"9\"\n"));
        CHECK(fails("expect = 1\n"));
    });

    run("patch registry: expect_bytes", [] {
        Scratch s(0x2000);
        const uintptr_t base = reinterpret_cast<uintptr_t>(s.base);
        PatchRegistry reg(base, 0x2000);
        const uint8_t code[] = {0x48, 0x89, 0x5C, 0x24, 0x08};
        std::memcpy(s.base + 0x100, code, sizeof(code));
        const uintptr_t addr = base + 0x100;
        std::string detail;
        CHECK(reg.expect_bytes("mine", addr, code, nullptr, sizeof(code)) == ANVIL_OK);
        const uint8_t other[] = {0x48, 0x89, 0x5C, 0x24, 0x10};
        CHECK(reg.expect_bytes("mine", addr, other, nullptr, sizeof(other), &detail) == ANVIL_ERROR_BYTES_MISMATCH);
        CHECK(detail == "byte +0x4: expected 0x10, found 0x08");
        const uint8_t wildcard[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00};
        CHECK(reg.expect_bytes("mine", addr, other, wildcard, sizeof(other)) == ANVIL_OK);
        const uint8_t lowNibble[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x0F};  // 0x08 vs 0x18: low nibble equal
        const uint8_t eighteen[] = {0x48, 0x89, 0x5C, 0x24, 0x18};
        CHECK(reg.expect_bytes("mine", addr, eighteen, lowNibble, sizeof(eighteen)) == ANVIL_OK);

        // Arguments, image bounds and unreadable memory.
        CHECK(reg.expect_bytes("mine", addr, nullptr, nullptr, 4) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(reg.expect_bytes("mine", addr, code, nullptr, 0) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(reg.expect_bytes("mine", addr, code, nullptr, ANVIL_MAX_EXPECT_BYTES + 1) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(reg.expect_bytes("mine", base + 0x1FFE, code, nullptr, 4) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(VirtualFree(s.base + 0x1000, 0x1000, MEM_DECOMMIT));
        CHECK(reg.expect_bytes("mine", base + 0x1000, code, nullptr, 1) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(reg.expect_bytes("mine", base + 0xFFE, code, nullptr, 4) == ANVIL_ERROR_INVALID_ARGUMENT);

        // Patches of other owners are compared in their original form.
        const uint8_t nops[] = {0x90, 0x90};
        CHECK(reg.patch_bytes("other", addr, code, nops, 2) == ANVIL_OK);
        CHECK(s.base[0x100] == 0x90);
        CHECK(reg.expect_bytes("mine", addr, code, nullptr, sizeof(code)) == ANVIL_OK);
        const uint8_t fn[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xC3};
        std::memcpy(s.base + 0x200, fn, sizeof(fn));
        FlushInstructionCache(GetCurrentProcess(), s.base + 0x200, sizeof(fn));
        void* original = nullptr;
        CHECK(reg.hook_function("other", base + 0x200, fn, 5, reinterpret_cast<void*>(&detour_returns_42), &original) ==
              ANVIL_OK);
        CHECK(std::memcmp(s.base + 0x200, fn, sizeof(fn)) != 0);
        CHECK(reg.expect_bytes("mine", base + 0x200, fn, nullptr, sizeof(fn)) == ANVIL_OK);

        // Own patches first: dependencies must be verified before patching.
        const uint8_t cc[] = {0xCC, 0xCC};
        CHECK(reg.patch_bytes("mine", base + 0x400, cc, nops, 2) == ANVIL_OK);
        CHECK(reg.expect_bytes("mine", addr, code, nullptr, sizeof(code)) == ANVIL_ERROR_WRONG_PHASE);
        CHECK(reg.expect_bytes("third", addr, code, nullptr, sizeof(code)) == ANVIL_OK);
        CHECK(reg.rollback("mine") == 1);
        CHECK(reg.expect_bytes("mine", addr, code, nullptr, sizeof(code)) == ANVIL_OK);
        CHECK(reg.rollback("other") == 2);
    });

    run("expectations: per build", [] {
        Scratch s(0x1000);
        const uintptr_t base = reinterpret_cast<uintptr_t>(s.base);
        PatchRegistry reg(base, 0x1000);
        const uint8_t code[] = {0x48, 0x89, 0x5C, 0x24, 0x08};
        std::memcpy(s.base + 0x100, code, sizeof(code));
        auto parsed = parse_manifest_text(
            plugin_toml("dep", "[[expect]]\nbuild = \"eu4-1.37.5-steam-win64\"\nrva = 0x100\nbytes = \"48 89 5C 24 ??\"\n"
                               "[[expect]]\nbuild = \"eu4-1.38.0-steam-win64\"\nrva = 0x100\nbytes = \"E9\"\nname = \"Foo\"\n"
                               "[[expect]]\nbuild = \"*\"\nrva = 0x200\nbytes = \"CC CC\"\n"),
            L"C:\\p");
        CHECK(parsed.manifest.has_value());
        if (!parsed.manifest) return;
        size_t checked = 0;
        CHECK(check_expectations(*parsed.manifest, "eu4-1.37.5-steam-win64", reg, base, checked).empty());
        CHECK(checked == 2);
        const std::string reason = check_expectations(*parsed.manifest, "eu4-1.38.0-steam-win64", reg, base, checked);
        CHECK(reason.find("Foo (rva 0x100, 1 B)") != std::string::npos);
        CHECK(reason.find("expected 0xE9, found 0x48") != std::string::npos);
        CHECK(check_expectations(*parsed.manifest, nullptr, reg, base, checked).empty() && checked == 1);
        auto outside = parse_manifest_text(plugin_toml("far", "[[expect]]\nbuild = \"*\"\nrva = 0x5000\nbytes = \"CC\"\n"),
                                           L"C:\\p");
        CHECK(outside.manifest &&
              check_expectations(*outside.manifest, nullptr, reg, base, checked).find("outside the game image") !=
                  std::string::npos);
    });

    run("resolver: verify skips plugins and dependents", [] {
        ResolveInput in;
        in.host_api = 1;
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
    });

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
