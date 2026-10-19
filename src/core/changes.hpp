#pragma once

#include <anvil/anvil_plugin.h>

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace anvil {

// The result of a change request, independent of the public header's numbers:
// the host maps it to AnvilResult one to one (host.cpp).
enum class ChangeStatus {
    Ok,
    InvalidArgument,
    NotDeclared,    // a raw pointer into eu4.exe, or an unknown symbol
    WrongKind,      // e.g. a function hook on a data symbol, a redirect on a site that is not `call rel32`
    Conflict,       // overlaps another change at a different address
    UnsafeSite,     // the stolen bytes contain JRCXZ / LOOP* or do not decode; a slot not 8-byte aligned
    BytesMismatch,  // expected bytes differ from the original ones
    OutOfMemory,
    WrongPhase,
    NotFound,
    Internal,
};

const char* change_status_name(ChangeStatus status);

// Kinds in this registry. Function hooks, mid hooks and block replacements at
// one address share one code site and its SafetyHook hook; pointer swaps chain
// like import replacements but on any declared slot.
enum class ChangeKind {
    FunctionHook,
    MidHook,
    BlockReplacement,
    CallRedirect,
    BytePatch,
    PointerSwap,
    ImportReplacement,
};

const char* change_kind_name(ChangeKind kind);

// The plugin that owns a change. Ids are given in load order: a lower id is an
// earlier plugin and sits further out in every chain (design §8.2).
using OwnerId = uint32_t;
// A change handle; 0 is none. Ids are never reused, so a stale handle is NotFound.
using ChangeId = uint64_t;

// Where a change goes and what must be found there.
struct ChangeTarget {
    uintptr_t address = 0;
    // inside_image: a declared symbol in eu4.exe (or a test's scratch image).
    // Otherwise a raw pointer (`*_at`), which must lie outside the image.
    bool inside_image = true;
    // Compared with the original bytes (other changes undone): (actual & mask)
    // == (expected & mask); mask nullptr compares every bit. May be empty.
    const uint8_t* expected = nullptr;
    const uint8_t* mask = nullptr;
    size_t expected_size = 0;
    // The declared symbol's name, for the call counters' report; may be null.
    const char* symbol = nullptr;
};

// The calls one change has seen while counting was on (design §17).
struct CallCount {
    OwnerId owner = 0;
    ChangeKind kind = ChangeKind::FunctionHook;
    std::string label;   // as in describe(), e.g. "function hook rva 0x40"
    std::string symbol;  // empty for a raw address
    uint64_t calls = 0;
};

// One line of the report, e.g. "function hook rva 0x8024D0 [speed_object]: 1234 calls".
std::string describe_call_count(const CallCount& count);

// What an address is to the registry, for crash reports (design §12).
enum class AddressRole {
    None,          // nothing the registry changed or allocated
    ChangedBytes,  // game bytes a change owns: stolen bytes, a block, a call, a patch, a slot
    Stub,          // an Anvil stub: entry, next, context, call or counting stub
    Trampoline,    // SafetyHook's copy of a hooked site's first instructions
    StubMemory,    // Anvil's stub memory that no change uses
};

const char* address_role_name(AddressRole role);

// One change an address belongs to. The strings live as long as the registry.
struct AddressChange {
    ChangeId id = 0;
    OwnerId owner = 0;
    ChangeKind kind = ChangeKind::FunctionHook;
    const char* label = "";   // as in describe()
    const char* symbol = "";  // the declared symbol, "" for a raw address
};

struct AddressAttribution {
    static constexpr size_t kMaxChanges = 8;
    AddressRole role = AddressRole::None;
    uintptr_t site = 0;  // the address the changes act on
    // For a stack walk through code without unwind data:
    uintptr_t original = 0;       // in a trampoline: the game instruction it is a copy of, 0 if none
    bool context_return = false;  // the return address of a context stub's call to its callbacks
    size_t count = 0;             // changes listed (at most kMaxChanges)
    size_t total = 0;             // changes found
    AddressChange changes[kMaxChanges];
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
//  - the core can suspend all changes of an owner and resume them (§8.4);
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
    // `profile = true` (design §17): every function hook, call redirect, mid
    // hook and block replacement requested afterwards counts its calls
    // atomically. Off by default, and then no counting code runs at all. Set
    // it before the first change.
    void set_profile(bool on);

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
    // The 8-byte aligned pointer at target.address holds `value`, stored
    // atomically, also in read-only memory. expected_value != 0 must be the
    // original pointer (a slot's `points_to`). `previous` (optional, aligned)
    // receives the next value in the chain: the following link's value or the
    // original pointer. The registry keeps writing the current one there while
    // the change lives, so it must stay valid until the change is removed, and
    // the plugin reads it each time it passes on.
    ChangeStatus swap_pointer(OwnerId owner, const ChangeTarget& target, uintptr_t expected_value, void* value,
                              void** previous, ChangeId* out = nullptr);
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
    // Returns the number of changes that were enabled (suspended ones too).
    size_t rollback(OwnerId owner);

    // --- suspension (design §8.4, §13): the game runs as if the owner's
    // enabled changes were removed. Its links are skipped (their own next still
    // leads on), its byte patches are reverted unless another owner shares
    // them, its pointer swaps leave the slot to the rest of the chain. Its
    // changes keep their bytes reserved against other changes, and changes it
    // requests while suspended are enabled suspended. Both calls are
    // idempotent: `changed` (optional) is false when the owner already was in
    // that state. Internal when the game's memory could not be written at
    // some site; the changes there keep their previous state, the rest move.
    ChangeStatus suspend(OwnerId owner, bool* changed = nullptr);
    ChangeStatus resume(OwnerId owner, bool* changed = nullptr);
    bool suspended(OwnerId owner) const;

    // --- queries
    // Copies bytes as the game shipped them: bytes changed by any change read in
    // their original form. False outside the image or for unreadable memory.
    bool read_original(uintptr_t address, void* out, size_t size) const;
    // Executable memory within rel32 reach of the image; never freed.
    void* alloc_near(size_t size);

    // Enabled changes of `owner` that act on the game (not suspended), in
    // request order.
    std::vector<std::string> describe(OwnerId owner) const;
    size_t count(OwnerId owner) const;
    size_t staged_count(OwnerId owner) const;
    size_t suspended_count(OwnerId owner) const;
    // Owners of the acting links at `address` in chain order (outermost first).
    std::vector<OwnerId> chain_at(uintptr_t address) const;
    // The counted changes in request order, removed ones only if they saw a
    // call. With wait = false (at process exit, where a killed thread may hold
    // the lock) false when the registry is busy.
    bool call_counts(std::vector<CallCount>& out, bool wait = true) const;
    // The changes an address belongs to (design §12): game bytes a live
    // change owns (its active links, or every link when none is active), or a
    // stub or trampoline (the link it belongs to, or every link of its site).
    // For the crash writer: it neither allocates nor waits, and returns false
    // when the registry is busy (a crashed thread may hold its lock).
    bool attribute(uintptr_t address, AddressAttribution& out) const;
    // The return address inside a context stub, after its call to the
    // callbacks; rbx then holds the saved AnvilCpuContext.
    static size_t context_stub_return();

private:
    struct Site;
    struct Change;
    struct Owner;
    struct Dispatch;
    struct CpuStub;

    static void dispatch(AnvilCpuContext* cpu);
    static void dispatch_counted(AnvilCpuContext* cpu);
    template <bool Count> static void run_callbacks(AnvilCpuContext* cpu);
    static uintptr_t entry_of(const Change& link);

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
                          size_t span = 0, const char* symbol = nullptr);
    ChangeStatus enable(std::vector<ChangeId> ids);
    ChangeStatus move_links(OwnerId owner, bool suspend);
    bool relink(Site& site);
    void publish(Site& site, uintptr_t resume, const std::vector<Change*>& callbacks);
    static void point(const Change& link, uintptr_t target);
    uintptr_t successor(const Site& site, const Change& link) const;
    bool live(const Site& site) const;
    size_t extent(const Site& site) const;
    std::string site_name(const Site& site) const;
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
    bool profile_ = false;
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
