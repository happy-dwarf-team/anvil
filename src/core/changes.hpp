#pragma once

#include "cpu_context.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace anvil {

// The result of a change request, independent of the public header's numbers:
// the host maps it to AnvilResult (API 1 now, API 2 with #46).
enum class ChangeStatus {
    Ok,
    InvalidArgument,
    NotDeclared,    // a raw pointer into eu4.exe, or an unknown symbol
    WrongKind,      // e.g. a function hook on a data symbol, a redirect on a site that is not `call rel32`
    Conflict,       // overlaps another change at a different address
    UnsafeSite,     // the stolen bytes contain JRCXZ / LOOP* or do not decode
    BytesMismatch,  // expected bytes differ from the original ones
    OutOfMemory,
    WrongPhase,
    NotFound,
    Internal,
};

const char* change_status_name(ChangeStatus status);

// Kinds in this registry. Function hooks, mid hooks and block replacements at
// one address share one code site and its SafetyHook hook; pointer swaps (#44)
// will chain like import replacements but on any declared slot.
enum class ChangeKind { FunctionHook, MidHook, BlockReplacement, CallRedirect, BytePatch, ImportReplacement };

const char* change_kind_name(ChangeKind kind);

// The plugin that owns a change. Ids are given in load order: a lower id is an
// earlier plugin and sits further out in every chain (design §8.2).
using OwnerId = uint32_t;
// A change handle; 0 is none. Ids are never reused, so a stale handle is NotFound.
using ChangeId = uint64_t;

// Where a change goes and what must be found there.
struct ChangeTarget {
    uintptr_t address = 0;
    // inside_image: a declared symbol (or an API 1 RVA) in eu4.exe.
    // Otherwise a raw pointer (`*_at`), which must lie outside the image.
    bool inside_image = true;
    // Compared with the original bytes (other changes undone): (actual & mask)
    // == (expected & mask); mask nullptr compares every bit. May be empty.
    const uint8_t* expected = nullptr;
    const uint8_t* mask = nullptr;
    size_t expected_size = 0;
};

// All changes of game memory go through one registry (design §8):
//  - one SafetyHook hook per address; Anvil chains the plugins itself, each
//    link with its own `next` (a jump stub Anvil re-points atomically), so
//    links can be removed in any order;
//  - at one address the mid hooks run first (one saved context, callbacks in
//    chain order), then a block replacement or the function-hook chain or the
//    redirected call (§8.2); a mid hook that changes `rip` ends the chain;
//  - a change overlapping another change at a different address is refused
//    (also for the same owner); identical byte patches share ownership;
//  - changes inside a group (begin/commit) are enabled together or not at
//    all; outside a group each request is its own group;
//  - executable memory (stubs, trampolines) is never freed.
// Addresses are absolute; the host converts RVAs and symbols.
class ChangeRegistry {
public:
    ChangeRegistry(uintptr_t image_base, uint32_t image_size);
    // The registry lives for the process. Destroying it (tests) removes every
    // change but frees no executable memory.
    ~ChangeRegistry();
    ChangeRegistry(const ChangeRegistry&) = delete;
    ChangeRegistry& operator=(const ChangeRegistry&) = delete;

    // Owners in load order: the first call gives the outermost owner. `owner`
    // finds a name or adds it at the end.
    OwnerId add_owner(const std::string& name);
    OwnerId owner(const std::string& name);
    std::string owner_name(OwnerId owner) const;

    // Receives why a request was refused, e.g. both owners of a
    // conflict. Called under the registry's lock: it must not call back.
    using RefusalSink = std::function<void(OwnerId owner, const std::string& message)>;
    void set_refusal_sink(RefusalSink sink);
    // Receives what the changes meet while the game runs: the first mid hook
    // per address that changes `rip`. Called on the game's thread, not under
    // the registry's lock; set it before the first change.
    using NoticeSink = std::function<void(OwnerId owner, const std::string& message)>;
    void set_notice_sink(NoticeSink sink);

    // --- requests: `next` receives the link's own next (never null on Ok);
    // `out` (optional) the handle. In an open group the change is staged and
    // `next` already leads to the rest of the chain.
    ChangeStatus hook_function(OwnerId owner, const ChangeTarget& target, void* detour, void** next,
                               ChangeId* out = nullptr);
    // callback(cpu, user) runs when the game reaches target.address, before
    // the code there (or the block replacement / function hooks / redirected
    // call at the same address). Registers written to `cpu` are loaded on the
    // way out; a new `cpu->rip` resumes there and skips the rest of the chain.
    ChangeStatus mid_hook(OwnerId owner, const ChangeTarget& target, AnvilCpuCallback callback, void* user,
                          ChangeId* out = nullptr);
    // The `size` bytes at target.address (whole instructions, >= 5) never run:
    // callback(cpu, user) runs instead and the game resumes at `resume`, or at
    // the block's end when it is 0 (unless the callback writes `cpu->rip`).
    // The owner holds the whole block: other changes inside it conflict, mid
    // hooks at its first byte run before it.
    ChangeStatus replace_block(OwnerId owner, const ChangeTarget& target, size_t size, uintptr_t resume,
                               AnvilCpuCallback callback, void* user, ChangeId* out = nullptr);
    // A `call rel32` at target.address. expected_callee != 0 must be the
    // original callee. `next` is the previous target in the chain.
    ChangeStatus redirect_call(OwnerId owner, const ChangeTarget& target, uintptr_t expected_callee, void* new_target,
                               void** next, ChangeId* out = nullptr);
    ChangeStatus patch_bytes(OwnerId owner, const ChangeTarget& target, const void* bytes, size_t size,
                             ChangeId* out = nullptr);
    // An import of `image` (eu4.exe in the game), module case-insensitive.
    ChangeStatus replace_import(OwnerId owner, HMODULE image, const char* module, const char* function,
                                void* replacement, void** next, ChangeId* out = nullptr);
    ChangeStatus remove(OwnerId owner, ChangeId change);

    // --- staging: the host opens a group around anvil_plugin_load.
    ChangeStatus begin_group(OwnerId owner);
    // Enables every staged change of the group, or none (then they are dropped).
    ChangeStatus commit_group(OwnerId owner);
    void abort_group(OwnerId owner);
    bool group_open(OwnerId owner) const;

    // Removes every change of `owner` (staged or enabled) and closes its group.
    // Returns the number of changes that were enabled.
    size_t rollback(OwnerId owner);

    // --- queries
    // Copies bytes as the game shipped them: bytes changed by any change read in
    // their original form. False outside the image or for unreadable memory.
    bool read_original(uintptr_t address, void* out, size_t size) const;
    // API 1 dependency check against the original bytes. WrongPhase once
    // `owner` has changes (staged or enabled); InvalidArgument outside the image
    // or for unreadable memory. On a mismatch `mismatch` names the first byte.
    ChangeStatus expect_bytes(OwnerId owner, uintptr_t address, const void* expected, const void* mask, size_t size,
                              std::string* mismatch = nullptr) const;
    // Executable memory within rel32 reach of the image; never freed.
    void* alloc_near(size_t size);

    // Enabled changes of `owner`, in request order.
    std::vector<std::string> describe(OwnerId owner) const;
    size_t count(OwnerId owner) const;
    size_t staged_count(OwnerId owner) const;
    // Owners of the enabled links at `address` in chain order (outermost first).
    std::vector<OwnerId> chain_at(uintptr_t address) const;

private:
    struct Site;
    struct Change;
    struct Owner;
    struct Dispatch;
    struct CpuStub;

    static void dispatch(AnvilCpuContext* cpu);

    ChangeStatus refuse(OwnerId owner, ChangeStatus status, const std::string& message);
    ChangeStatus check_owner(OwnerId owner) const;
    ChangeStatus check_target(OwnerId owner, const ChangeTarget& target, size_t size, const char* what);
    ChangeStatus check_overlap(OwnerId owner, const Site* joining, uintptr_t address, size_t size, ChangeKind kind,
                               const std::string& what, const Site* partner = nullptr);
    Site* find_site(uintptr_t address, ChangeKind kind) const;
    Site& add_site(uintptr_t address, ChangeKind kind, size_t footprint);
    ChangeStatus code_site(OwnerId owner, const ChangeTarget& target, ChangeKind kind, size_t span,
                           const std::string& label, Site*& out);
    bool add_call_stub(Site& code, const Site* call);
    ChangeStatus add_link(OwnerId owner, Site& site, ChangeKind kind, uintptr_t detour, std::vector<uint8_t> bytes,
                          std::string label, void** next, ChangeId* out, void* user = nullptr, uintptr_t resume = 0,
                          size_t span = 0);
    ChangeStatus enable(std::vector<ChangeId> ids);
    bool relink(Site& site);
    void publish(Site& site, uintptr_t resume, const std::vector<Change*>& callbacks);
    uintptr_t successor(const Site& site, const Change& link) const;
    bool live(const Site& site) const;
    size_t extent(const Site& site) const;
    std::string site_name(const Site& site) const;
    bool installed_bytes(const Site& site, const std::vector<uint8_t>*& original) const;
    uint8_t* new_stub(uintptr_t target);
    bool in_image(uintptr_t address, size_t size) const;
    bool read_original_locked(uintptr_t address, void* out, size_t size) const;
    void* alloc_near_locked(size_t size);
    std::string where(uintptr_t address) const;

    uintptr_t image_base_;
    uint32_t image_size_;
    mutable std::mutex mutex_;
    std::vector<Owner> owners_;
    std::vector<std::unique_ptr<Site>> sites_;
    std::vector<std::unique_ptr<Change>> changes_;  // index == id - 1; removed ones stay
    RefusalSink sink_;
    std::mutex notice_mutex_;  // not the registry's lock: the sink runs inside game code
    NoticeSink notice_;
    struct Arena {
        uint8_t* base;
        size_t used;
        size_t size;
    };
    std::vector<Arena> arenas_;
};

// Writes to code/data pages with a temporary protection change and flushes the
// instruction cache. Returns false when the protection cannot be changed.
bool write_memory(void* address, const void* data, size_t size);

}  // namespace anvil
