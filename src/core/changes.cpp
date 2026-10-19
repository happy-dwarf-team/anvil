#include "changes.hpp"

#include <Zydis/Zydis.h>
#include <safetyhook.hpp>
#include <safetyhook/os.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace anvil {

const char* change_status_name(ChangeStatus status) {
    switch (status) {
    case ChangeStatus::Ok: return "ok";
    case ChangeStatus::InvalidArgument: return "invalid argument";
    case ChangeStatus::NotDeclared: return "not declared";
    case ChangeStatus::WrongKind: return "wrong kind";
    case ChangeStatus::Conflict: return "conflict with another change";
    case ChangeStatus::UnsafeSite: return "unsafe site";
    case ChangeStatus::BytesMismatch: return "bytes mismatch";
    case ChangeStatus::OutOfMemory: return "out of memory";
    case ChangeStatus::WrongPhase: return "wrong phase";
    case ChangeStatus::NotFound: return "not found";
    case ChangeStatus::Internal: return "internal error";
    }
    return "unknown";
}

const char* change_kind_name(ChangeKind kind) {
    switch (kind) {
    case ChangeKind::FunctionHook: return "function hook";
    case ChangeKind::CallRedirect: return "call redirect";
    case ChangeKind::BytePatch: return "byte patch";
    case ChangeKind::ImportReplacement: return "import replacement";
    }
    return "change";
}

namespace {

// A link's life. Only Active links are part of a chain or keep a byte patch
// written; suspension (#45) adds a state that is skipped the same way.
enum class LinkState { Staged, Active, Removed };

// Jump stub: jmp qword ptr [rip+2]; int3 x2; dq target. The target sits 8-byte
// aligned at +8, so re-pointing a link is one atomic store.
constexpr size_t kStubSize = 16;

void set_stub(uint8_t* stub, uintptr_t target) {
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(stub + 8), static_cast<LONG64>(target));
}

bool readable(uintptr_t address, size_t size) {
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    uintptr_t current = address;
    while (current < address + size) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(current), &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0 || (mbi.Protect & kReadable) == 0) return false;
        current = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

bool executable(uintptr_t address) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) == 0) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Code is written under SafetyHook's page trapping: a thread that reaches the
// bytes meanwhile waits until they are complete. Data is written directly; an
// aligned pointer in one atomic store.
bool write_code_or_data(uintptr_t address, const void* data, size_t size) {
    if (executable(address)) {
        auto* at = reinterpret_cast<uint8_t*>(address);
        bool written = false;
        safetyhook::trap_threads(at, at, size, [&] {
            if (!readable(address, size)) return;
            std::memcpy(at, data, size);
            written = true;
        });
        FlushInstructionCache(GetCurrentProcess(), at, size);
        return written;
    }
    if (size == sizeof(uintptr_t) && address % sizeof(uintptr_t) == 0) {
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_READWRITE, &old)) return false;
        LONG64 value = 0;
        std::memcpy(&value, data, sizeof(value));
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(address), value);
        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<void*>(address), size, old, &ignored);
        return true;
    }
    return write_memory(reinterpret_cast<void*>(address), data, size);
}

std::string hex(uintptr_t value) {
    std::ostringstream s;
    s << "0x" << std::hex << std::uppercase << value;
    return s.str();
}

// The bytes SafetyHook will steal at a function start (whole instructions
// covering 5 bytes), refusing JRCXZ / LOOP*, which it mis-relocates (§8.3).
ChangeStatus stolen_length(const uint8_t* code, size_t available, size_t& length, std::string& detail) {
    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) {
        return ChangeStatus::Internal;
    }
    size_t at = 0;
    while (at < 5) {
        ZydisDecodedInstruction instruction;
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, nullptr, code + at, available - at, &instruction))) {
            detail = "no instruction decodes at +" + hex(at);
            return ChangeStatus::UnsafeSite;
        }
        switch (instruction.mnemonic) {
        case ZYDIS_MNEMONIC_JCXZ:
        case ZYDIS_MNEMONIC_JECXZ:
        case ZYDIS_MNEMONIC_JRCXZ:
        case ZYDIS_MNEMONIC_LOOP:
        case ZYDIS_MNEMONIC_LOOPE:
        case ZYDIS_MNEMONIC_LOOPNE:
            detail = "the stolen bytes hold JRCXZ/LOOP at +" + hex(at);
            return ChangeStatus::UnsafeSite;
        default: break;
        }
        at += instruction.length;
    }
    length = at;
    return ChangeStatus::Ok;
}

ChangeStatus from_safetyhook(const safetyhook::InlineHook::Error& error, std::string& detail) {
    using E = safetyhook::InlineHook::Error;
    switch (error.type) {
    case E::BAD_ALLOCATION: detail = "SafetyHook found no memory for a trampoline"; return ChangeStatus::OutOfMemory;
    case E::FAILED_TO_DECODE_INSTRUCTION:
    case E::SHORT_JUMP_IN_TRAMPOLINE:
    case E::IP_RELATIVE_INSTRUCTION_OUT_OF_RANGE:
    case E::UNSUPPORTED_INSTRUCTION_IN_TRAMPOLINE:
    case E::NOT_ENOUGH_SPACE:
        detail = "SafetyHook cannot relocate the stolen bytes (error " + std::to_string(static_cast<int>(error.type)) + ")";
        return ChangeStatus::UnsafeSite;
    default: detail = "SafetyHook error " + std::to_string(static_cast<int>(error.type)); return ChangeStatus::Internal;
    }
}

}  // namespace

// One change: a link of a chain, or one owner of a (shared) byte patch.
struct ChangeRegistry::Change {
    ChangeId id = 0;
    OwnerId owner = 0;
    Site* site = nullptr;
    LinkState state = LinkState::Staged;
    uintptr_t detour = 0;        // chain kinds: where this link goes
    uint8_t* next = nullptr;     // chain kinds: this link's own next stub
    std::vector<uint8_t> bytes;  // byte patch
    std::string label;
};

// Everything at one address of one kind. The game is changed only while a site
// has Active links; a site without links stays (its stubs and hook are never
// freed) and is reused by the next request of the same kind.
struct ChangeRegistry::Site {
    ChangeKind kind = ChangeKind::BytePatch;
    uintptr_t address = 0;
    size_t footprint = 0;           // bytes this site owns: stolen bytes, call, slot, patch
    std::vector<uint8_t> original;  // those bytes as the game shipped them
    uintptr_t terminal = 0;         // where the innermost next leads: trampoline, callee, import
    uint8_t* entry = nullptr;       // function hook, redirect: the stub the game jumps to first
    std::unique_ptr<safetyhook::InlineHook> hook;  // function hook; #43 hangs mid hooks here too
    bool installed = false;         // the game's bytes are changed
    std::vector<Change*> links;     // not Removed, in chain order (owner, then request)
};

struct ChangeRegistry::Owner {
    std::string name;
    bool group = false;
    std::vector<ChangeId> staged;  // in request order
};

bool write_memory(void* address, const void* data, size_t size) {
    DWORD old = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(address, data, size);
    DWORD ignored = 0;
    VirtualProtect(address, size, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    return true;
}

ChangeRegistry::ChangeRegistry(uintptr_t image_base, uint32_t image_size)
    : image_base_(image_base), image_size_(image_size) {}

ChangeRegistry::~ChangeRegistry() {
    std::lock_guard lock(mutex_);
    for (auto& change : changes_) change->state = LinkState::Removed;
    for (auto& site : sites_) {
        site->links.clear();
        relink(*site);
        (void)site->hook.release();  // the trampoline stays allocated
    }
}

OwnerId ChangeRegistry::add_owner(const std::string& name) {
    std::lock_guard lock(mutex_);
    owners_.push_back({name});
    return static_cast<OwnerId>(owners_.size() - 1);
}

OwnerId ChangeRegistry::owner(const std::string& name) {
    {
        std::lock_guard lock(mutex_);
        for (size_t i = 0; i < owners_.size(); ++i) {
            if (owners_[i].name == name) return static_cast<OwnerId>(i);
        }
    }
    return add_owner(name);
}

std::string ChangeRegistry::owner_name(OwnerId owner) const {
    std::lock_guard lock(mutex_);
    return owner < owners_.size() ? owners_[owner].name : "?";
}

void ChangeRegistry::set_refusal_sink(RefusalSink sink) {
    std::lock_guard lock(mutex_);
    sink_ = std::move(sink);
}

ChangeStatus ChangeRegistry::refuse(OwnerId owner, ChangeStatus status, const std::string& message) {
    if (sink_) sink_(owner, message);
    return status;
}

ChangeStatus ChangeRegistry::check_owner(OwnerId owner) const {
    return owner < owners_.size() ? ChangeStatus::Ok : ChangeStatus::InvalidArgument;
}

bool ChangeRegistry::in_image(uintptr_t address, size_t size) const {
    return address >= image_base_ && size <= image_size_ && address - image_base_ <= image_size_ - size;
}

std::string ChangeRegistry::where(uintptr_t address) const {
    return in_image(address, 0) ? "rva " + hex(address - image_base_) : "at " + hex(address);
}

ChangeStatus ChangeRegistry::check_target(OwnerId owner, const ChangeTarget& target, size_t size, const char* what) {
    const std::string label = std::string(what) + " " + where(target.address);
    const size_t checked = std::max(size, target.expected_size);
    if (target.inside_image) {
        if (!in_image(target.address, checked)) {
            return refuse(owner, ChangeStatus::InvalidArgument, label + ": outside eu4.exe");
        }
    } else {
        const uintptr_t end = target.address + checked;
        if (end < target.address || (target.address < image_base_ + image_size_ && image_base_ < end)) {
            return refuse(owner, ChangeStatus::NotDeclared,
                          label + ": a raw pointer into eu4.exe; declare a symbol instead");
        }
    }
    if (!readable(target.address, checked)) return refuse(owner, ChangeStatus::InvalidArgument, label + ": unreadable");
    if (target.expected_size == 0) return ChangeStatus::Ok;
    if (target.expected == nullptr) return ChangeStatus::InvalidArgument;
    std::vector<uint8_t> actual(target.expected_size);
    if (!read_original_locked(target.address, actual.data(), actual.size())) return ChangeStatus::InvalidArgument;
    for (size_t i = 0; i < actual.size(); ++i) {
        const uint8_t m = target.mask != nullptr ? target.mask[i] : 0xFF;
        if ((actual[i] & m) == (target.expected[i] & m)) continue;
        char text[96];
        std::snprintf(text, sizeof(text), ": byte +0x%zX: expected 0x%02X, found 0x%02X", i, target.expected[i] & m,
                      actual[i]);
        return refuse(owner, ChangeStatus::BytesMismatch, label + text);
    }
    return ChangeStatus::Ok;
}

bool ChangeRegistry::live(const Site& site) const { return !site.links.empty(); }

ChangeStatus ChangeRegistry::check_overlap(OwnerId owner, const Site* joining, uintptr_t address, size_t size,
                                           ChangeKind kind, const std::string& what) {
    for (const auto& s : sites_) {
        if (s.get() == joining || !live(*s)) continue;
        if (!(address < s->address + s->footprint && s->address < address + size)) continue;
        std::string owners;
        for (const Change* c : s->links) {
            const std::string& name = owners_[c->owner].name;
            if (owners.find("'" + name + "'") != std::string::npos) continue;
            owners += (owners.empty() ? "'" : ", '") + name + "'";
        }
        std::string message = what + " overlaps the " + change_kind_name(s->kind) + " " + where(s->address) + " (" +
                              std::to_string(s->footprint) + " B) of " + owners;
        if (s->address == address && s->kind == kind) message += " with different bytes";
        return refuse(owner, ChangeStatus::Conflict, message + "; refused");
    }
    return ChangeStatus::Ok;
}

ChangeRegistry::Site* ChangeRegistry::find_site(uintptr_t address, ChangeKind kind) const {
    for (const auto& s : sites_) {
        if (s->address == address && s->kind == kind) return s.get();
    }
    return nullptr;
}

ChangeRegistry::Site& ChangeRegistry::add_site(uintptr_t address, ChangeKind kind, size_t footprint) {
    auto site = std::make_unique<Site>();
    site->address = address;
    site->kind = kind;
    site->footprint = footprint;
    sites_.push_back(std::move(site));
    return *sites_.back();
}

uint8_t* ChangeRegistry::new_stub(uintptr_t target) {
    auto* stub = static_cast<uint8_t*>(alloc_near_locked(kStubSize));
    if (stub == nullptr) return nullptr;
    const uint8_t jump[8] = {0xFF, 0x25, 0x02, 0x00, 0x00, 0x00, 0xCC, 0xCC};
    std::memcpy(stub, jump, sizeof(jump));
    set_stub(stub, target);
    FlushInstructionCache(GetCurrentProcess(), stub, kStubSize);
    return stub;
}

uintptr_t ChangeRegistry::successor(const Site& site, const Change& link) const {
    bool after = false;
    for (const Change* c : site.links) {
        if (c == &link) {
            after = true;
        } else if (after && c->state == LinkState::Active) {
            return c->detour;
        }
    }
    return site.terminal;
}

// Points every link and the site's entry at the Active links, innermost first,
// so a thread entering the chain at any moment finds a complete one; installs
// or uninstalls the site when its first link comes or its last one goes.
bool ChangeRegistry::relink(Site& site) {
    std::vector<Change*> active;
    for (Change* c : site.links) {
        if (c->state == LinkState::Active) active.push_back(c);
    }
    if (site.kind != ChangeKind::BytePatch) {
        for (Change* c : site.links) {
            if (c->state != LinkState::Active) set_stub(c->next, successor(site, *c));
        }
        for (size_t i = active.size(); i-- > 0;) {
            set_stub(active[i]->next, i + 1 < active.size() ? active[i + 1]->detour : site.terminal);
        }
    }
    const bool want = !active.empty();
    switch (site.kind) {
    case ChangeKind::FunctionHook:
        // Uninstalled, the entry leads past every link: a thread already on
        // its way in skips the removed ones.
        set_stub(site.entry, want ? active.front()->detour : site.terminal);
        if (want != site.installed) {
            if (!(want ? site.hook->enable() : site.hook->disable())) return false;
        }
        break;
    case ChangeKind::CallRedirect:
        set_stub(site.entry, want ? active.front()->detour : site.terminal);
        if (want != site.installed) {
            uint8_t call[5] = {0xE8, 0, 0, 0, 0};
            const int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(site.entry) -
                                                     static_cast<intptr_t>(site.address + 5));
            std::memcpy(call + 1, &rel, sizeof(rel));
            if (!write_code_or_data(site.address, want ? call : site.original.data(), 5)) return false;
        }
        break;
    case ChangeKind::ImportReplacement: {
        const uintptr_t value = want ? active.front()->detour : site.terminal;
        if (want || site.installed) {
            uintptr_t current = 0;
            std::memcpy(&current, reinterpret_cast<const void*>(site.address), sizeof(current));
            if (current != value && !write_code_or_data(site.address, &value, sizeof(value))) return false;
        }
        break;
    }
    case ChangeKind::BytePatch:
        if (want != site.installed) {
            const auto& bytes = want ? active.front()->bytes : site.original;
            if (!write_code_or_data(site.address, bytes.data(), bytes.size())) return false;
        }
        break;
    }
    site.installed = want;
    return true;
}

ChangeStatus ChangeRegistry::add_link(OwnerId owner, Site& site, uintptr_t detour, std::vector<uint8_t> bytes,
                                      std::string label, void** next, ChangeId* out) {
    auto change = std::make_unique<Change>();
    change->id = changes_.size() + 1;
    change->owner = owner;
    change->site = &site;
    change->detour = detour;
    change->bytes = std::move(bytes);
    change->label = std::move(label);
    if (site.kind != ChangeKind::BytePatch) {
        change->next = new_stub(site.terminal);
        if (change->next == nullptr) return refuse(owner, ChangeStatus::OutOfMemory, change->label + ": no stub memory");
    }
    Change* link = change.get();
    const auto position = std::find_if(site.links.begin(), site.links.end(), [&](const Change* c) {
        return c->owner > owner;  // same owner: later requests sit further in
    });
    site.links.insert(position, link);
    changes_.push_back(std::move(change));
    if (link->next != nullptr) set_stub(link->next, successor(site, *link));
    if (next != nullptr) *next = link->next;
    if (out != nullptr) *out = link->id;
    Owner& o = owners_[owner];
    o.staged.push_back(link->id);
    if (o.group) return ChangeStatus::Ok;
    std::vector<ChangeId> single = std::move(o.staged);
    o.staged.clear();
    const ChangeStatus status = enable(std::move(single));
    if (status != ChangeStatus::Ok) {
        if (next != nullptr) *next = nullptr;
        if (out != nullptr) *out = 0;
    }
    return status;
}

ChangeStatus ChangeRegistry::enable(std::vector<ChangeId> ids) {
    std::vector<Site*> sites;
    for (ChangeId id : ids) {
        Change& c = *changes_[id - 1];
        if (c.state != LinkState::Staged) continue;
        c.state = LinkState::Active;
        if (std::find(sites.begin(), sites.end(), c.site) == sites.end()) sites.push_back(c.site);
    }
    for (Site* site : sites) {
        if (relink(*site)) continue;
        // All or nothing: drop every change of the group again.
        const std::string failed = std::string(change_kind_name(site->kind)) + " " + where(site->address);
        OwnerId owner = 0;
        for (ChangeId id : ids) {
            Change& c = *changes_[id - 1];
            owner = c.owner;
            c.state = LinkState::Removed;
            std::erase(c.site->links, &c);
        }
        for (Site* s : sites) relink(*s);
        return refuse(owner, ChangeStatus::Internal, failed + ": cannot write the game's memory; none enabled");
    }
    return ChangeStatus::Ok;
}

ChangeStatus ChangeRegistry::hook_function(OwnerId owner, const ChangeTarget& target, void* detour, void** next,
                                           ChangeId* out) {
    if (detour == nullptr || next == nullptr) return ChangeStatus::InvalidArgument;
    std::lock_guard lock(mutex_);
    if (check_owner(owner) != ChangeStatus::Ok) return ChangeStatus::InvalidArgument;
    const std::string label = "function hook " + where(target.address);
    // At least the 5 bytes of the jump; the exact stolen length follows below.
    if (const ChangeStatus s = check_target(owner, target, 5, "function hook"); s != ChangeStatus::Ok) return s;
    Site* site = find_site(target.address, ChangeKind::FunctionHook);
    if (site != nullptr) {
        if (const ChangeStatus s = check_overlap(owner, site, site->address, site->footprint, site->kind, label);
            s != ChangeStatus::Ok) {
            return s;
        }
    } else {
        // No change covers these bytes (that would be a conflict), so the live
        // bytes are the original ones and SafetyHook may copy them.
        uint8_t code[32];
        size_t available = sizeof(code);
        if (target.inside_image) available = std::min<size_t>(available, image_base_ + image_size_ - target.address);
        while (available > 0 && !readable(target.address, available)) --available;
        if (!read_original_locked(target.address, code, available)) return ChangeStatus::InvalidArgument;
        size_t length = 0;
        std::string detail;
        if (const ChangeStatus s = stolen_length(code, available, length, detail); s != ChangeStatus::Ok) {
            return refuse(owner, s, label + ": " + detail);
        }
        if (const ChangeStatus s = check_overlap(owner, nullptr, target.address, length, ChangeKind::FunctionHook, label);
            s != ChangeStatus::Ok) {
            return s;
        }
        uint8_t* entry = new_stub(0);
        if (entry == nullptr) return refuse(owner, ChangeStatus::OutOfMemory, label + ": no stub memory");
        auto created = safetyhook::InlineHook::create(reinterpret_cast<void*>(target.address), entry,
                                                      safetyhook::InlineHook::StartDisabled);
        if (!created) {
            const ChangeStatus s = from_safetyhook(created.error(), detail);
            return refuse(owner, s, label + ": " + detail);
        }
        auto hook = std::make_unique<safetyhook::InlineHook>(std::move(*created));
        const size_t stolen = hook->original_bytes().size();
        if (stolen > length) {  // SafetyHook's 14-byte form: check the longer range
            if (const ChangeStatus s =
                    check_overlap(owner, nullptr, target.address, stolen, ChangeKind::FunctionHook, label);
                s != ChangeStatus::Ok) {
                (void)hook.release();  // never free a trampoline
                return s;
            }
        }
        site = &add_site(target.address, ChangeKind::FunctionHook, stolen);
        site->original = hook->original_bytes();
        site->terminal = hook->trampoline().address();
        site->entry = entry;
        site->hook = std::move(hook);
        set_stub(entry, site->terminal);
    }
    return add_link(owner, *site, reinterpret_cast<uintptr_t>(detour), {}, label, next, out);
}

ChangeStatus ChangeRegistry::redirect_call(OwnerId owner, const ChangeTarget& target, uintptr_t expected_callee,
                                           void* new_target, void** next, ChangeId* out) {
    if (new_target == nullptr || next == nullptr) return ChangeStatus::InvalidArgument;
    std::lock_guard lock(mutex_);
    if (check_owner(owner) != ChangeStatus::Ok) return ChangeStatus::InvalidArgument;
    const std::string label = "call redirect " + where(target.address);
    if (const ChangeStatus s = check_target(owner, target, 5, "call redirect"); s != ChangeStatus::Ok) return s;
    Site* site = find_site(target.address, ChangeKind::CallRedirect);
    if (site == nullptr) {
        uint8_t code[5];
        if (!read_original_locked(target.address, code, sizeof(code))) return ChangeStatus::InvalidArgument;
        if (code[0] != 0xE8) return refuse(owner, ChangeStatus::WrongKind, label + ": not a call rel32");
        int32_t rel = 0;
        std::memcpy(&rel, code + 1, sizeof(rel));
        const uintptr_t callee = target.address + 5 + static_cast<intptr_t>(rel);
        if (expected_callee != 0 && callee != expected_callee) {
            return refuse(owner, ChangeStatus::BytesMismatch,
                          label + ": calls " + where(callee) + ", expected " + where(expected_callee));
        }
        if (const ChangeStatus s = check_overlap(owner, nullptr, target.address, 5, ChangeKind::CallRedirect, label);
            s != ChangeStatus::Ok) {
            return s;
        }
        uint8_t* entry = new_stub(callee);
        const intptr_t delta =
            entry == nullptr ? 0 : reinterpret_cast<intptr_t>(entry) - static_cast<intptr_t>(target.address + 5);
        if (entry == nullptr || delta < INT32_MIN || delta > INT32_MAX) {
            return refuse(owner, ChangeStatus::OutOfMemory, label + ": no stub memory within rel32 reach");
        }
        site = &add_site(target.address, ChangeKind::CallRedirect, 5);
        site->original.assign(code, code + 5);
        site->terminal = callee;
        site->entry = entry;
    } else {
        if (expected_callee != 0 && site->terminal != expected_callee) {
            return refuse(owner, ChangeStatus::BytesMismatch,
                          label + ": calls " + where(site->terminal) + ", expected " + where(expected_callee));
        }
        if (const ChangeStatus s = check_overlap(owner, site, site->address, 5, site->kind, label);
            s != ChangeStatus::Ok) {
            return s;
        }
    }
    return add_link(owner, *site, reinterpret_cast<uintptr_t>(new_target), {}, label, next, out);
}

ChangeStatus ChangeRegistry::patch_bytes(OwnerId owner, const ChangeTarget& target, const void* bytes, size_t size,
                                         ChangeId* out) {
    if (bytes == nullptr || size == 0 || size > 4096) return ChangeStatus::InvalidArgument;
    std::lock_guard lock(mutex_);
    if (check_owner(owner) != ChangeStatus::Ok) return ChangeStatus::InvalidArgument;
    const std::string label = "byte patch " + where(target.address) + " (" + std::to_string(size) + " B)";
    if (const ChangeStatus s = check_target(owner, target, size, "byte patch"); s != ChangeStatus::Ok) return s;
    const auto* data = static_cast<const uint8_t*>(bytes);
    std::vector<uint8_t> patch(data, data + size);
    // Identical byte patches share ownership (§8.3).
    Site* site = nullptr;
    for (const auto& s : sites_) {
        if (s->kind != ChangeKind::BytePatch || s->address != target.address || s->footprint != size) continue;
        if (live(*s) && s->links.front()->bytes != patch) continue;  // reported by check_overlap below
        if (site == nullptr || live(*s)) site = s.get();
    }
    if (site == nullptr || !live(*site)) {
        if (const ChangeStatus s = check_overlap(owner, nullptr, target.address, size, ChangeKind::BytePatch, label);
            s != ChangeStatus::Ok) {
            return s;
        }
    }
    if (site == nullptr) {
        site = &add_site(target.address, ChangeKind::BytePatch, size);
        site->original.resize(size);
        std::memcpy(site->original.data(), reinterpret_cast<const void*>(target.address), size);
    }
    return add_link(owner, *site, 0, std::move(patch), label, nullptr, out);
}

ChangeStatus ChangeRegistry::replace_import(OwnerId owner, HMODULE image, const char* module, const char* function,
                                            void* replacement, void** next, ChangeId* out) {
    if (image == nullptr || module == nullptr || function == nullptr || replacement == nullptr || next == nullptr) {
        return ChangeStatus::InvalidArgument;
    }
    auto* base = reinterpret_cast<uint8_t*>(image);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    std::lock_guard lock(mutex_);
    if (check_owner(owner) != ChangeStatus::Ok) return ChangeStatus::InvalidArgument;
    const std::string label = std::string("import replacement ") + module + "!" + function;
    if (dir.VirtualAddress == 0) return refuse(owner, ChangeStatus::NotFound, label + ": the image has no imports");
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name != 0; ++desc) {
        if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), module) != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                                                             : desc->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), function) != 0) continue;
            const uintptr_t slot = reinterpret_cast<uintptr_t>(&slots->u1.Function);
            Site* site = find_site(slot, ChangeKind::ImportReplacement);
            if (const ChangeStatus s =
                    check_overlap(owner, site, slot, sizeof(uintptr_t), ChangeKind::ImportReplacement, label);
                s != ChangeStatus::Ok) {
                return s;
            }
            if (site == nullptr) {
                site = &add_site(slot, ChangeKind::ImportReplacement, sizeof(uintptr_t));
                const auto* current = reinterpret_cast<const uint8_t*>(slot);
                site->original.assign(current, current + sizeof(uintptr_t));
                site->terminal = static_cast<uintptr_t>(slots->u1.Function);
            }
            return add_link(owner, *site, reinterpret_cast<uintptr_t>(replacement), {}, label, next, out);
        }
    }
    return refuse(owner, ChangeStatus::NotFound, label + ": not imported");
}

ChangeStatus ChangeRegistry::remove(OwnerId owner, ChangeId id) {
    std::lock_guard lock(mutex_);
    if (id == 0 || id > changes_.size()) return ChangeStatus::NotFound;
    Change& c = *changes_[id - 1];
    if (c.owner != owner || c.state == LinkState::Removed) return ChangeStatus::NotFound;
    if (c.state == LinkState::Staged) std::erase(owners_[owner].staged, id);
    c.state = LinkState::Removed;
    std::erase(c.site->links, &c);
    if (!relink(*c.site)) return refuse(owner, ChangeStatus::Internal, c.label + ": cannot restore the game's memory");
    return ChangeStatus::Ok;
}

ChangeStatus ChangeRegistry::begin_group(OwnerId owner) {
    std::lock_guard lock(mutex_);
    if (check_owner(owner) != ChangeStatus::Ok) return ChangeStatus::InvalidArgument;
    if (owners_[owner].group) return ChangeStatus::WrongPhase;
    owners_[owner].group = true;
    return ChangeStatus::Ok;
}

ChangeStatus ChangeRegistry::commit_group(OwnerId owner) {
    std::lock_guard lock(mutex_);
    if (check_owner(owner) != ChangeStatus::Ok) return ChangeStatus::InvalidArgument;
    Owner& o = owners_[owner];
    if (!o.group) return ChangeStatus::WrongPhase;
    o.group = false;
    std::vector<ChangeId> staged = std::move(o.staged);
    o.staged.clear();
    return enable(std::move(staged));
}

void ChangeRegistry::abort_group(OwnerId owner) {
    std::lock_guard lock(mutex_);
    if (check_owner(owner) != ChangeStatus::Ok) return;
    Owner& o = owners_[owner];
    o.group = false;
    for (ChangeId id : o.staged) {
        Change& c = *changes_[id - 1];
        c.state = LinkState::Removed;
        std::erase(c.site->links, &c);
        relink(*c.site);  // staged links never changed the game; only stubs move
    }
    o.staged.clear();
}

bool ChangeRegistry::group_open(OwnerId owner) const {
    std::lock_guard lock(mutex_);
    return owner < owners_.size() && owners_[owner].group;
}

size_t ChangeRegistry::rollback(OwnerId owner) {
    std::lock_guard lock(mutex_);
    if (check_owner(owner) != ChangeStatus::Ok) return 0;
    size_t undone = 0;
    for (auto it = changes_.rbegin(); it != changes_.rend(); ++it) {
        Change& c = **it;
        if (c.owner != owner || c.state == LinkState::Removed) continue;
        undone += c.state == LinkState::Active ? 1 : 0;
        c.state = LinkState::Removed;
        std::erase(c.site->links, &c);
        relink(*c.site);
    }
    owners_[owner].group = false;
    owners_[owner].staged.clear();
    return undone;
}

bool ChangeRegistry::read_original_locked(uintptr_t address, void* out, size_t size) const {
    if (!readable(address, size)) return false;
    auto* actual = static_cast<uint8_t*>(out);
    std::memcpy(actual, reinterpret_cast<const void*>(address), size);
    // Show the game's own bytes: undo every installed change in the copy.
    for (const auto& s : sites_) {
        if (!s->installed) continue;
        const uintptr_t begin = std::max(address, s->address);
        const uintptr_t end = std::min(address + size, s->address + s->original.size());
        for (uintptr_t a = begin; a < end; ++a) actual[a - address] = s->original[a - s->address];
    }
    return true;
}

bool ChangeRegistry::read_original(uintptr_t address, void* out, size_t size) const {
    if (!in_image(address, size)) return false;
    if (size == 0) return true;
    std::lock_guard lock(mutex_);
    return read_original_locked(address, out, size);
}

ChangeStatus ChangeRegistry::expect_bytes(OwnerId owner, uintptr_t address, const void* expected, const void* mask,
                                          size_t size, std::string* mismatch) const {
    if (expected == nullptr || size == 0 || size > 65536) return ChangeStatus::InvalidArgument;
    std::lock_guard lock(mutex_);
    if (!in_image(address, size) || !readable(address, size)) return ChangeStatus::InvalidArgument;
    for (const auto& c : changes_) {
        if (c->owner == owner && c->state != LinkState::Removed) return ChangeStatus::WrongPhase;
    }
    std::vector<uint8_t> actual(size);
    read_original_locked(address, actual.data(), size);
    const auto* want = static_cast<const uint8_t*>(expected);
    const auto* bits = static_cast<const uint8_t*>(mask);
    for (size_t i = 0; i < size; ++i) {
        const uint8_t m = bits != nullptr ? bits[i] : 0xFF;
        if ((actual[i] & m) == (want[i] & m)) continue;
        if (mismatch != nullptr) {
            char text[96];
            std::snprintf(text, sizeof(text), "byte +0x%zX: expected 0x%02X, found 0x%02X", i, want[i] & m, actual[i]);
            *mismatch = text;
        }
        return ChangeStatus::BytesMismatch;
    }
    return ChangeStatus::Ok;
}

void* ChangeRegistry::alloc_near_locked(size_t size) {
    size = (size + 15) & ~static_cast<size_t>(15);
    if (size == 0 || size > 0x10000) return nullptr;
    for (auto& a : arenas_) {
        if (a.size - a.used >= size) {
            void* p = a.base + a.used;
            a.used += size;
            return p;
        }
    }
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const uintptr_t granularity = info.dwAllocationGranularity;
    const uintptr_t span = 0x7FF00000;  // stay within rel32 reach of the whole image
    const uintptr_t low = image_base_ > span ? ((image_base_ - span) & ~(granularity - 1)) : granularity;
    const uintptr_t high = image_base_ + image_size_ + span - 0x10000;
    // Try above the image first, then below.
    for (int direction = 0; direction < 2; ++direction) {
        uintptr_t candidate = direction == 0 ? ((image_base_ + image_size_ + granularity - 1) & ~(granularity - 1))
                                             : ((image_base_ - granularity) & ~(granularity - 1));
        while (candidate >= low && candidate <= high) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(candidate), &mbi, sizeof(mbi)) == 0) break;
            if (mbi.State == MEM_FREE) {
                void* block = VirtualAlloc(reinterpret_cast<void*>(candidate), 0x10000, MEM_RESERVE | MEM_COMMIT,
                                           PAGE_EXECUTE_READWRITE);
                if (block != nullptr) {
                    arenas_.push_back({static_cast<uint8_t*>(block), size, 0x10000});
                    std::memset(static_cast<uint8_t*>(block) + size, 0xCC, 0x10000 - size);
                    return block;
                }
            }
            if (direction == 0) {
                candidate = (reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize + granularity - 1) &
                            ~(granularity - 1);
            } else {
                if (candidate < granularity) break;
                candidate -= granularity;
            }
        }
    }
    return nullptr;
}

void* ChangeRegistry::alloc_near(size_t size) {
    std::lock_guard lock(mutex_);
    return alloc_near_locked(size);
}

std::vector<std::string> ChangeRegistry::describe(OwnerId owner) const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    for (const auto& c : changes_) {
        if (c->owner == owner && c->state == LinkState::Active) out.push_back(c->label);
    }
    return out;
}

size_t ChangeRegistry::count(OwnerId owner) const {
    std::lock_guard lock(mutex_);
    size_t n = 0;
    for (const auto& c : changes_) n += c->owner == owner && c->state == LinkState::Active ? 1 : 0;
    return n;
}

size_t ChangeRegistry::staged_count(OwnerId owner) const {
    std::lock_guard lock(mutex_);
    return owner < owners_.size() ? owners_[owner].staged.size() : 0;
}

std::vector<OwnerId> ChangeRegistry::chain_at(uintptr_t address) const {
    std::lock_guard lock(mutex_);
    std::vector<OwnerId> out;
    for (const auto& s : sites_) {
        if (s->address != address || !s->installed) continue;
        for (const Change* c : s->links) {
            if (c->state == LinkState::Active) out.push_back(c->owner);
        }
    }
    return out;
}

}  // namespace anvil
