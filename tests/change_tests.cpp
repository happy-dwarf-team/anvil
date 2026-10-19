// Change registry tests (design §8): chains, mid hooks, block replacements,
// pointer swaps, conflicts, shared byte patches, staging, handles, rollback,
// suspension, call counters and `*_at`, on code written into scratch memory.
#include "core/changes.hpp"
#include "core/discovery.hpp"
#include "core/suspension.hpp"
#include "test_support.hpp"

#include <intrin.h>
#include <windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

namespace {

using namespace anvil;

// Executable scratch "image".
struct Scratch {
    uint8_t* base;
    size_t size;
    explicit Scratch(size_t bytes) : size(bytes) {
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        std::memset(base, 0xCC, bytes);
    }
    ~Scratch() { VirtualFree(base, 0, MEM_RELEASE); }
    uintptr_t at(size_t offset) const { return reinterpret_cast<uintptr_t>(base) + offset; }
    void put(size_t offset, const std::vector<uint8_t>& code) {
        std::memcpy(base + offset, code.data(), code.size());
        FlushInstructionCache(GetCurrentProcess(), base + offset, code.size());
    }
};

// int f(int x) { return x + 1; }: lea eax, [rcx+1]; 9x nop; ret
const std::vector<uint8_t> kAddOne = {0x8D, 0x41, 0x01, 0x90, 0x90, 0x90, 0x90, 0x90,
                                      0x90, 0x90, 0x90, 0x90, 0xC3};
using Fn = int (*)(int);

ChangeTarget in_image(uintptr_t address, const std::vector<uint8_t>& expected = {}) {
    return {address, true, expected.empty() ? nullptr : expected.data(), nullptr, expected.size()};
}

ChangeTarget outside(uintptr_t address, const std::vector<uint8_t>& expected = {}) {
    return {address, false, expected.empty() ? nullptr : expected.data(), nullptr, expected.size()};
}

// Detours: each records itself and passes on through its own next.
std::string g_trace;
Fn g_next_a = nullptr, g_next_b = nullptr, g_next_c = nullptr, g_next_a2 = nullptr;
int detour_a(int x) { g_trace += 'a'; return g_next_a(x) + 100; }
int detour_a2(int x) { g_trace += 'A'; return g_next_a2(x) + 1000; }
int detour_b(int x) { g_trace += 'b'; return g_next_b(x) * 2; }
int detour_c(int x) { g_trace += 'c'; return g_next_c(x) + 10; }

int traced(Fn f, int x) {
    g_trace.clear();
    return f(x);
}

void** slot(Fn& next) { return reinterpret_cast<void**>(&next); }

using TickFn = DWORD(WINAPI*)();
TickFn g_tick_next_a = nullptr, g_tick_next_b = nullptr;
DWORD WINAPI tick_a() { g_trace += 'a'; return g_tick_next_a() == 7u ? 70u : 0u; }
DWORD WINAPI tick_b() { g_trace += 'b'; return 7u; }

// int f(int x) { return x + 1 + 2 + 4 + 8; }, one instruction per step:
// 0x00 lea eax, [rcx+1]; 0x03 add eax, 2; 0x06 add eax, 4; 0x09 add eax, 8; 0x0C ret
const std::vector<uint8_t> kSteps = {0x8D, 0x41, 0x01, 0x83, 0xC0, 0x02, 0x83, 0xC0,
                                     0x04, 0x83, 0xC0, 0x08, 0xC3};

// double g(double x) { return x + xmm1; }: addsd xmm0, xmm1; nop; ret
const std::vector<uint8_t> kAddXmm1 = {0xF2, 0x0F, 0x58, 0xC1, 0x90, 0xC3};
using FnD = double (*)(double);

// What a mid hook or block callback does: record its tag, optionally write
// rax and rip.
struct Probe {
    char tag;
    uintptr_t rip = 0;  // 0: leave rip alone
    uintptr_t rax = 0;  // written with rip
    uintptr_t add = 0;  // added to rax
};

void probe(AnvilCpuContext* cpu, void* user) {
    const auto* p = static_cast<const Probe*>(user);
    g_trace += p->tag;
    cpu->rax += p->add;
    if (p->rip != 0) {
        cpu->rax = p->rax;
        cpu->rip = p->rip;
    }
}

// Seen in a mid hook at a function start, then rcx (the argument) replaced.
uintptr_t g_seen_rcx = 0, g_seen_rsp = 0, g_seen_return = 0;
void set_rcx(AnvilCpuContext* cpu, void* user) {
    g_trace += 'm';
    g_seen_rcx = cpu->rcx;
    g_seen_rsp = cpu->rsp;
    g_seen_return = *reinterpret_cast<const uintptr_t*>(cpu->rsp);
    cpu->rcx = reinterpret_cast<uintptr_t>(user);
}

void set_xmm1(AnvilCpuContext* cpu, void*) { cpu->xmm[1].f64[0] = 40.0; }

// A redirect detour that records where its call returns to.
void* g_return = nullptr;
Fn g_next_r = nullptr;
int detour_r(int x) {
    g_return = _ReturnAddress();
    g_trace += 'r';
    return g_next_r(x) * 2;
}

// Pointer swaps: values a slot can hold, and plugins' previous cells.
int g_values[4] = {};
void* g_prev_a = nullptr;
void* g_prev_b = nullptr;
void* g_prev_c = nullptr;
void* value(int i) { return &g_values[i]; }

DWORD protection(const void* address) {
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(address, &mbi, sizeof(mbi));
    return mbi.Protect;
}

// Through a swapped function pointer: each reads its previous on every call.
int shipped_fn(int x) { return x + 1; }
int swapped_a(int x) { g_trace += 'a'; return reinterpret_cast<Fn>(g_prev_a)(x) + 100; }
int swapped_b(int x) { g_trace += 'b'; return reinterpret_cast<Fn>(g_prev_b)(x) * 2; }

}  // namespace

void change_tests() {
    run("changes: function hook chain in load order with its own next per link", [] {
        Scratch s(0x1000);
        s.put(0x40, kAddOne);
        const auto f = reinterpret_cast<Fn>(s.base + 0x40);
        ChangeRegistry reg(s.at(0), 0x1000);
        const OwnerId a = reg.add_owner("a"), b = reg.add_owner("b"), c = reg.add_owner("c");
        ChangeId ha = 0, hb = 0, hc = 0;
        // Requested out of load order: the chain still follows the owners.
        CHECK(reg.hook_function(c, in_image(s.at(0x40), {0x8D, 0x41}), reinterpret_cast<void*>(&detour_c),
                                slot(g_next_c), &hc) == ChangeStatus::Ok);
        CHECK(traced(f, 1) == 12 && g_trace == "c");
        CHECK(reg.hook_function(a, in_image(s.at(0x40), kAddOne), reinterpret_cast<void*>(&detour_a), slot(g_next_a),
                                &ha) == ChangeStatus::Ok);
        CHECK(reg.hook_function(b, in_image(s.at(0x40)), reinterpret_cast<void*>(&detour_b), slot(g_next_b), &hb) ==
              ChangeStatus::Ok);
        CHECK(ha != 0 && hb != 0 && hc != 0 && ha != hb && hb != hc);
        CHECK(traced(f, 1) == 124 && g_trace == "abc");  // ((1 + 1 + 10) * 2) + 100
        CHECK((reg.chain_at(s.at(0x40)) == std::vector<OwnerId>{a, b, c}));
        CHECK(reg.count(a) == 1 && reg.count(b) == 1 && reg.count(c) == 1);

        // The middle link leaves; a's next now leads to c.
        CHECK(reg.remove(b, hb) == ChangeStatus::Ok);
        CHECK(traced(f, 1) == 112 && g_trace == "ac");
        // A removed link's next still works (a thread may still be inside it).
        CHECK(traced(g_next_b, 1) == 12 && g_trace == "c");
        CHECK(reg.remove(b, hb) == ChangeStatus::NotFound);  // stale handle
        CHECK(reg.remove(c, ha) == ChangeStatus::NotFound);  // not c's
        CHECK(reg.remove(a, 0) == ChangeStatus::NotFound);

        // The outermost link leaves, then b comes back between nobody.
        CHECK(reg.remove(a, ha) == ChangeStatus::Ok);
        CHECK(traced(f, 1) == 12 && g_trace == "c");
        CHECK(reg.hook_function(b, in_image(s.at(0x40), kAddOne), reinterpret_cast<void*>(&detour_b), slot(g_next_b),
                                &hb) == ChangeStatus::Ok);
        CHECK(traced(f, 1) == 24 && g_trace == "bc");

        // Same owner twice: the later request sits further in.
        CHECK(reg.hook_function(a, in_image(s.at(0x40)), reinterpret_cast<void*>(&detour_a), slot(g_next_a), &ha) ==
              ChangeStatus::Ok);
        ChangeId ha2 = 0;
        CHECK(reg.hook_function(a, in_image(s.at(0x40)), reinterpret_cast<void*>(&detour_a2), slot(g_next_a2), &ha2) ==
              ChangeStatus::Ok);
        CHECK(traced(f, 1) == 1124 && g_trace == "aAbc");
        CHECK((reg.chain_at(s.at(0x40)) == std::vector<OwnerId>{a, a, b, c}));

        // The bytes read as shipped while hooked; the last link restores them.
        CHECK(std::memcmp(s.base + 0x40, kAddOne.data(), 5) != 0);
        uint8_t shipped[13] = {};
        CHECK(reg.read_original(s.at(0x40), shipped, sizeof(shipped)));
        CHECK(std::memcmp(shipped, kAddOne.data(), sizeof(shipped)) == 0);
        CHECK(reg.remove(c, hc) == ChangeStatus::Ok);
        CHECK(reg.remove(a, ha) == ChangeStatus::Ok);
        CHECK(reg.remove(b, hb) == ChangeStatus::Ok);
        CHECK(traced(f, 1) == 1002 && g_trace == "A");
        CHECK(reg.remove(a, ha2) == ChangeStatus::Ok);
        CHECK(std::memcmp(s.base + 0x40, kAddOne.data(), kAddOne.size()) == 0);
        CHECK(traced(f, 1) == 2 && g_trace.empty());
        CHECK(reg.chain_at(s.at(0x40)).empty());

        // The site is reused: hooking again works after an empty chain.
        CHECK(reg.hook_function(c, in_image(s.at(0x40), kAddOne), reinterpret_cast<void*>(&detour_c), slot(g_next_c)) ==
              ChangeStatus::Ok);
        CHECK(traced(f, 1) == 12 && g_trace == "c");
        CHECK(reg.rollback(c) == 1);
        CHECK(traced(f, 1) == 2);
    });

    run("changes: function hooks check the original prologue and refuse unsafe sites", [] {
        Scratch s(0x1000);
        s.put(0x40, kAddOne);
        // jrcxz +2; ... and loop +2; ...: SafetyHook would mis-relocate them.
        s.put(0x80, {0xE3, 0x02, 0x90, 0x90, 0x90, 0x90, 0xC3});
        s.put(0xC0, {0x90, 0xE2, 0x02, 0x90, 0x90, 0x90, 0xC3});
        ChangeRegistry reg(s.at(0), 0x1000);
        std::vector<std::string> refusals;
        reg.set_refusal_sink([&](OwnerId, const std::string& m) { refusals.push_back(m); });
        const OwnerId a = reg.add_owner("a");
        Fn next = nullptr;
        CHECK(reg.hook_function(a, in_image(s.at(0x40), {0x8D, 0x42}), reinterpret_cast<void*>(&detour_a), slot(next)) ==
              ChangeStatus::BytesMismatch);
        CHECK(!refusals.empty() && refusals.back() == "function hook rva 0x40: byte +0x1: expected 0x42, found 0x41");
        CHECK(reg.hook_function(a, in_image(s.at(0x80)), reinterpret_cast<void*>(&detour_a), slot(next)) ==
              ChangeStatus::UnsafeSite);
        CHECK(reg.hook_function(a, in_image(s.at(0xC0)), reinterpret_cast<void*>(&detour_a), slot(next)) ==
              ChangeStatus::UnsafeSite);
        CHECK(refusals.back().find("JRCXZ/LOOP") != std::string::npos);
        CHECK(reg.hook_function(a, in_image(s.at(0xFFE)), reinterpret_cast<void*>(&detour_a), slot(next)) ==
              ChangeStatus::InvalidArgument);  // 5 bytes run past the image end
        CHECK(reg.hook_function(a, in_image(s.at(0x40)), nullptr, slot(next)) == ChangeStatus::InvalidArgument);
        CHECK(reg.hook_function(a, in_image(s.at(0x40)), reinterpret_cast<void*>(&detour_a), nullptr) ==
              ChangeStatus::InvalidArgument);
        CHECK(reg.hook_function(7, in_image(s.at(0x40)), reinterpret_cast<void*>(&detour_a), slot(next)) ==
              ChangeStatus::InvalidArgument);  // unknown owner
        CHECK(reg.count(a) == 0 && std::memcmp(s.base + 0x40, kAddOne.data(), kAddOne.size()) == 0);
    });

    run("changes: call redirect chain, next = the previous target", [] {
        Scratch s(0x1000);
        // int caller(int x) { return callee(x); }: sub rsp,28h; call 0x200; add rsp,28h; ret
        s.put(0x10, {0x48, 0x83, 0xEC, 0x28, 0xE8, 0, 0, 0, 0, 0x48, 0x83, 0xC4, 0x28, 0xC3});
        const int32_t rel = 0x200 - (0x14 + 5);
        std::memcpy(s.base + 0x15, &rel, 4);
        s.put(0x200, kAddOne);
        s.put(0x300, {0x90, 0x90, 0x90, 0x90, 0x90, 0xC3});
        const auto caller = reinterpret_cast<Fn>(s.base + 0x10);
        CHECK(caller(1) == 2);
        ChangeRegistry reg(s.at(0), 0x1000);
        const OwnerId a = reg.add_owner("a"), b = reg.add_owner("b");
        CHECK(reg.redirect_call(a, in_image(s.at(0x14)), s.at(0x300), reinterpret_cast<void*>(&detour_a),
                                slot(g_next_a)) == ChangeStatus::BytesMismatch);
        CHECK(reg.redirect_call(a, in_image(s.at(0x10)), 0, reinterpret_cast<void*>(&detour_a), slot(g_next_a)) ==
              ChangeStatus::WrongKind);  // not a call rel32
        ChangeId ha = 0, hb = 0;
        CHECK(reg.redirect_call(b, in_image(s.at(0x14)), s.at(0x200), reinterpret_cast<void*>(&detour_b),
                                slot(g_next_b), &hb) == ChangeStatus::Ok);
        CHECK(traced(caller, 1) == 4 && g_trace == "b");
        CHECK(reinterpret_cast<uintptr_t>(g_next_b) != s.at(0x200) && traced(g_next_b, 1) == 2);  // a stub to the callee
        // The original callee stays the expected one for a later plugin.
        CHECK(reg.redirect_call(a, in_image(s.at(0x14)), s.at(0x200), reinterpret_cast<void*>(&detour_a),
                                slot(g_next_a), &ha) == ChangeStatus::Ok);
        CHECK(traced(caller, 1) == 104 && g_trace == "ab");
        uint8_t shipped[5] = {};
        CHECK(reg.read_original(s.at(0x14), shipped, 5) && shipped[0] == 0xE8 && std::memcmp(shipped + 1, &rel, 4) == 0);
        CHECK(reg.remove(a, ha) == ChangeStatus::Ok);  // the outer one first
        CHECK(traced(caller, 1) == 4 && g_trace == "b");
        CHECK(reg.redirect_call(a, in_image(s.at(0x14)), 0, reinterpret_cast<void*>(&detour_a), slot(g_next_a), &ha) ==
              ChangeStatus::Ok);
        CHECK(reg.remove(b, hb) == ChangeStatus::Ok);  // now the inner one
        CHECK(traced(caller, 1) == 102 && g_trace == "a");
        CHECK(reg.rollback(a) == 1);
        int32_t now = 0;
        std::memcpy(&now, s.base + 0x15, 4);
        CHECK(s.base[0x14] == 0xE8 && now == rel && traced(caller, 1) == 2);
    });

    run("changes: byte patches, shared when identical", [] {
        Scratch s(0x1000);
        ChangeRegistry reg(s.at(0), 0x1000);
        std::vector<std::string> refusals;
        reg.set_refusal_sink([&](OwnerId, const std::string& m) { refusals.push_back(m); });
        const OwnerId a = reg.add_owner("a"), b = reg.add_owner("b"), c = reg.add_owner("c");
        const std::vector<uint8_t> cc = {0xCC, 0xCC, 0xCC, 0xCC};
        const uint8_t patch[4] = {1, 2, 3, 4};
        const uint8_t other[4] = {1, 2, 3, 5};
        ChangeId ha = 0, hb = 0;
        CHECK(reg.patch_bytes(a, in_image(s.at(0x100), cc), patch, 4, &ha) == ChangeStatus::Ok);
        CHECK(std::memcmp(s.base + 0x100, patch, 4) == 0);
        // The same bytes at the same address: shared. Expected bytes are the original ones.
        CHECK(reg.patch_bytes(b, in_image(s.at(0x100), cc), patch, 4, &hb) == ChangeStatus::Ok);
        CHECK(reg.count(a) == 1 && reg.count(b) == 1);
        CHECK((reg.chain_at(s.at(0x100)) == std::vector<OwnerId>{a, b}));
        // Different bytes at the same address, or an overlap at another address: refused.
        CHECK(reg.patch_bytes(c, in_image(s.at(0x100)), other, 4) == ChangeStatus::Conflict);
        CHECK(refusals.back() ==
              "byte patch rva 0x100 (4 B) overlaps the byte patch rva 0x100 (4 B) of 'a', 'b' with different bytes; "
              "refused");
        CHECK(reg.patch_bytes(c, in_image(s.at(0x102)), patch, 4) == ChangeStatus::Conflict);
        CHECK(refusals.back().find("of 'a', 'b'") != std::string::npos);
        CHECK(reg.patch_bytes(c, in_image(s.at(0x100)), patch, 2) == ChangeStatus::Conflict);  // a different size
        CHECK(reg.patch_bytes(a, in_image(s.at(0x0FE)), patch, 4) == ChangeStatus::Conflict);  // the owner itself too
        CHECK(reg.patch_bytes(c, in_image(s.at(0x110), {1, 2, 3, 4}), patch, 4) == ChangeStatus::BytesMismatch);
        CHECK(reg.patch_bytes(c, in_image(s.at(0xFFE)), patch, 4) == ChangeStatus::InvalidArgument);
        CHECK(reg.patch_bytes(c, in_image(s.at(0x200)), patch, 0) == ChangeStatus::InvalidArgument);

        // One owner leaves: the bytes stay for the other.
        CHECK(reg.remove(a, ha) == ChangeStatus::Ok);
        CHECK(std::memcmp(s.base + 0x100, patch, 4) == 0);
        CHECK(reg.patch_bytes(c, in_image(s.at(0x102)), patch, 4) == ChangeStatus::Conflict);
        CHECK(reg.remove(b, hb) == ChangeStatus::Ok);
        CHECK(std::memcmp(s.base + 0x100, cc.data(), 4) == 0);
        CHECK(reg.patch_bytes(c, in_image(s.at(0x102), cc), other, 4) == ChangeStatus::Ok);  // free again
        CHECK(reg.rollback(c) == 1 && std::memcmp(s.base + 0x102, cc.data(), 4) == 0);
    });

    run("changes: overlaps between kinds name both owners", [] {
        Scratch s(0x1000);
        s.put(0x40, kAddOne);
        ChangeRegistry reg(s.at(0), 0x1000);
        std::vector<std::pair<OwnerId, std::string>> refusals;
        reg.set_refusal_sink([&](OwnerId o, const std::string& m) { refusals.push_back({o, m}); });
        const OwnerId a = reg.add_owner("a"), b = reg.add_owner("b");
        Fn next = nullptr;
        CHECK(reg.hook_function(a, in_image(s.at(0x40)), reinterpret_cast<void*>(&detour_a), slot(g_next_a)) ==
              ChangeStatus::Ok);
        const uint8_t two[2] = {0x41, 0x01};
        CHECK(reg.patch_bytes(b, in_image(s.at(0x41)), two, 2) == ChangeStatus::Conflict);
        CHECK(refusals.back().first == b &&
              refusals.back().second == "byte patch rva 0x41 (2 B) overlaps the function hook rva 0x40 (5 B) of 'a'; "
                                        "refused");
        CHECK(reg.patch_bytes(b, in_image(s.at(0x40)), two, 2) == ChangeStatus::Conflict);  // same address, other kind
        CHECK(reg.hook_function(b, in_image(s.at(0x43)), reinterpret_cast<void*>(&detour_b), slot(next)) ==
              ChangeStatus::Conflict);
        CHECK(reg.patch_bytes(b, in_image(s.at(0x45)), two, 2) == ChangeStatus::Ok);  // past the 5 stolen bytes
        CHECK(reg.hook_function(a, in_image(s.at(0x46)), reinterpret_cast<void*>(&detour_b), slot(next)) ==
              ChangeStatus::Conflict);
        CHECK(refusals.back().first == a && refusals.back().second.find("of 'b'") != std::string::npos);
        CHECK(reg.rollback(a) == 1 && reg.rollback(b) == 1);
        CHECK(std::memcmp(s.base + 0x40, kAddOne.data(), kAddOne.size()) == 0);
    });

    run("changes: staging enables a group together or not at all", [] {
        Scratch s(0x2000);
        s.put(0x40, kAddOne);
        const auto f = reinterpret_cast<Fn>(s.base + 0x40);
        ChangeRegistry reg(s.at(0), 0x2000);
        const OwnerId a = reg.add_owner("a"), b = reg.add_owner("b");
        const uint8_t patch[2] = {0x90, 0x90};
        CHECK(reg.commit_group(a) == ChangeStatus::WrongPhase);
        CHECK(reg.begin_group(a) == ChangeStatus::Ok);
        CHECK(reg.begin_group(a) == ChangeStatus::WrongPhase);
        CHECK(reg.group_open(a) && !reg.group_open(b));
        CHECK(reg.hook_function(a, in_image(s.at(0x40), kAddOne), reinterpret_cast<void*>(&detour_a), slot(g_next_a)) ==
              ChangeStatus::Ok);
        CHECK(reg.patch_bytes(a, in_image(s.at(0x100)), patch, 2) == ChangeStatus::Ok);
        // Staged: nothing changed yet, but next already leads to the original.
        CHECK(reg.staged_count(a) == 2 && reg.count(a) == 0);
        CHECK(std::memcmp(s.base + 0x40, kAddOne.data(), kAddOne.size()) == 0 && s.base[0x100] == 0xCC);
        CHECK(traced(f, 1) == 2 && g_trace.empty());
        CHECK(traced(g_next_a, 1) == 2);
        // Staged changes reserve their bytes against other owners.
        CHECK(reg.patch_bytes(b, in_image(s.at(0x101)), patch, 2) == ChangeStatus::Conflict);
        CHECK(reg.commit_group(a) == ChangeStatus::Ok);
        CHECK(!reg.group_open(a) && reg.staged_count(a) == 0 && reg.count(a) == 2);
        CHECK(traced(f, 1) == 102 && g_trace == "a" && s.base[0x100] == 0x90);

        // Aborted: none of the group is enabled, and its bytes are free again.
        CHECK(reg.begin_group(b) == ChangeStatus::Ok);
        CHECK(reg.hook_function(b, in_image(s.at(0x40)), reinterpret_cast<void*>(&detour_b), slot(g_next_b)) ==
              ChangeStatus::Ok);
        CHECK(reg.patch_bytes(b, in_image(s.at(0x200)), patch, 2) == ChangeStatus::Ok);
        reg.abort_group(b);
        CHECK(reg.count(b) == 0 && reg.staged_count(b) == 0 && s.base[0x200] == 0xCC);
        CHECK(traced(f, 1) == 102 && g_trace == "a");
        CHECK(reg.patch_bytes(a, in_image(s.at(0x200)), patch, 2) == ChangeStatus::Ok);

        // A group whose memory cannot be written at commit: none enabled.
        CHECK(reg.begin_group(b) == ChangeStatus::Ok);
        CHECK(reg.hook_function(b, in_image(s.at(0x40)), reinterpret_cast<void*>(&detour_b), slot(g_next_b)) ==
              ChangeStatus::Ok);
        CHECK(reg.patch_bytes(b, in_image(s.at(0x300)), patch, 2) == ChangeStatus::Ok);
        CHECK(reg.patch_bytes(b, in_image(s.at(0x1000)), patch, 2) == ChangeStatus::Ok);
        CHECK(VirtualFree(s.base + 0x1000, 0x1000, MEM_DECOMMIT));
        CHECK(reg.commit_group(b) == ChangeStatus::Internal);
        CHECK(reg.count(b) == 0 && s.base[0x300] == 0xCC);
        CHECK(traced(f, 1) == 102 && g_trace == "a");
        CHECK((reg.chain_at(s.at(0x40)) == std::vector<OwnerId>{a}));
    });

    run("changes: rollback removes staged and enabled changes of one owner", [] {
        Scratch s(0x1000);
        s.put(0x40, kAddOne);
        const auto f = reinterpret_cast<Fn>(s.base + 0x40);
        ChangeRegistry reg(s.at(0), 0x1000);
        const OwnerId a = reg.add_owner("a"), b = reg.add_owner("b");
        const uint8_t patch[2] = {0x90, 0x90};
        CHECK(reg.hook_function(b, in_image(s.at(0x40)), reinterpret_cast<void*>(&detour_b), slot(g_next_b)) ==
              ChangeStatus::Ok);
        CHECK(reg.hook_function(a, in_image(s.at(0x40)), reinterpret_cast<void*>(&detour_a), slot(g_next_a)) ==
              ChangeStatus::Ok);
        CHECK(reg.patch_bytes(a, in_image(s.at(0x100)), patch, 2) == ChangeStatus::Ok);
        CHECK(reg.begin_group(a) == ChangeStatus::Ok);
        CHECK(reg.patch_bytes(a, in_image(s.at(0x200)), patch, 2) == ChangeStatus::Ok);
        CHECK(traced(f, 1) == 104 && g_trace == "ab");
        CHECK(reg.rollback(a) == 2);  // the enabled ones; the staged one is dropped
        CHECK(!reg.group_open(a) && reg.staged_count(a) == 0 && reg.count(a) == 0);
        CHECK(s.base[0x100] == 0xCC && s.base[0x200] == 0xCC);
        CHECK(traced(f, 1) == 4 && g_trace == "b");
        CHECK(reg.describe(b).size() == 1 && reg.describe(b)[0] == "function hook rva 0x40");
        CHECK(reg.rollback(b) == 1 && traced(f, 1) == 2);
    });

    run("changes: *_at outside the image, raw pointers into it refused", [] {
        Scratch image(0x1000);
        Scratch elsewhere(0x1000);  // e.g. a Direct3D 9 vtable target
        image.put(0x40, kAddOne);
        elsewhere.put(0x40, kAddOne);
        const auto g = reinterpret_cast<Fn>(elsewhere.base + 0x40);
        ChangeRegistry reg(image.at(0), 0x1000);
        const OwnerId a = reg.add_owner("a"), b = reg.add_owner("b");
        CHECK(reg.hook_function(a, outside(image.at(0x40)), reinterpret_cast<void*>(&detour_a), slot(g_next_a)) ==
              ChangeStatus::NotDeclared);
        CHECK(reg.hook_function(a, outside(image.at(0) - 2), reinterpret_cast<void*>(&detour_a), slot(g_next_a)) ==
              ChangeStatus::NotDeclared);  // runs into the image
        const uint8_t two[2] = {0x90, 0x90};
        CHECK(reg.patch_bytes(a, outside(image.at(0x100)), two, 2) == ChangeStatus::NotDeclared);
        CHECK(reg.hook_function(a, in_image(elsewhere.at(0x40)), reinterpret_cast<void*>(&detour_a), slot(g_next_a)) ==
              ChangeStatus::InvalidArgument);  // a symbol must lie in the image
        CHECK(reg.hook_function(a, outside(elsewhere.at(0x40), {0x8D, 0x42}), reinterpret_cast<void*>(&detour_a),
                                slot(g_next_a)) == ChangeStatus::BytesMismatch);
        ChangeId hb = 0;
        CHECK(reg.hook_function(b, outside(elsewhere.at(0x40)), reinterpret_cast<void*>(&detour_b), slot(g_next_b),
                                &hb) == ChangeStatus::Ok);
        // The expected prologue is compared with the original bytes, so a chain can grow.
        CHECK(reg.hook_function(a, outside(elsewhere.at(0x40), {0x8D, 0x41, 0x01}), reinterpret_cast<void*>(&detour_a),
                                slot(g_next_a)) == ChangeStatus::Ok);
        CHECK(traced(g, 1) == 104 && g_trace == "ab");
        CHECK(reg.patch_bytes(b, outside(elsewhere.at(0x42)), two, 2) == ChangeStatus::Conflict);
        CHECK(reg.remove(b, hb) == ChangeStatus::Ok);
        CHECK(traced(g, 1) == 102 && g_trace == "a");
        CHECK(reg.rollback(a) == 1 && traced(g, 1) == 2);
        CHECK(std::memcmp(elsewhere.base + 0x40, kAddOne.data(), kAddOne.size()) == 0);
    });

    run("changes: import replacement chain", [] {
        HMODULE self = GetModuleHandleW(nullptr);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(self) + dos->e_lfanew);
        ChangeRegistry reg(reinterpret_cast<uintptr_t>(self), nt->OptionalHeader.SizeOfImage);
        const OwnerId a = reg.add_owner("a"), b = reg.add_owner("b");
        const DWORD real = GetTickCount();
        ChangeId ha = 0, hb = 0;
        CHECK(reg.replace_import(b, self, "kernel32.dll", "GetTickCount", reinterpret_cast<void*>(&tick_b),
                                 reinterpret_cast<void**>(&g_tick_next_b), &hb) == ChangeStatus::Ok);
        CHECK(GetTickCount() == 7u);
        CHECK(g_tick_next_b != nullptr && g_tick_next_b() - real < 60000u);  // the original import
        CHECK(reg.replace_import(a, self, "KERNEL32.dll", "GetTickCount", reinterpret_cast<void*>(&tick_a),
                                 reinterpret_cast<void**>(&g_tick_next_a), &ha) == ChangeStatus::Ok);
        g_trace.clear();
        CHECK(GetTickCount() == 70u && g_trace == "ab");
        void* next = nullptr;
        CHECK(reg.replace_import(a, self, "kernel32.dll", "NoSuchFunction", reinterpret_cast<void*>(&tick_a), &next) ==
              ChangeStatus::NotFound);
        CHECK(reg.remove(b, hb) == ChangeStatus::Ok);  // the inner one: a's next leads to the original
        CHECK(GetTickCount() == 0u);                   // tick_a: the original is not 7
        CHECK(reg.remove(a, ha) == ChangeStatus::Ok);
        CHECK(GetTickCount() - real < 60000u);
    });

    run("changes: owners, names and the end of the registry", [] {
        Scratch s(0x1000);
        s.put(0x40, kAddOne);
        const auto f = reinterpret_cast<Fn>(s.base + 0x40);
        {
            ChangeRegistry reg(s.at(0), 0x1000);
            CHECK(reg.add_owner("first") == 0 && reg.owner("second") == 1 && reg.owner("first") == 0);
            CHECK(reg.owner_name(1) == "second" && reg.owner_name(9) == "?");
            CHECK(reg.hook_function(1, in_image(s.at(0x40)), reinterpret_cast<void*>(&detour_a), slot(g_next_a)) ==
                  ChangeStatus::Ok);
            CHECK(traced(f, 1) == 102);
            CHECK(reg.alloc_near(32) != nullptr && reg.alloc_near(0) == nullptr);
        }
        // Destroying the registry (tests only) removes its changes.
        CHECK(traced(f, 1) == 2 && std::memcmp(s.base + 0x40, kAddOne.data(), kAddOne.size()) == 0);
    });

    run("pointer swap: chain in load order, previous follows the removal of any link", [] {
        Scratch s(0x2000);
        uint64_t* const slot_ptr = reinterpret_cast<uint64_t*>(s.base + 0x1000);
        *slot_ptr = reinterpret_cast<uint64_t>(value(0));
        DWORD old = 0;
        CHECK(VirtualProtect(s.base + 0x1000, 0x1000, PAGE_READONLY, &old));  // a read-only slot, as in .rdata
        const uintptr_t slot = s.at(0x1000);
        const auto now = [&] { return reinterpret_cast<void*>(*slot_ptr); };
        ChangeRegistry reg(s.at(0), 0x2000);
        const OwnerId a = reg.add_owner("a"), b = reg.add_owner("b"), c = reg.add_owner("c");
        ChangeId ha = 0, hb = 0, hc = 0;
        // Requested out of load order: the chain still follows the owners.
        CHECK(reg.swap_pointer(c, in_image(slot), reinterpret_cast<uintptr_t>(value(0)), value(3), &g_prev_c, &hc) ==
              ChangeStatus::Ok);
        CHECK(now() == value(3) && g_prev_c == value(0));
        CHECK(protection(s.base + 0x1000) == PAGE_READONLY);
        CHECK(reg.swap_pointer(a, in_image(slot), 0, value(1), &g_prev_a, &ha) == ChangeStatus::Ok);
        CHECK(reg.swap_pointer(b, in_image(slot), 0, value(2), &g_prev_b, &hb) == ChangeStatus::Ok);
        CHECK(now() == value(1) && g_prev_a == value(2) && g_prev_b == value(3) && g_prev_c == value(0));
        CHECK((reg.chain_at(slot) == std::vector<OwnerId>{a, b, c}));
        CHECK(reg.describe(b) == std::vector<std::string>{"pointer swap rva 0x1000"});
        uint64_t shipped = 0;
        CHECK(reg.read_original(slot, &shipped, sizeof(shipped)) && shipped == reinterpret_cast<uint64_t>(value(0)));

        // The middle link leaves: a's previous now holds c's value; b's keeps
        // its last value (a thread may still pass through b).
        CHECK(reg.remove(b, hb) == ChangeStatus::Ok);
        CHECK(now() == value(1) && g_prev_a == value(3) && g_prev_b == value(3));
        // The outermost link leaves: the slot holds c's value.
        CHECK(reg.remove(a, ha) == ChangeStatus::Ok);
        CHECK(now() == value(3) && g_prev_c == value(0));
        // The innermost link leaves under a new outer one.
        CHECK(reg.swap_pointer(a, in_image(slot), 0, value(1), &g_prev_a, &ha) == ChangeStatus::Ok);
        CHECK(g_prev_a == value(3));
        CHECK(reg.remove(c, hc) == ChangeStatus::Ok);
        CHECK(now() == value(1) && g_prev_a == value(0));
        CHECK(reg.remove(a, ha) == ChangeStatus::Ok);
        CHECK(now() == value(0) && protection(s.base + 0x1000) == PAGE_READONLY);
        CHECK(reg.chain_at(slot).empty());

        // `previous` is optional; a staged swap leaves the slot alone, but its
        // previous already holds the rest of the chain.
        CHECK(reg.swap_pointer(c, in_image(slot), 0, value(3), nullptr, &hc) == ChangeStatus::Ok);
        CHECK(reg.begin_group(a) == ChangeStatus::Ok);
        CHECK(reg.swap_pointer(a, in_image(slot), 0, value(1), &g_prev_a) == ChangeStatus::Ok);
        CHECK(now() == value(3) && g_prev_a == value(3));
        CHECK(reg.commit_group(a) == ChangeStatus::Ok);
        CHECK(now() == value(1));
        CHECK(reg.rollback(a) == 1 && reg.rollback(c) == 1 && now() == value(0));

        // The game set another pointer while nobody swapped it: that is the
        // original for the next chain.
        CHECK(VirtualProtect(s.base + 0x1000, 0x1000, PAGE_READWRITE, &old));
        *slot_ptr = reinterpret_cast<uint64_t>(value(2));
        CHECK(VirtualProtect(s.base + 0x1000, 0x1000, PAGE_READONLY, &old));
        CHECK(reg.swap_pointer(a, in_image(slot), reinterpret_cast<uintptr_t>(value(0)), value(1), &g_prev_a) ==
              ChangeStatus::BytesMismatch);
        CHECK(reg.swap_pointer(a, in_image(slot), reinterpret_cast<uintptr_t>(value(2)), value(1), &g_prev_a, &ha) ==
              ChangeStatus::Ok);
        CHECK(now() == value(1) && g_prev_a == value(2));
        CHECK(reg.remove(a, ha) == ChangeStatus::Ok && now() == value(2));
    });

    run("pointer swap: refusals, conflicts, executable and outside memory", [] {
        Scratch s(0x2000);
        Scratch elsewhere(0x1000);  // e.g. a Direct3D 9 vtable
        void* const shipped_value = value(0);
        std::memcpy(s.base + 0x200, &shipped_value, sizeof(shipped_value));
        ChangeRegistry reg(s.at(0), 0x2000);
        std::vector<std::string> refusals;
        reg.set_refusal_sink([&](OwnerId, const std::string& m) { refusals.push_back(m); });
        const OwnerId a = reg.add_owner("a"), b = reg.add_owner("b");
        alignas(8) uint8_t cells[16] = {};
        CHECK(reg.swap_pointer(a, in_image(s.at(0x204)), 0, value(1), &g_prev_a) == ChangeStatus::UnsafeSite);
        CHECK(refusals.back() == "pointer swap rva 0x204: not 8-byte aligned, so not atomic");
        CHECK(reg.swap_pointer(a, in_image(s.at(0x200)), 0, value(1), reinterpret_cast<void**>(cells + 4)) ==
              ChangeStatus::InvalidArgument);  // an unaligned previous
        CHECK(reg.swap_pointer(a, in_image(s.at(0x1FFC)), 0, value(1), &g_prev_a) == ChangeStatus::InvalidArgument);
        CHECK(reg.swap_pointer(a, outside(s.at(0x200)), 0, value(1), &g_prev_a) == ChangeStatus::NotDeclared);
        CHECK(reg.swap_pointer(9, in_image(s.at(0x200)), 0, value(1), &g_prev_a) == ChangeStatus::InvalidArgument);
        CHECK(reg.swap_pointer(a, in_image(s.at(0x200)), reinterpret_cast<uintptr_t>(value(3)), value(1), &g_prev_a) ==
              ChangeStatus::BytesMismatch);
        CHECK(refusals.back().find("pointer swap rva 0x200: holds at 0x") == 0);

        // Executable read-only memory stays executable.
        DWORD old = 0;
        CHECK(VirtualProtect(s.base, 0x1000, PAGE_EXECUTE_READ, &old));
        ChangeId ha = 0;
        CHECK(reg.swap_pointer(a, in_image(s.at(0x200)), reinterpret_cast<uintptr_t>(value(0)), value(1), &g_prev_a,
                               &ha) == ChangeStatus::Ok);
        CHECK(*reinterpret_cast<void**>(s.base + 0x200) == value(1) && protection(s.base) == PAGE_EXECUTE_READ);
        CHECK(VirtualProtect(s.base, 0x1000, PAGE_EXECUTE_READWRITE, &old));
        // The slot is owned against other kinds and other addresses.
        const uint8_t two[2] = {0x90, 0x90};
        CHECK(reg.patch_bytes(b, in_image(s.at(0x206)), two, 2) == ChangeStatus::Conflict);
        CHECK(refusals.back() == "byte patch rva 0x206 (2 B) overlaps the pointer swap rva 0x200 (8 B) of 'a'; refused");
        CHECK(reg.patch_bytes(b, in_image(s.at(0x1FE)), two, 2) == ChangeStatus::Ok);
        CHECK(reg.swap_pointer(a, in_image(s.at(0x1F8)), 0, value(1), &g_prev_b) == ChangeStatus::Conflict);
        CHECK(reg.remove(a, ha) == ChangeStatus::Ok && *reinterpret_cast<void**>(s.base + 0x200) == value(0));
        CHECK(reg.rollback(b) == 1);

        // A function pointer outside the image: callers pass on through previous.
        const uintptr_t entry = elsewhere.at(0x10);
        const Fn shipped = &shipped_fn;
        std::memcpy(elsewhere.base + 0x10, &shipped, sizeof(shipped));
        const auto call = [&](int x) {
            g_trace.clear();
            return (*reinterpret_cast<Fn*>(entry))(x);
        };
        ChangeId hb = 0;
        CHECK(reg.swap_pointer(b, outside(entry), reinterpret_cast<uintptr_t>(shipped),
                               reinterpret_cast<void*>(&swapped_b), &g_prev_b, &hb) == ChangeStatus::Ok);
        CHECK(reg.swap_pointer(a, outside(entry), 0, reinterpret_cast<void*>(&swapped_a), &g_prev_a, &ha) ==
              ChangeStatus::Ok);
        CHECK(call(1) == 104 && g_trace == "ab");
        CHECK(reg.remove(b, hb) == ChangeStatus::Ok);
        CHECK(call(1) == 102 && g_trace == "a");
        CHECK(reg.rollback(a) == 1 && call(1) == 2 && g_trace.empty());
    });

    run("mid hooks: registers written in the callback are loaded on the way out", [] {
        Scratch s(0x1000);
        s.put(0x40, kSteps);
        s.put(0x80, kAddXmm1);
        const auto f = reinterpret_cast<Fn>(s.base + 0x40);
        const auto g = reinterpret_cast<FnD>(s.base + 0x80);
        ChangeRegistry reg(s.at(0), 0x1000);
        const OwnerId a = reg.add_owner("a");
        CHECK(traced(f, 0) == 15);
        // At the function start: the argument is rcx; rsp points at the return address.
        ChangeId start = 0;
        CHECK(reg.mid_hook(a, in_image(s.at(0x40), {0x8D, 0x41, 0x01}), &set_rcx, reinterpret_cast<void*>(100),
                           &start) == ChangeStatus::Ok);
        CHECK(traced(f, 7) == 115 && g_trace == "m" && g_seen_rcx == 7);
        CHECK(g_seen_rsp % 16 == 8 && g_seen_return != 0);
        CHECK(reg.describe(a) == std::vector<std::string>{"mid hook rva 0x40"});
        // In the middle: the instructions after the site still run.
        Probe plus{'p', 0, 0, 1000};
        CHECK(reg.mid_hook(a, in_image(s.at(0x43)), &probe, &plus) == ChangeStatus::Conflict);  // inside 0x40's jump
        CHECK(reg.remove(a, start) == ChangeStatus::Ok);
        CHECK(reg.mid_hook(a, in_image(s.at(0x43), {0x83, 0xC0, 0x02}), &probe, &plus) == ChangeStatus::Ok);
        CHECK(traced(f, 0) == 1015 && g_trace == "p");
        // XMM registers: g(x) = x + xmm1.
        CHECK(reg.mid_hook(a, in_image(s.at(0x80), kAddXmm1), &set_xmm1, nullptr) == ChangeStatus::Ok);
        CHECK(g(2.0) == 42.0);
        CHECK(reg.mid_hook(a, in_image(s.at(0x80)), nullptr, nullptr) == ChangeStatus::InvalidArgument);
        CHECK(reg.rollback(a) == 2);
        CHECK(std::memcmp(s.base + 0x40, kSteps.data(), kSteps.size()) == 0 && traced(f, 0) == 15);
        CHECK(std::memcmp(s.base + 0x80, kAddXmm1.data(), kAddXmm1.size()) == 0);
    });

    run("mid hooks: chain order, any link removed, a rip change ends the chain once logged", [] {
        Scratch s(0x1000);
        s.put(0x40, kSteps);
        const auto f = reinterpret_cast<Fn>(s.base + 0x40);
        ChangeRegistry reg(s.at(0), 0x1000);
        std::vector<std::pair<OwnerId, std::string>> notices;
        reg.set_notice_sink([&](OwnerId o, const std::string& m) { notices.push_back({o, m}); });
        const OwnerId a = reg.add_owner("a"), b = reg.add_owner("b"), c = reg.add_owner("c");
        Probe pa{'a'}, pb{'b'}, pc{'c'};
        ChangeId ha = 0, hb = 0, hc = 0;
        // Requested out of load order; one context for all of them.
        CHECK(reg.mid_hook(c, in_image(s.at(0x43)), &probe, &pc, &hc) == ChangeStatus::Ok);
        CHECK(reg.mid_hook(a, in_image(s.at(0x43)), &probe, &pa, &ha) == ChangeStatus::Ok);
        CHECK(reg.mid_hook(b, in_image(s.at(0x43)), &probe, &pb, &hb) == ChangeStatus::Ok);
        CHECK(traced(f, 0) == 15 && g_trace == "abc");
        CHECK((reg.chain_at(s.at(0x43)) == std::vector<OwnerId>{a, b, c}));
        CHECK(reg.remove(b, hb) == ChangeStatus::Ok);  // the middle one
        CHECK(traced(f, 0) == 15 && g_trace == "ac");
        CHECK(reg.remove(a, ha) == ChangeStatus::Ok);  // the outermost one
        CHECK(traced(f, 0) == 15 && g_trace == "c");
        CHECK(reg.remove(a, ha) == ChangeStatus::NotFound);

        // a writes rip: past `add eax, 2` and `add eax, 4`, with rax = 50.
        Probe jump{'a', s.at(0x49), 50};
        CHECK(reg.mid_hook(a, in_image(s.at(0x43)), &probe, &jump, &ha) == ChangeStatus::Ok);
        CHECK(reg.mid_hook(b, in_image(s.at(0x43)), &probe, &pb, &hb) == ChangeStatus::Ok);
        CHECK(traced(f, 0) == 58 && g_trace == "a");  // b and c skipped
        CHECK(traced(f, 0) == 58 && g_trace == "a");
        CHECK(notices.size() == 1 && notices[0].first == a &&
              notices[0].second ==
                  "mid hook rva 0x43 changed rip to rva 0x49: the rest of the chain at this address is skipped "
                  "(reported once)");
        CHECK(reg.remove(a, ha) == ChangeStatus::Ok);
        CHECK(traced(f, 0) == 15 && g_trace == "bc");

        // A group: staged mid hooks run only after the commit, all together.
        CHECK(reg.begin_group(a) == ChangeStatus::Ok);
        CHECK(reg.mid_hook(a, in_image(s.at(0x43)), &probe, &pa) == ChangeStatus::Ok);
        CHECK(reg.mid_hook(a, in_image(s.at(0x46)), &probe, &pa) == ChangeStatus::Conflict);  // inside the jump
        CHECK(reg.mid_hook(a, in_image(s.at(0x49)), &probe, &pa) == ChangeStatus::Ok);
        CHECK(traced(f, 0) == 15 && g_trace == "bc");
        CHECK(reg.commit_group(a) == ChangeStatus::Ok);
        CHECK(traced(f, 0) == 15 && g_trace == "abca");
        CHECK(reg.rollback(a) == 2 && reg.rollback(b) == 1 && reg.rollback(c) == 1);
        CHECK(std::memcmp(s.base + 0x40, kSteps.data(), kSteps.size()) == 0 && traced(f, 0) == 15);
        CHECK(reg.chain_at(s.at(0x43)).empty() && notices.size() == 1);
    });

    run("mid hooks run before the function-hook chain at the same address", [] {
        Scratch s(0x1000);
        s.put(0x40, kAddOne);
        const auto f = reinterpret_cast<Fn>(s.base + 0x40);
        ChangeRegistry reg(s.at(0), 0x1000);
        std::vector<std::string> notices;
        reg.set_notice_sink([&](OwnerId, const std::string& m) { notices.push_back(m); });
        const OwnerId a = reg.add_owner("a"), b = reg.add_owner("b");
        ChangeId ha = 0, hb = 0;
        // b is later in the load order, yet its mid hook runs before a's function hook.
        CHECK(reg.hook_function(a, in_image(s.at(0x40)), reinterpret_cast<void*>(&detour_a), slot(g_next_a), &ha) ==
              ChangeStatus::Ok);
        CHECK(reg.mid_hook(b, in_image(s.at(0x40)), &set_rcx, reinterpret_cast<void*>(10), &hb) == ChangeStatus::Ok);
        CHECK(traced(f, 1) == 111 && g_trace == "ma" && g_seen_rcx == 1);  // (10 + 1) + 100
        CHECK((reg.chain_at(s.at(0x40)) == std::vector<OwnerId>{b, a}));
        CHECK(traced(g_next_a, 1) == 2 && g_trace.empty());  // a's next: the original, past the mid hook
        // A rip change skips the function hooks too.
        Probe skip{'s', s.at(0x4C), 7};
        ChangeId hs = 0;
        CHECK(reg.mid_hook(b, in_image(s.at(0x40)), &probe, &skip, &hs) == ChangeStatus::Ok);
        CHECK(traced(f, 1) == 7 && g_trace == "ms" && notices.size() == 1);
        CHECK(reg.remove(b, hs) == ChangeStatus::Ok);
        // Either part leaves on its own.
        CHECK(reg.remove(b, hb) == ChangeStatus::Ok);
        CHECK(traced(f, 1) == 102 && g_trace == "a");
        CHECK(reg.mid_hook(b, in_image(s.at(0x40)), &set_rcx, reinterpret_cast<void*>(10), &hb) == ChangeStatus::Ok);
        CHECK(reg.remove(a, ha) == ChangeStatus::Ok);
        CHECK(traced(f, 1) == 11 && g_trace == "m");
        // A block replacement and function hooks exclude each other at one address.
        CHECK(reg.hook_function(a, in_image(s.at(0x40)), reinterpret_cast<void*>(&detour_a), slot(g_next_a), &ha) ==
              ChangeStatus::Ok);
        Probe block{'B'};
        CHECK(reg.replace_block(b, in_image(s.at(0x40)), 4, 0, &probe, &block) == ChangeStatus::UnsafeSite);
        CHECK(reg.replace_block(b, in_image(s.at(0x40)), 6, 0, &probe, &block) == ChangeStatus::Conflict);
        CHECK(reg.remove(b, hb) == ChangeStatus::Ok);
        CHECK(reg.rollback(a) == 1 && traced(f, 1) == 2);
        CHECK(std::memcmp(s.base + 0x40, kAddOne.data(), kAddOne.size()) == 0);
    });

    run("block replacement: the block never runs, the game resumes at its end or at resume", [] {
        Scratch s(0x1000);
        s.put(0x40, kSteps);
        const auto f = reinterpret_cast<Fn>(s.base + 0x40);
        ChangeRegistry reg(s.at(0), 0x1000);
        std::vector<std::string> refusals;
        reg.set_refusal_sink([&](OwnerId, const std::string& m) { refusals.push_back(m); });
        const OwnerId a = reg.add_owner("a"), b = reg.add_owner("b"), c = reg.add_owner("c");
        const std::vector<uint8_t> block = {0x83, 0xC0, 0x02, 0x83, 0xC0, 0x04};  // add eax, 2; add eax, 4
        Probe pb{'B', 0, 0, 1000};
        ChangeId hb = 0;
        CHECK(reg.replace_block(b, in_image(s.at(0x43), block), 6, 0, &probe, &pb, &hb) == ChangeStatus::Ok);
        CHECK(traced(f, 0) == 1009 && g_trace == "B");  // 1 + 1000 + 8
        CHECK(reg.describe(b) == std::vector<std::string>{"block replacement rva 0x43 (6 B)"});
        CHECK(reg.remove(b, hb) == ChangeStatus::Ok);
        CHECK(traced(f, 0) == 15 && g_trace.empty());
        // resume: here the final `ret`, past `add eax, 8`.
        CHECK(reg.replace_block(b, in_image(s.at(0x43), block), 6, s.at(0x4C), &probe, &pb, &hb) == ChangeStatus::Ok);
        CHECK(traced(f, 0) == 1001 && g_trace == "B");

        // Mid hooks at the block's address run first, whatever the load order.
        Probe pc{'c'};
        ChangeId hc = 0;
        CHECK(reg.mid_hook(c, in_image(s.at(0x43)), &probe, &pc, &hc) == ChangeStatus::Ok);
        CHECK(traced(f, 0) == 1001 && g_trace == "cB");
        CHECK((reg.chain_at(s.at(0x43)) == std::vector<OwnerId>{c, b}));
        Probe jump{'j', s.at(0x49), 50};
        ChangeId hj = 0;
        CHECK(reg.mid_hook(a, in_image(s.at(0x43)), &probe, &jump, &hj) == ChangeStatus::Ok);
        CHECK(traced(f, 0) == 58 && g_trace == "j");  // the rest of the chain, the block included, skipped
        CHECK(reg.remove(a, hj) == ChangeStatus::Ok);

        // The owner holds the whole block.
        const uint8_t nop = 0x90;
        CHECK(reg.patch_bytes(a, in_image(s.at(0x47)), &nop, 1) == ChangeStatus::Conflict);
        CHECK(refusals.back() ==
              "byte patch rva 0x47 (1 B) overlaps the mid hook and block replacement rva 0x43 (6 B) of 'b', 'c'; "
              "refused");
        CHECK(reg.mid_hook(a, in_image(s.at(0x46)), &probe, &pc) == ChangeStatus::Conflict);
        CHECK(reg.hook_function(a, in_image(s.at(0x43)), reinterpret_cast<void*>(&detour_a), slot(g_next_a)) ==
              ChangeStatus::Conflict);
        CHECK(refusals.back() == "function hook rva 0x43: rva 0x43 already has a block replacement of 'b'; refused");
        CHECK(reg.replace_block(a, in_image(s.at(0x43)), 6, 0, &probe, &pb) == ChangeStatus::Conflict);
        CHECK(reg.replace_block(b, in_image(s.at(0x43)), 6, 0, &probe, &pb) == ChangeStatus::Conflict);  // also its owner
        CHECK(reg.replace_block(a, in_image(s.at(0x40)), 6, 0, &probe, &pb) == ChangeStatus::Conflict);
        // Blocks that cannot be replaced safely.
        CHECK(reg.replace_block(a, in_image(s.at(0x49)), 4, 0, &probe, &pb) == ChangeStatus::UnsafeSite);
        CHECK(reg.replace_block(a, in_image(s.at(0x40)), 5, 0, &probe, &pb) == ChangeStatus::UnsafeSite);
        CHECK(refusals.back() == "block replacement rva 0x40 (5 B): does not end on an instruction boundary");
        CHECK(reg.replace_block(a, in_image(s.at(0x40), {0x8D, 0x42}), 3, 0, &probe, &pb) == ChangeStatus::UnsafeSite);
        CHECK(reg.remove(b, hb) == ChangeStatus::Ok);
        CHECK(reg.replace_block(a, in_image(s.at(0x43), {0x83, 0xC1}), 6, 0, &probe, &pb) ==
              ChangeStatus::BytesMismatch);
        CHECK(reg.replace_block(a, in_image(s.at(0x43)), 6, s.at(0x45), &probe, &pb) == ChangeStatus::InvalidArgument);
        CHECK(refusals.back() == "block replacement rva 0x43 (6 B): resumes inside the block");
        CHECK(reg.replace_block(a, in_image(s.at(0x43)), 6, 0, nullptr, &pb) == ChangeStatus::InvalidArgument);

        // Without the block the original code runs again; the mid hook stays.
        CHECK(traced(f, 0) == 15 && g_trace == "c");
        CHECK(reg.replace_block(a, in_image(s.at(0x43), block), 6, 0, &probe, &pb) == ChangeStatus::Ok);  // free again
        CHECK(traced(f, 0) == 1009 && g_trace == "cB");
        CHECK(reg.rollback(a) == 1 && reg.rollback(c) == 1);
        CHECK(std::memcmp(s.base + 0x40, kSteps.data(), kSteps.size()) == 0 && traced(f, 0) == 15);
    });

    run("mid hooks at a redirected call run first and keep the call's return address", [] {
        Scratch s(0x1000);
        // int caller(int x) { return callee(x); }: sub rsp,28h; call 0x200; add rsp,28h; ret
        s.put(0x10, {0x48, 0x83, 0xEC, 0x28, 0xE8, 0, 0, 0, 0, 0x48, 0x83, 0xC4, 0x28, 0xC3});
        const int32_t rel = 0x200 - (0x14 + 5);
        std::memcpy(s.base + 0x15, &rel, 4);
        s.put(0x200, kAddOne);
        const auto caller = reinterpret_cast<Fn>(s.base + 0x10);
        const std::vector<uint8_t> shipped(s.base + 0x10, s.base + 0x1E);
        ChangeRegistry reg(s.at(0), 0x1000);
        const OwnerId a = reg.add_owner("a"), b = reg.add_owner("b");
        ChangeId ha = 0, hb = 0;
        // The mid hook first, then the redirect joins it.
        CHECK(reg.mid_hook(a, in_image(s.at(0x14)), &set_rcx, reinterpret_cast<void*>(5), &ha) == ChangeStatus::Ok);
        CHECK(traced(caller, 1) == 6 && g_trace == "m");
        CHECK(reg.redirect_call(b, in_image(s.at(0x14)), s.at(0x200), reinterpret_cast<void*>(&detour_r),
                                slot(g_next_r), &hb) == ChangeStatus::Ok);
        g_return = nullptr;
        CHECK(traced(caller, 1) == 12 && g_trace == "mr" && g_return == s.base + 0x19);
        CHECK((reg.chain_at(s.at(0x14)) == std::vector<OwnerId>{a, b}));
        // Function hooks and block replacements at the call stay a conflict.
        CHECK(reg.hook_function(a, in_image(s.at(0x14)), reinterpret_cast<void*>(&detour_a), slot(g_next_a)) ==
              ChangeStatus::Conflict);
        Probe pb{'B'};
        CHECK(reg.replace_block(a, in_image(s.at(0x14)), 5, 0, &probe, &pb) == ChangeStatus::Conflict);
        // The mid hook leaves: the redirect writes its own call again.
        CHECK(reg.remove(a, ha) == ChangeStatus::Ok);
        CHECK(s.base[0x14] == 0xE8 && traced(caller, 1) == 4 && g_trace == "r" && g_return == s.base + 0x19);
        // The mid hook comes back over the installed redirect.
        CHECK(reg.mid_hook(a, in_image(s.at(0x14)), &set_rcx, reinterpret_cast<void*>(5), &ha) == ChangeStatus::Ok);
        CHECK(traced(caller, 1) == 12 && g_trace == "mr");
        uint8_t original[5] = {};
        CHECK(reg.read_original(s.at(0x14), original, 5) && std::memcmp(original, shipped.data() + 4, 5) == 0);
        // The redirect leaves under the mid hook, then the mid hook.
        CHECK(reg.remove(b, hb) == ChangeStatus::Ok);
        CHECK(traced(caller, 1) == 6 && g_trace == "m");
        CHECK(reg.remove(a, ha) == ChangeStatus::Ok);
        CHECK(std::memcmp(s.base + 0x10, shipped.data(), shipped.size()) == 0 && traced(caller, 1) == 2);
        // Redirect first, mid hook second, removed in that order.
        CHECK(reg.redirect_call(b, in_image(s.at(0x14)), 0, reinterpret_cast<void*>(&detour_r), slot(g_next_r), &hb) ==
              ChangeStatus::Ok);
        CHECK(reg.mid_hook(a, in_image(s.at(0x14)), &set_rcx, reinterpret_cast<void*>(5), &ha) == ChangeStatus::Ok);
        CHECK(traced(caller, 1) == 12 && g_trace == "mr");
        CHECK(reg.remove(b, hb) == ChangeStatus::Ok);
        CHECK(traced(caller, 1) == 6 && g_trace == "m");
        CHECK(reg.remove(a, ha) == ChangeStatus::Ok);
        CHECK(std::memcmp(s.base + 0x10, shipped.data(), shipped.size()) == 0 && traced(caller, 1) == 2);
    });

    run("mid_hook_at outside the image, raw pointers into it refused", [] {
        Scratch image(0x1000);
        Scratch elsewhere(0x1000);
        image.put(0x40, kSteps);
        elsewhere.put(0x40, kSteps);
        const auto g = reinterpret_cast<Fn>(elsewhere.base + 0x40);
        ChangeRegistry reg(image.at(0), 0x1000);
        const OwnerId a = reg.add_owner("a"), b = reg.add_owner("b");
        Probe pa{'a', 0, 0, 100}, pb{'b'};
        CHECK(reg.mid_hook(a, outside(image.at(0x43)), &probe, &pa) == ChangeStatus::NotDeclared);
        CHECK(reg.mid_hook(a, in_image(elsewhere.at(0x43)), &probe, &pa) == ChangeStatus::InvalidArgument);
        CHECK(reg.mid_hook(a, outside(elsewhere.at(0x43), {0x83, 0xC0, 0x03}), &probe, &pa) ==
              ChangeStatus::BytesMismatch);
        CHECK(reg.mid_hook(b, outside(elsewhere.at(0x43), {0x83, 0xC0, 0x02}), &probe, &pb) == ChangeStatus::Ok);
        CHECK(reg.mid_hook(a, outside(elsewhere.at(0x43)), &probe, &pa) == ChangeStatus::Ok);
        CHECK(traced(g, 0) == 115 && g_trace == "ab");
        CHECK(reg.rollback(a) == 1 && reg.rollback(b) == 1 && traced(g, 0) == 15);
        CHECK(std::memcmp(elsewhere.base + 0x40, kSteps.data(), kSteps.size()) == 0);
    });

    run("suspend: a middle link is skipped and comes back in place; idempotent", [] {
        Scratch s(0x1000);
        s.put(0x40, kAddOne);
        const auto f = reinterpret_cast<Fn>(s.base + 0x40);
        ChangeRegistry reg(s.at(0), 0x1000);
        const OwnerId a = reg.add_owner("a"), b = reg.add_owner("b"), c = reg.add_owner("c");
        for (const auto& [owner, detour, next] : {std::tuple{a, &detour_a, &g_next_a}, std::tuple{b, &detour_b, &g_next_b},
                                                   std::tuple{c, &detour_c, &g_next_c}}) {
            CHECK(reg.hook_function(owner, in_image(s.at(0x40)), reinterpret_cast<void*>(detour), slot(*next)) ==
                  ChangeStatus::Ok);
        }
        CHECK(traced(f, 1) == 124 && g_trace == "abc");
        bool changed = false;
        CHECK(reg.suspend(b, &changed) == ChangeStatus::Ok && changed && reg.suspended(b));
        CHECK(traced(f, 1) == 112 && g_trace == "ac");
        CHECK(traced(g_next_b, 1) == 12 && g_trace == "c");  // a thread still inside b passes on
        CHECK(reg.count(b) == 0 && reg.suspended_count(b) == 1 && reg.describe(b).empty());
        CHECK((reg.chain_at(s.at(0x40)) == std::vector<OwnerId>{a, c}));
        CHECK(reg.suspend(b, &changed) == ChangeStatus::Ok && !changed);
        CHECK(traced(f, 1) == 112 && g_trace == "ac");
        // Every link suspended: the original bytes are back.
        CHECK(reg.suspend(a) == ChangeStatus::Ok && reg.suspend(c) == ChangeStatus::Ok);
        CHECK(std::memcmp(s.base + 0x40, kAddOne.data(), kAddOne.size()) == 0 && traced(f, 1) == 2);
        CHECK(reg.resume(b, &changed) == ChangeStatus::Ok && changed && !reg.suspended(b));
        CHECK(traced(f, 1) == 4 && g_trace == "b");
        CHECK(reg.resume(a) == ChangeStatus::Ok && reg.resume(c) == ChangeStatus::Ok);
        CHECK(traced(f, 1) == 124 && g_trace == "abc");
        CHECK(reg.resume(b, &changed) == ChangeStatus::Ok && !changed);
        CHECK(reg.count(b) == 1 && reg.suspended_count(b) == 0);
        CHECK(reg.suspend(9) == ChangeStatus::InvalidArgument && !reg.suspended(9));
        CHECK(reg.rollback(a) == 1 && reg.rollback(b) == 1 && reg.rollback(c) == 1 && traced(f, 1) == 2);
    });

    run("suspend: a shared byte patch stays while another owner keeps it; the bytes stay reserved", [] {
        Scratch s(0x1000);
        ChangeRegistry reg(s.at(0), 0x1000);
        const OwnerId a = reg.add_owner("a"), b = reg.add_owner("b"), c = reg.add_owner("c");
        const uint8_t patch[4] = {1, 2, 3, 4};
        const uint8_t cc[4] = {0xCC, 0xCC, 0xCC, 0xCC};
        CHECK(reg.patch_bytes(a, in_image(s.at(0x100)), patch, 4) == ChangeStatus::Ok);
        CHECK(reg.patch_bytes(b, in_image(s.at(0x100)), patch, 4) == ChangeStatus::Ok);
        CHECK(reg.suspend(a) == ChangeStatus::Ok);
        CHECK(std::memcmp(s.base + 0x100, patch, 4) == 0);  // b still has it
        CHECK((reg.chain_at(s.at(0x100)) == std::vector<OwnerId>{b}));
        CHECK(reg.suspend(b) == ChangeStatus::Ok);
        CHECK(std::memcmp(s.base + 0x100, cc, 4) == 0);  // nobody acting: reverted
        CHECK(reg.chain_at(s.at(0x100)).empty());
        uint8_t shipped[4] = {};
        CHECK(reg.read_original(s.at(0x100), shipped, 4) && std::memcmp(shipped, cc, 4) == 0);
        // Suspended changes keep their bytes: other bytes there conflict, the same ones share.
        const uint8_t other[4] = {1, 2, 3, 5};
        CHECK(reg.patch_bytes(c, in_image(s.at(0x100)), other, 4) == ChangeStatus::Conflict);
        CHECK(reg.patch_bytes(c, in_image(s.at(0x102)), patch, 2) == ChangeStatus::Conflict);
        ChangeId hc = 0;
        CHECK(reg.patch_bytes(c, in_image(s.at(0x100)), patch, 4, &hc) == ChangeStatus::Ok);
        CHECK(std::memcmp(s.base + 0x100, patch, 4) == 0);
        CHECK(reg.remove(c, hc) == ChangeStatus::Ok && std::memcmp(s.base + 0x100, cc, 4) == 0);
        CHECK(reg.resume(a) == ChangeStatus::Ok && std::memcmp(s.base + 0x100, patch, 4) == 0);
        CHECK(reg.resume(b) == ChangeStatus::Ok && reg.rollback(a) == 1);
        CHECK(std::memcmp(s.base + 0x100, patch, 4) == 0);
        CHECK(reg.rollback(b) == 1 && std::memcmp(s.base + 0x100, cc, 4) == 0);
    });

    run("suspend: a pointer swap in a chain leaves the slot to the others", [] {
        Scratch s(0x2000);
        uint64_t* const slot_ptr = reinterpret_cast<uint64_t*>(s.base + 0x1000);
        *slot_ptr = reinterpret_cast<uint64_t>(value(0));
        DWORD old = 0;
        CHECK(VirtualProtect(s.base + 0x1000, 0x1000, PAGE_READONLY, &old));
        const uintptr_t slot = s.at(0x1000);
        const auto now = [&] { return reinterpret_cast<void*>(*slot_ptr); };
        ChangeRegistry reg(s.at(0), 0x2000);
        const OwnerId a = reg.add_owner("a"), b = reg.add_owner("b"), c = reg.add_owner("c");
        CHECK(reg.swap_pointer(a, in_image(slot), 0, value(1), &g_prev_a) == ChangeStatus::Ok);
        CHECK(reg.swap_pointer(b, in_image(slot), 0, value(2), &g_prev_b) == ChangeStatus::Ok);
        CHECK(reg.swap_pointer(c, in_image(slot), 0, value(3), &g_prev_c) == ChangeStatus::Ok);
        CHECK(reg.suspend(b) == ChangeStatus::Ok);
        CHECK(now() == value(1) && g_prev_a == value(3) && g_prev_b == value(3) && g_prev_c == value(0));
        CHECK(reg.suspend(a) == ChangeStatus::Ok);
        CHECK(now() == value(3) && g_prev_a == value(3));
        CHECK(reg.suspend(c) == ChangeStatus::Ok);
        CHECK(now() == value(0) && g_prev_a == value(0) && g_prev_b == value(0));
        CHECK(protection(s.base + 0x1000) == PAGE_READONLY);
        CHECK(reg.resume(b) == ChangeStatus::Ok);
        CHECK(now() == value(2) && g_prev_b == value(0) && g_prev_a == value(2));
        CHECK(reg.resume(a) == ChangeStatus::Ok && reg.resume(c) == ChangeStatus::Ok);
        CHECK(now() == value(1) && g_prev_a == value(2) && g_prev_b == value(3) && g_prev_c == value(0));
        CHECK(reg.rollback(a) == 1 && reg.rollback(b) == 1 && reg.rollback(c) == 1 && now() == value(0));
    });

    run("suspend: every kind of one owner leaves the game, a suspended block keeps its range", [] {
        Scratch s(0x2000);
        s.put(0x40, kSteps);
        s.put(0x80, kAddOne);
        // int caller(int x) { return callee(x); }: sub rsp,28h; call 0x200; add rsp,28h; ret
        s.put(0x10, {0x48, 0x83, 0xEC, 0x28, 0xE8, 0, 0, 0, 0, 0x48, 0x83, 0xC4, 0x28, 0xC3});
        const int32_t rel = 0x200 - (0x14 + 5);
        std::memcpy(s.base + 0x15, &rel, 4);
        s.put(0x200, kAddOne);
        void* const shipped_value = value(0);
        std::memcpy(s.base + 0x1000, &shipped_value, sizeof(shipped_value));
        const std::vector<uint8_t> shipped(s.base, s.base + 0x2000);
        const auto f = reinterpret_cast<Fn>(s.base + 0x40);
        const auto g = reinterpret_cast<Fn>(s.base + 0x80);
        const auto caller = reinterpret_cast<Fn>(s.base + 0x10);
        ChangeRegistry reg(s.at(0), 0x2000);
        const OwnerId a = reg.add_owner("a"), b = reg.add_owner("b");
        Probe block{'B', 0, 0, 1000}, mid{'m'};
        const uint8_t patch[2] = {0x90, 0x90};
        CHECK(reg.replace_block(b, in_image(s.at(0x43)), 6, 0, &probe, &block) == ChangeStatus::Ok);
        CHECK(reg.mid_hook(b, in_image(s.at(0x43)), &probe, &mid) == ChangeStatus::Ok);
        CHECK(reg.hook_function(b, in_image(s.at(0x80)), reinterpret_cast<void*>(&detour_b), slot(g_next_b)) ==
              ChangeStatus::Ok);
        CHECK(reg.redirect_call(b, in_image(s.at(0x14)), 0, reinterpret_cast<void*>(&detour_c), slot(g_next_c)) ==
              ChangeStatus::Ok);
        CHECK(reg.patch_bytes(b, in_image(s.at(0x300)), patch, 2) == ChangeStatus::Ok);
        CHECK(reg.swap_pointer(b, in_image(s.at(0x1000)), 0, value(1), &g_prev_b) == ChangeStatus::Ok);
        CHECK(traced(f, 0) == 1009 && g_trace == "mB");
        CHECK(traced(g, 1) == 4 && traced(caller, 1) == 12);
        CHECK(reg.count(b) == 6);

        CHECK(reg.suspend(b) == ChangeStatus::Ok);
        CHECK(reg.count(b) == 0 && reg.suspended_count(b) == 6);
        CHECK(std::memcmp(s.base, shipped.data(), shipped.size()) == 0);  // the game as shipped
        CHECK(traced(f, 0) == 15 && g_trace.empty() && traced(g, 1) == 2 && traced(caller, 1) == 2);
        // The suspended block still owns its range, its site too.
        CHECK(reg.patch_bytes(a, in_image(s.at(0x47)), patch, 1) == ChangeStatus::Conflict);
        CHECK(reg.hook_function(a, in_image(s.at(0x43)), reinterpret_cast<void*>(&detour_a), slot(g_next_a)) ==
              ChangeStatus::Conflict);
        CHECK(reg.swap_pointer(a, in_image(s.at(0x1000)), 0, value(2), &g_prev_a) == ChangeStatus::Ok);  // chains
        // Changes requested while suspended are enabled suspended.
        ChangeId later = 0;
        CHECK(reg.patch_bytes(b, in_image(s.at(0x400)), patch, 2, &later) == ChangeStatus::Ok);
        CHECK(s.base[0x400] == 0xCC && reg.suspended_count(b) == 7);
        CHECK(reg.remove(b, later) == ChangeStatus::Ok && reg.suspended_count(b) == 6);

        CHECK(reg.resume(b) == ChangeStatus::Ok && reg.count(b) == 6);
        CHECK(traced(f, 0) == 1009 && g_trace == "mB");
        CHECK(traced(g, 1) == 4 && traced(caller, 1) == 12 && s.base[0x300] == 0x90);
        CHECK(*reinterpret_cast<void**>(s.base + 0x1000) == value(2) && g_prev_a == value(1) && g_prev_b == value(0));
        // Rolling back counts the enabled changes, suspended or not.
        CHECK(reg.suspend(b) == ChangeStatus::Ok && reg.rollback(b) == 6 && reg.rollback(a) == 1);
        CHECK(std::memcmp(s.base, shipped.data(), shipped.size()) == 0);
    });

    run("suspend_plugin and resume_plugin tell only the plugin, once per change of state", [] {
        Scratch s(0x1000);
        ChangeRegistry reg(s.at(0), 0x1000);
        ServiceBus bus;
        const OwnerId p = reg.add_owner("p");
        reg.add_owner("q");
        const uint8_t patch[2] = {0x90, 0x90};
        CHECK(reg.patch_bytes(p, in_image(s.at(0x100)), patch, 2) == ChangeStatus::Ok);
        static std::vector<std::string> events;
        events.clear();
        const AnvilEventCallback record = [](const char* event, const void*, void* who) {
            events.push_back(std::string(static_cast<const char*>(who)) + ":" + event);
        };
        CHECK(bus.subscribe("p", kEventSuspended, record, const_cast<char*>("p")) == ANVIL_OK);
        CHECK(bus.subscribe("p", kEventResumed, record, const_cast<char*>("p")) == ANVIL_OK);
        CHECK(bus.subscribe("q", kEventSuspended, record, const_cast<char*>("q")) == ANVIL_OK);
        CHECK(suspend_plugin(reg, bus, p, "p") == ChangeStatus::Ok);
        CHECK(s.base[0x100] == 0xCC && reg.suspended(p));
        CHECK(suspend_plugin(reg, bus, p, "p") == ChangeStatus::Ok);
        CHECK(events == std::vector<std::string>{"p:anvil.suspended"});
        CHECK(resume_plugin(reg, bus, p, "p") == ChangeStatus::Ok && resume_plugin(reg, bus, p, "p") == ChangeStatus::Ok);
        CHECK(s.base[0x100] == 0x90 && !reg.suspended(p));
        CHECK((events == std::vector<std::string>{"p:anvil.suspended", "p:anvil.resumed"}));
        CHECK(bus.publish_to("q", kEventSuspended, nullptr) == 1 && events.back() == "q:anvil.suspended");
        CHECK(reg.rollback(p) == 1);
    });

    run("profile: calls counted per change, without counting code when off", [] {
        Scratch s(0x1000);
        s.put(0x40, kAddOne);
        s.put(0x80, kSteps);
        // int caller(int x) { return callee(x); }: sub rsp,28h; call 0x200; add rsp,28h; ret
        s.put(0x10, {0x48, 0x83, 0xEC, 0x28, 0xE8, 0, 0, 0, 0, 0x48, 0x83, 0xC4, 0x28, 0xC3});
        const int32_t rel = 0x200 - (0x14 + 5);
        std::memcpy(s.base + 0x15, &rel, 4);
        s.put(0x200, kAddOne);
        const auto f = reinterpret_cast<Fn>(s.base + 0x40);
        const auto g = reinterpret_cast<Fn>(s.base + 0x80);
        const auto caller = reinterpret_cast<Fn>(s.base + 0x10);
        // Where a next stub leads (its target at +8).
        const auto leads_to = [](Fn next) { return *reinterpret_cast<const uintptr_t*>(reinterpret_cast<const uint8_t*>(next) + 8); };
        {
            ChangeRegistry reg(s.at(0), 0x1000);
            const OwnerId a = reg.add_owner("a"), b = reg.add_owner("b");
            CHECK(reg.hook_function(a, in_image(s.at(0x40)), reinterpret_cast<void*>(&detour_a), slot(g_next_a)) ==
                  ChangeStatus::Ok);
            CHECK(reg.hook_function(b, in_image(s.at(0x40)), reinterpret_cast<void*>(&detour_b), slot(g_next_b)) ==
                  ChangeStatus::Ok);
            CHECK(leads_to(g_next_a) == reinterpret_cast<uintptr_t>(&detour_b));  // straight to the detour
            CHECK(traced(f, 1) == 104 && g_trace == "ab");
            std::vector<CallCount> counts{{}};
            CHECK(reg.call_counts(counts) && counts.empty());
        }

        ChangeRegistry reg(s.at(0), 0x1000);
        reg.set_profile(true);
        const OwnerId a = reg.add_owner("a"), b = reg.add_owner("b");
        ChangeTarget hooked = in_image(s.at(0x40));
        hooked.symbol = "add_one";
        ChangeId ha = 0, hm = 0;
        CHECK(reg.hook_function(a, hooked, reinterpret_cast<void*>(&detour_a), slot(g_next_a), &ha) == ChangeStatus::Ok);
        CHECK(reg.hook_function(b, hooked, reinterpret_cast<void*>(&detour_b), slot(g_next_b)) == ChangeStatus::Ok);
        CHECK(leads_to(g_next_a) != reinterpret_cast<uintptr_t>(&detour_b));  // b's counting stub
        Probe pm{'m'}, block{'B', 0, 0, 1000}, jump{'j', s.at(0x8C), 50};  // to the final ret
        CHECK(reg.mid_hook(a, in_image(s.at(0x83)), &probe, &pm, &hm) == ChangeStatus::Ok);
        CHECK(reg.replace_block(b, in_image(s.at(0x83)), 6, 0, &probe, &block) == ChangeStatus::Ok);
        CHECK(reg.redirect_call(b, in_image(s.at(0x14)), 0, reinterpret_cast<void*>(&detour_c), slot(g_next_c)) ==
              ChangeStatus::Ok);
        const uint8_t nop = 0x90;
        CHECK(reg.patch_bytes(a, in_image(s.at(0x300)), &nop, 1) == ChangeStatus::Ok);  // not counted
        for (int i = 0; i < 5; ++i) CHECK(traced(f, 1) == 104 && g_trace == "ab");
        for (int i = 0; i < 3; ++i) CHECK(traced(g, 0) == 1009 && g_trace == "mB");
        CHECK(traced(caller, 1) == 12);
        CHECK(traced(g_next_a, 1) == 4 && g_trace == "b");  // a thread passing on from a counts b only
        // A mid hook that ends the chain: the block is not called, not counted.
        ChangeId hj = 0;
        CHECK(reg.mid_hook(a, in_image(s.at(0x83)), &probe, &jump, &hj) == ChangeStatus::Ok);
        CHECK(traced(g, 0) == 50 && g_trace == "mj");

        std::vector<CallCount> counts;
        CHECK(reg.call_counts(counts));
        std::vector<std::string> lines;
        for (const CallCount& c : counts) lines.push_back(reg.owner_name(c.owner) + " " + describe_call_count(c));
        CHECK((lines == std::vector<std::string>{
                            "a function hook rva 0x40 [add_one]: 5 calls",
                            "b function hook rva 0x40 [add_one]: 6 calls",
                            "a mid hook rva 0x83: 4 calls",
                            "b block replacement rva 0x83 (6 B): 3 calls",
                            "b call redirect rva 0x14: 1 call",
                            "a mid hook rva 0x83: 1 call",
                        }));
        CHECK(counts[0].kind == ChangeKind::FunctionHook && counts[0].symbol == "add_one" && counts[2].symbol.empty());
        // Removed changes keep their count; a removed one never called is left out.
        CHECK(reg.remove(a, ha) == ChangeStatus::Ok && reg.remove(a, hj) == ChangeStatus::Ok);
        ChangeId unused = 0;
        CHECK(reg.hook_function(a, hooked, reinterpret_cast<void*>(&detour_a), slot(g_next_a), &unused) ==
              ChangeStatus::Ok);
        CHECK(reg.remove(a, unused) == ChangeStatus::Ok);
        CHECK(traced(f, 1) == 4 && g_trace == "b");
        CHECK(reg.call_counts(counts, false) && counts.size() == 6 && counts[0].calls == 5 && counts[1].calls == 7);
        CHECK(reg.rollback(a) == 2 && reg.rollback(b) == 3);
        CHECK(traced(f, 1) == 2 && traced(g, 0) == 15 && traced(caller, 1) == 2);
        (void)hm;
    });

    run("profile: anvil.toml profile = true, false by default", [] {
        const auto file = std::filesystem::temp_directory_path() /
                          ("anvil_profile_" + std::to_string(GetCurrentProcessId()) + ".toml");
        const auto settings = [&](const char* text) {
            std::ofstream(file, std::ios::binary) << text;
            return read_anvil_settings(file);
        };
        CHECK(!settings("log_level = \"debug\"\n").profile);
        CHECK(settings("profile = true\n").profile);
        CHECK(!settings("profile = false\n").profile);
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
    });
}
