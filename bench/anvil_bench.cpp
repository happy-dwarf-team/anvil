// anvil_bench: microbenchmarks per kind of change (design §17).
//
// Each case is a tiny function in executable scratch memory, changed through
// the real ChangeRegistry and called in a dependent loop (x = f(x)) through a
// function pointer. Cases are measured interleaved, one sample of each per
// repetition, so drift (turbo, other load) hits every case alike; the result is
// the median of the repetitions in ns per call. The parser and resolver cases
// time a typical plugin set per iteration instead of one call.
//
// Output: a JSON document (stdout or --out) with every case's median and its
// ratio to the plain call of the same run, which is what the CI gate compares
// (bench/check-regression.ps1), plus the per-link costs against the budgets.
// Not a ctest: CI builds and runs it as its own step; the lab runs it pinned
// (bench/run-lab.ps1).
#include "core/changes.hpp"
#include "core/manifest.hpp"
#include "core/resolver.hpp"
#include "core/symbols.hpp"

#include <intrin.h>
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

using namespace anvil;
using Fn = uint32_t (*)(uint32_t);

constexpr int kLinks = 8;  // "N links" in the chained cases

// Executable scratch "image" for the registry.
struct Scratch {
    uint8_t* base;
    size_t size;
    explicit Scratch(size_t bytes) : size(bytes) {
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        if (base == nullptr) std::abort();
        std::memset(base, 0xCC, bytes);
    }
    ~Scratch() { VirtualFree(base, 0, MEM_RELEASE); }
    uintptr_t at(size_t offset) const { return reinterpret_cast<uintptr_t>(base) + offset; }
    Fn fn(size_t offset) const { return reinterpret_cast<Fn>(base + offset); }
    void put(size_t offset, const std::vector<uint8_t>& code) {
        std::memcpy(base + offset, code.data(), code.size());
        FlushInstructionCache(GetCurrentProcess(), base + offset, code.size());
    }
};

// f(x) = x + 1: lea eax, [rcx+1]; 9x nop; ret. 5+ bytes to steal, no relative operands.
const std::vector<uint8_t> kAddOne = {0x8D, 0x41, 0x01, 0x90, 0x90, 0x90, 0x90, 0x90,
                                      0x90, 0x90, 0x90, 0x90, 0xC3};
// f(x) = x + 15, one instruction per step:
// 0x00 lea eax, [rcx+1]; 0x03 add eax, 2; 0x06 add eax, 4; 0x09 add eax, 8; 0x0C ret
const std::vector<uint8_t> kSteps = {0x8D, 0x41, 0x01, 0x83, 0xC0, 0x02, 0x83, 0xC0,
                                     0x04, 0x83, 0xC0, 0x08, 0xC3};
constexpr size_t kBlockAt = 0x03, kBlockSize = 9;

// A caller: sub rsp, 28h; call rel32 callee; add rsp, 28h; ret. The call is at +4.
std::vector<uint8_t> caller(uintptr_t at, uintptr_t callee) {
    const auto rel = static_cast<int32_t>(static_cast<intptr_t>(callee) - static_cast<intptr_t>(at + 4 + 5));
    std::vector<uint8_t> code = {0x48, 0x83, 0xEC, 0x28, 0xE8, 0, 0, 0, 0, 0x48, 0x83, 0xC4, 0x28, 0xC3};
    std::memcpy(&code[5], &rel, sizeof(rel));
    return code;
}
constexpr size_t kCallAt = 4;

// Detours that only pass on through their own next: what a link costs when
// the plugin does nothing (MSVC /O2 makes each a `jmp [next]`).
template <int I> Fn g_next = nullptr;
template <int I> uint32_t detour(uint32_t x) { return g_next<I>(x); }

template <int... I> void detours(std::integer_sequence<int, I...>, std::vector<void*>& fns, std::vector<void**>& nexts) {
    (fns.push_back(reinterpret_cast<void*>(&detour<I>)), ...);
    (nexts.push_back(reinterpret_cast<void**>(&g_next<I>)), ...);
}

// Mid hook callbacks: empty, and the block replacement's add of 2 + 4 + 8.
void empty_callback(AnvilCpuContext*, void*) {}
void add_fourteen(AnvilCpuContext* cpu, void*) { cpu->rax = static_cast<uint32_t>(cpu->rax + 14); }

volatile uint32_t g_sink = 0;

// The timed loop: n dependent calls; returns the elapsed ticks and checks the result.
__declspec(noinline) int64_t time_calls(Fn f, uint32_t delta, uint64_t n, bool& correct) {
    LARGE_INTEGER t0{}, t1{};
    uint32_t x = 7;
    QueryPerformanceCounter(&t0);
    for (uint64_t i = 0; i < n; ++i) x = f(x);
    QueryPerformanceCounter(&t1);
    g_sink = x;
    correct = correct && x == static_cast<uint32_t>(7 + delta * n);
    return t1.QuadPart - t0.QuadPart;
}

// A case: one iteration is one call, or one pass over a plugin set.
struct Case {
    std::string name;
    std::string what;
    std::function<int64_t(uint64_t, bool&)> run;  // runs n iterations, returns ticks
    uint64_t iterations = 0;                     // per sample, from calibration
    std::vector<double> samples;                 // ns per iteration
    bool correct = true;                         // every run gave the expected result
    double median = 0, min = 0, max = 0;
};

Case call_case(std::string name, std::string what, Fn f, uint32_t delta) {
    return {std::move(name), std::move(what), [f, delta](uint64_t n, bool& ok) { return time_calls(f, delta, n, ok); }};
}

// --- the typical plugin set for the parser and resolver cases: the two
// standard plugins and eight community plugins with dependencies, optional
// dependencies, load_after and a conflict, as a player's folder would have.
// Top-level keys go before the [[dependencies]] and [[conflicts]] tables.
std::vector<std::string> plugin_set() {
    auto toml = [](const std::string& id, const std::string& phase, const std::string& extra) {
        return "id = \"" + id + "\"\nname = \"" + id + " for EU4\"\nversion = \"1.4.2\"\nauthors = [\"someone\"]\n"
               "description = \"A plugin used by the start-up benchmark.\"\napi = 2\nanvil = \">=1.0.0\"\nentry = \"" +
               id + ".dll\"\nphase = \"" + phase +
               "\"\naffects_gameplay = false\nissues = \"https://example.invalid/" + id +
               "/issues\"\nrepository = \"https://example.invalid/" + id + "\"\n" + extra;
    };
    auto dep = [](const std::string& id, const std::string& range, bool optional = false) {
        return "[[dependencies]]\nid = \"" + id + "\"\nversion = \"" + range + "\"\n" +
               (optional ? "optional = true\n" : "");
    };
    return {
        toml("anvil-game", "early", ""),
        toml("anvil-overlay", "main", dep("anvil-game", ">=1.0.0")),
        toml("unicode-patch", "early", dep("anvil-game", ">=1.0.0")),
        toml("faster-ui", "main", "load_after = [\"unicode-patch\"]\n" + dep("anvil-overlay", ">=1.0.0")),
        toml("map-tweaks", "main", dep("anvil-game", "^1.0.0") + dep("faster-ui", ">=1.2.0", true)),
        toml("ledger-plus", "main", dep("anvil-overlay", ">=1.0.0") + dep("map-tweaks", ">=1.0.0", true)),
        toml("speed-control", "main", "load_after = [\"faster-ui\", \"map-tweaks\"]\n" + dep("anvil-game", ">=1.0.0")),
        toml("trade-view", "main", dep("ledger-plus", ">=1.4.0") + "[[conflicts]]\nid = \"old-trade-view\"\n"),
        toml("music-shuffle", "main", ""),
        toml("save-guard", "early", "load_after = [\"unicode-patch\"]\n" + dep("anvil-game", ">=1.0.0")),
    };
}

// A symbols.toml of a mid-sized plugin: 40 code symbols and 8 slots.
std::string symbols_file() {
    std::string text = "build = \"eu4-1.37.5-steam-win64\"\n";
    char line[256];
    for (int i = 0; i < 40; ++i) {
        std::snprintf(line, sizeof(line),
                      "\n[symbol.function_%02d]\nrva = 0x%X\nbytes = \"48 8B 81 18 04 00 00 ?? 48 85 C0 74 0A\"\n"
                      "memory = \"code\"\nsignature = \"void* (void* self, int index)\"\n",
                      i, 0x400000 + i * 0x340);
        text += line;
    }
    for (int i = 0; i < 8; ++i) {
        std::snprintf(line, sizeof(line), "\n[symbol.slot_%d]\nrva = 0x%X\nmemory = \"read\"\npoints_to = 0x%X\n", i,
                      0x1D5A800 + i * 8, 0x10D2E80 + i * 0x10);
        text += line;
    }
    return text;
}

int64_t ticks_now() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double median_of(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

std::string cpu_brand() {
    int regs[4] = {};
    char brand[49] = {};
    __cpuid(regs, 0x80000000);
    if (static_cast<unsigned>(regs[0]) < 0x80000004u) return "unknown";
    for (int i = 0; i < 3; ++i) {
        __cpuid(regs, 0x80000002 + i);
        std::memcpy(brand + i * 16, regs, 16);
    }
    std::string s = brand;
    s.erase(0, s.find_first_not_of(' '));
    s.erase(s.find_last_not_of(' ') + 1);
    return s;
}

std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

void usage() {
    std::fprintf(stderr,
                 "Usage: anvil_bench [--repetitions N] [--sample-ms MS] [--cpu N] [--out FILE]\n"
                 "  --repetitions N  samples per case, median reported (default 31)\n"
                 "  --sample-ms MS   target length of one sample (default 2)\n"
                 "  --cpu N          pin the thread to logical CPU N, high priority (the lab uses 4)\n"
                 "  --out FILE       write the JSON there instead of stdout\n");
}

}  // namespace

int main(int argc, char** argv) {
    int repetitions = 31;
    double sample_ms = 2.0;
    int cpu = -1;
    const char* out_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--repetitions" && has_value) repetitions = std::atoi(argv[++i]);
        else if (arg == "--sample-ms" && has_value) sample_ms = std::atof(argv[++i]);
        else if (arg == "--cpu" && has_value) cpu = std::atoi(argv[++i]);
        else if (arg == "--out" && has_value) out_path = argv[++i];
        else {
            usage();
            return 2;
        }
    }
    if (repetitions < 1 || sample_ms <= 0 || cpu < -1 || cpu > 63) {
        usage();
        return 2;
    }
    if (cpu >= 0 && SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << cpu) == 0) {
        std::fprintf(stderr, "anvil_bench: cannot pin to CPU %d\n", cpu);
        return 1;
    }
    if (cpu >= 0) SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    // --- the code under test
    Scratch s(0x1000);
    ChangeRegistry reg(s.at(0), static_cast<uint32_t>(s.size));
    reg.set_refusal_sink([](OwnerId, const std::string& m) { std::fprintf(stderr, "anvil_bench: refused: %s\n", m.c_str()); });
    std::vector<OwnerId> owners;
    for (int i = 0; i < kLinks; ++i) owners.push_back(reg.add_owner("p" + std::to_string(i)));
    std::vector<void*> fns;
    std::vector<void**> nexts;
    detours(std::make_integer_sequence<int, 4 * kLinks>{}, fns, nexts);
    int used = 0;
    bool setup = true;
    auto ok = [&](ChangeStatus st) { setup = setup && st == ChangeStatus::Ok; };
    auto at = [&](size_t offset) { return ChangeTarget{s.at(offset), true, nullptr, nullptr, 0}; };

    // 0x000 plain call; 0x040 / 0x080 function hook, 1 and N links.
    for (size_t off : {0x000, 0x040, 0x080}) s.put(off, kAddOne);
    ok(reg.hook_function(owners[0], at(0x040), fns[used], nexts[used]));
    ++used;
    for (int i = 0; i < kLinks; ++i, ++used) ok(reg.hook_function(owners[i], at(0x080), fns[used], nexts[used]));
    // 0x0C0 / 0x100 / 0x140: callers of 0x180 / 0x1C0 / 0x200; plain, redirected by 1 and N links.
    for (size_t i = 0; i < 3; ++i) {
        s.put(0x180 + 0x40 * i, kAddOne);
        s.put(0x0C0 + 0x40 * i, caller(s.at(0x0C0 + 0x40 * i), s.at(0x180 + 0x40 * i)));
    }
    ok(reg.redirect_call(owners[0], at(0x100 + kCallAt), s.at(0x1C0), fns[used], nexts[used]));
    ++used;
    for (int i = 0; i < kLinks; ++i, ++used) {
        ok(reg.redirect_call(owners[i], at(0x140 + kCallAt), s.at(0x200), fns[used], nexts[used]));
    }
    // 0x240 / 0x280: mid hooks, 1 and N links, at a function start.
    for (size_t off : {0x240, 0x280}) s.put(off, kAddOne);
    ok(reg.mid_hook(owners[0], at(0x240), &empty_callback, nullptr));
    for (int i = 0; i < kLinks; ++i) ok(reg.mid_hook(owners[i], at(0x280), &empty_callback, nullptr));
    // 0x2C0 / 0x300: the three adds run natively, or a block replacement does them.
    for (size_t off : {0x2C0, 0x300}) s.put(off, kSteps);
    ok(reg.replace_block(owners[0], at(0x300 + kBlockAt), kBlockSize, 0, &add_fourteen, nullptr));
    if (!setup) {
        std::fprintf(stderr, "anvil_bench: a change was refused\n");
        return 1;
    }

    const std::vector<std::string> manifests = plugin_set();
    const std::string symbols = symbols_file();
    for (const auto& text : manifests) {
        if (const auto r = parse_manifest_text(text, L"C:\\p"); !r.manifest) {
            std::fprintf(stderr, "anvil_bench: plugin set: %s (line %u)\n", r.error.c_str(), r.line);
            return 1;
        }
    }
    if (const auto r = parse_symbols_text(symbols); !r.file) {
        std::fprintf(stderr, "anvil_bench: symbols.toml: %s (line %u)\n", r.error.c_str(), r.line);
        return 1;
    }
    std::vector<Case> cases = {
        call_case("call", "plain call (reference for the ratios)", s.fn(0x000), 1),
        call_case("function_hook_1", "function hook, 1 link", s.fn(0x040), 1),
        call_case("function_hook_" + std::to_string(kLinks), "function hook, N links", s.fn(0x080), 1),
        call_case("call_site", "plain call through a call rel32 site", s.fn(0x0C0), 1),
        call_case("call_redirect_1", "call redirect, 1 link", s.fn(0x100), 1),
        call_case("call_redirect_" + std::to_string(kLinks), "call redirect, N links", s.fn(0x140), 1),
        call_case("mid_hook_1", "mid hook, 1 link, empty callback", s.fn(0x240), 1),
        call_case("mid_hook_" + std::to_string(kLinks), "mid hook, N links, empty callbacks", s.fn(0x280), 1),
        call_case("block_native", "the block's own instructions", s.fn(0x2C0), 15),
        call_case("block_replacement", "block replacement (callback does the block's work)", s.fn(0x300), 15),
        {"manifest_parse_set", "parse the 10 plugin.toml of a typical set",
         [&](uint64_t n, bool& correct) {
             const int64_t t0 = ticks_now();
             for (uint64_t i = 0; i < n; ++i) {
                 for (const auto& text : manifests) correct = correct && parse_manifest_text(text, L"C:\\p").manifest;
             }
             return ticks_now() - t0;
         }},
        {"resolve_set", "parse and resolve the typical set",
         [&](uint64_t n, bool& correct) {
             const int64_t t0 = ticks_now();
             for (uint64_t i = 0; i < n; ++i) {
                 ResolveInput input;
                 input.host_api = kManifestApi;
                 input.host_version = Version::parse("1.0.0");
                 input.game_build = "eu4-1.37.5-steam-win64";
                 for (const auto& text : manifests) input.manifests.push_back(*parse_manifest_text(text, L"C:\\p").manifest);
                 correct = correct && resolve(input).order.size() == manifests.size();
             }
             return ticks_now() - t0;
         }},
        {"symbols_parse", "parse a symbols.toml with 48 symbols",
         [&](uint64_t n, bool& correct) {
             const int64_t t0 = ticks_now();
             for (uint64_t i = 0; i < n; ++i) {
                 const auto r = parse_symbols_text(symbols);
                 correct = correct && r.file && r.file->symbols.size() == 48;
             }
             return ticks_now() - t0;
         }},
    };

    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const double ns_per_tick = 1e9 / static_cast<double>(frequency.QuadPart);
    // Calibration (also the warm-up): grow n until one run takes sample_ms.
    for (auto& c : cases) {
        uint64_t n = 1;
        for (;;) {
            const double ms = static_cast<double>(c.run(n, c.correct)) * ns_per_tick / 1e6;
            if (ms >= sample_ms / 4 || n >= (uint64_t{1} << 34)) {
                c.iterations = std::max<uint64_t>(1, static_cast<uint64_t>(static_cast<double>(n) * sample_ms / std::max(ms, 1e-3)));
                break;
            }
            n *= 4;
        }
    }
    for (int r = 0; r < repetitions; ++r) {
        for (auto& c : cases) {
            const double ns = static_cast<double>(c.run(c.iterations, c.correct)) * ns_per_tick;
            c.samples.push_back(ns / static_cast<double>(c.iterations));
        }
    }
    bool correct = true;
    for (const auto& c : cases) {
        if (!c.correct) std::fprintf(stderr, "anvil_bench: %s returned a wrong result\n", c.name.c_str());
        correct = correct && c.correct;
    }
    if (!correct) return 1;
    for (auto& c : cases) {
        c.median = median_of(c.samples);
        c.min = *std::min_element(c.samples.begin(), c.samples.end());
        c.max = *std::max_element(c.samples.begin(), c.samples.end());
    }
    auto median = [&](const std::string& name) {
        for (const auto& c : cases) {
            if (c.name == name) return c.median;
        }
        return 0.0;
    };
    const std::string n = std::to_string(kLinks);
    const double reference = median("call");

    // Per-link costs against design §17: the first link over the unchanged
    // code, each further link as the slope from 1 to N links.
    struct Budget {
        const char* name;
        double ns;
        double budget;
    };
    const double links = kLinks - 1;
    const std::vector<Budget> budgets = {
        {"function_hook_first_link", median("function_hook_1") - median("call"), 2.0},
        {"function_hook_further_link", (median("function_hook_" + n) - median("function_hook_1")) / links, 2.0},
        {"call_redirect_first_link", median("call_redirect_1") - median("call_site"), 2.0},
        {"call_redirect_further_link", (median("call_redirect_" + n) - median("call_redirect_1")) / links, 2.0},
        {"mid_hook_first_link", median("mid_hook_1") - median("call"), 25.0},
        {"mid_hook_further_link", (median("mid_hook_" + n) - median("mid_hook_1")) / links, 20.0},
        {"block_replacement", median("block_replacement") - median("block_native"), 25.0},
    };

    std::string json = "{\n  \"format\": 1,\n  \"anvil\": \"" ANVIL_VERSION "\",\n";
    json += "  \"cpu\": \"" + json_escape(cpu_brand()) + "\",\n";
    json += "  \"pinned_cpu\": " + (cpu >= 0 ? std::to_string(cpu) : std::string("null")) + ",\n";
    json += "  \"repetitions\": " + std::to_string(repetitions) + ",\n";
    json += "  \"links\": " + n + ",\n";
    json += "  \"unit\": \"ns per iteration (one call, or one pass over the plugin set), median of the repetitions\",\n";
    json += "  \"cases\": {\n";
    char buffer[512];
    for (size_t i = 0; i < cases.size(); ++i) {
        const auto& c = cases[i];
        std::snprintf(buffer, sizeof(buffer),
                      "    \"%s\": {\"median_ns\": %.3f, \"min_ns\": %.3f, \"max_ns\": %.3f, \"ratio\": %.4f, "
                      "\"iterations\": %llu, \"what\": \"%s\"}%s\n",
                      c.name.c_str(), c.median, c.min, c.max, c.median / reference,
                      static_cast<unsigned long long>(c.iterations), json_escape(c.what).c_str(),
                      i + 1 < cases.size() ? "," : "");
        json += buffer;
    }
    json += "  },\n  \"budgets\": {\n";
    for (size_t i = 0; i < budgets.size(); ++i) {
        const auto& b = budgets[i];
        std::snprintf(buffer, sizeof(buffer), "    \"%s\": {\"ns\": %.3f, \"budget_ns\": %.1f, \"within\": %s}%s\n", b.name,
                      b.ns, b.budget, b.ns <= b.budget ? "true" : "false", i + 1 < budgets.size() ? "," : "");
        json += buffer;
    }
    json += "  }\n}\n";

    if (out_path != nullptr) {
        FILE* f = nullptr;
        if (fopen_s(&f, out_path, "wb") != 0 || f == nullptr) {
            std::fprintf(stderr, "anvil_bench: cannot write %s\n", out_path);
            return 1;
        }
        std::fputs(json.c_str(), f);
        std::fclose(f);
    } else {
        std::fputs(json.c_str(), stdout);
    }
    // A short table on stderr for people.
    std::fprintf(stderr, "%-28s %12s %8s\n", "case", "median ns", "ratio");
    for (const auto& c : cases) std::fprintf(stderr, "%-28s %12.2f %8.2f\n", c.name.c_str(), c.median, c.median / reference);
    std::fprintf(stderr, "%-28s %12s %8s\n", "per link (design 17)", "ns", "budget");
    for (const auto& b : budgets) std::fprintf(stderr, "%-28s %12.2f %8.1f%s\n", b.name, b.ns, b.budget, b.ns <= b.budget ? "" : "  OVER");
    return 0;
}
