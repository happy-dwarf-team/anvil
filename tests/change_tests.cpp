// Change registry tests (design §8): chains, conflicts, shared byte patches,
// staging, handles, rollback and `*_at`, on code written into scratch memory.
#include "core/changes.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <cstring>
#include <string>
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
        // An owner with changes may not verify dependencies any more.
        CHECK(reg.expect_bytes(a, s.at(0x40), kAddOne.data(), nullptr, 3) == ChangeStatus::WrongPhase);
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

    run("changes: expect_bytes compares the original bytes", [] {
        Scratch s(0x2000);
        const uintptr_t base = s.at(0);
        ChangeRegistry reg(base, 0x2000);
        const OwnerId mine = reg.add_owner("mine"), other = reg.add_owner("other"), third = reg.add_owner("third");
        const std::vector<uint8_t> code = {0x48, 0x89, 0x5C, 0x24, 0x08};
        s.put(0x100, code);
        const uintptr_t addr = base + 0x100;
        std::string detail;
        CHECK(reg.expect_bytes(mine, addr, code.data(), nullptr, code.size()) == ChangeStatus::Ok);
        const uint8_t wrong[] = {0x48, 0x89, 0x5C, 0x24, 0x10};
        CHECK(reg.expect_bytes(mine, addr, wrong, nullptr, sizeof(wrong), &detail) == ChangeStatus::BytesMismatch);
        CHECK(detail == "byte +0x4: expected 0x10, found 0x08");
        const uint8_t wildcard[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00};
        CHECK(reg.expect_bytes(mine, addr, wrong, wildcard, sizeof(wrong)) == ChangeStatus::Ok);
        const uint8_t low_nibble[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x0F};  // 0x08 vs 0x18: low nibble equal
        const uint8_t eighteen[] = {0x48, 0x89, 0x5C, 0x24, 0x18};
        CHECK(reg.expect_bytes(mine, addr, eighteen, low_nibble, sizeof(eighteen)) == ChangeStatus::Ok);

        CHECK(reg.expect_bytes(mine, addr, nullptr, nullptr, 4) == ChangeStatus::InvalidArgument);
        CHECK(reg.expect_bytes(mine, addr, code.data(), nullptr, 0) == ChangeStatus::InvalidArgument);
        CHECK(reg.expect_bytes(mine, addr, code.data(), nullptr, 65537) == ChangeStatus::InvalidArgument);
        CHECK(reg.expect_bytes(mine, base + 0x1FFE, code.data(), nullptr, 4) == ChangeStatus::InvalidArgument);
        CHECK(VirtualFree(s.base + 0x1000, 0x1000, MEM_DECOMMIT));
        CHECK(reg.expect_bytes(mine, base + 0x1000, code.data(), nullptr, 1) == ChangeStatus::InvalidArgument);
        CHECK(reg.expect_bytes(mine, base + 0xFFE, code.data(), nullptr, 4) == ChangeStatus::InvalidArgument);

        // Changes of other owners are compared in their original form.
        const uint8_t nops[] = {0x90, 0x90};
        CHECK(reg.patch_bytes(other, in_image(addr, {0x48, 0x89}), nops, 2) == ChangeStatus::Ok);
        CHECK(s.base[0x100] == 0x90);
        CHECK(reg.expect_bytes(mine, addr, code.data(), nullptr, code.size()) == ChangeStatus::Ok);
        s.put(0x200, kAddOne);
        CHECK(reg.hook_function(other, in_image(base + 0x200, kAddOne), reinterpret_cast<void*>(&detour_a),
                                slot(g_next_a)) == ChangeStatus::Ok);
        CHECK(std::memcmp(s.base + 0x200, kAddOne.data(), kAddOne.size()) != 0);
        CHECK(reg.expect_bytes(mine, base + 0x200, kAddOne.data(), nullptr, kAddOne.size()) == ChangeStatus::Ok);

        // Own changes first: dependencies must be verified before changing anything.
        CHECK(reg.patch_bytes(mine, in_image(base + 0x400), nops, 2) == ChangeStatus::Ok);
        CHECK(reg.expect_bytes(mine, addr, code.data(), nullptr, code.size()) == ChangeStatus::WrongPhase);
        CHECK(reg.expect_bytes(third, addr, code.data(), nullptr, code.size()) == ChangeStatus::Ok);
        CHECK(reg.rollback(mine) == 1);
        CHECK(reg.expect_bytes(mine, addr, code.data(), nullptr, code.size()) == ChangeStatus::Ok);
        CHECK(reg.rollback(other) == 2);
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
}
