#include "changes.hpp"

#include <Zydis/Zydis.h>
#include <safetyhook.hpp>
#include <safetyhook/os.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>
#include <sstream>
#include <type_traits>

// AnvilCpuContext is SafetyHook's Context64, field for field (design §8.5).
static_assert(sizeof(AnvilCpuContext) == 408 && sizeof(AnvilCpuContext) == sizeof(safetyhook::Context64));
static_assert(sizeof(AnvilXmm) == sizeof(safetyhook::Xmm) && sizeof(AnvilXmm) == 16);
static_assert(offsetof(AnvilCpuContext, xmm) == offsetof(safetyhook::Context64, xmm0));
static_assert(offsetof(AnvilCpuContext, xmm) + 15 * sizeof(AnvilXmm) == offsetof(safetyhook::Context64, xmm15));
static_assert(offsetof(AnvilCpuContext, rflags) == offsetof(safetyhook::Context64, rflags) &&
              offsetof(AnvilCpuContext, rflags) == 256);
static_assert(offsetof(AnvilCpuContext, r15) == offsetof(safetyhook::Context64, r15));
static_assert(offsetof(AnvilCpuContext, r14) == offsetof(safetyhook::Context64, r14));
static_assert(offsetof(AnvilCpuContext, r13) == offsetof(safetyhook::Context64, r13));
static_assert(offsetof(AnvilCpuContext, r12) == offsetof(safetyhook::Context64, r12));
static_assert(offsetof(AnvilCpuContext, r11) == offsetof(safetyhook::Context64, r11));
static_assert(offsetof(AnvilCpuContext, r10) == offsetof(safetyhook::Context64, r10));
static_assert(offsetof(AnvilCpuContext, r9) == offsetof(safetyhook::Context64, r9));
static_assert(offsetof(AnvilCpuContext, r8) == offsetof(safetyhook::Context64, r8));
static_assert(offsetof(AnvilCpuContext, rdi) == offsetof(safetyhook::Context64, rdi));
static_assert(offsetof(AnvilCpuContext, rsi) == offsetof(safetyhook::Context64, rsi));
static_assert(offsetof(AnvilCpuContext, rdx) == offsetof(safetyhook::Context64, rdx));
static_assert(offsetof(AnvilCpuContext, rcx) == offsetof(safetyhook::Context64, rcx));
static_assert(offsetof(AnvilCpuContext, rbx) == offsetof(safetyhook::Context64, rbx));
static_assert(offsetof(AnvilCpuContext, rax) == offsetof(safetyhook::Context64, rax));
static_assert(offsetof(AnvilCpuContext, rbp) == offsetof(safetyhook::Context64, rbp));
static_assert(offsetof(AnvilCpuContext, rsp) == offsetof(safetyhook::Context64, rsp) &&
              offsetof(AnvilCpuContext, rsp) == 384);
static_assert(offsetof(AnvilCpuContext, reserved) == offsetof(safetyhook::Context64, trampoline_rsp));
static_assert(offsetof(AnvilCpuContext, rip) == offsetof(safetyhook::Context64, rip) &&
              offsetof(AnvilCpuContext, rip) == 400);

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

std::string describe_call_count(const CallCount& count) {
    std::string line = count.label;
    if (!count.symbol.empty()) line += " [" + count.symbol + "]";
    return line + ": " + std::to_string(count.calls) + (count.calls == 1 ? " call" : " calls");
}

const char* address_role_name(AddressRole role) {
    switch (role) {
    case AddressRole::None: return "none";
    case AddressRole::ChangedBytes: return "changed bytes";
    case AddressRole::Stub: return "stub";
    case AddressRole::Trampoline: return "trampoline";
    case AddressRole::StubMemory: return "stub memory";
    }
    return "?";
}

const char* change_kind_name(ChangeKind kind) {
    switch (kind) {
    case ChangeKind::FunctionHook: return "function hook";
    case ChangeKind::MidHook: return "mid hook";
    case ChangeKind::BlockReplacement: return "block replacement";
    case ChangeKind::CallRedirect: return "call redirect";
    case ChangeKind::BytePatch: return "byte patch";
    case ChangeKind::PointerSwap: return "pointer swap";
    case ChangeKind::ImportReplacement: return "import replacement";
    }
    return "change";
}

namespace {

// A link's life. Only Active links are part of a chain or keep a byte patch
// written. Staged and Suspended links are skipped, but they keep their place in
// the chain and their bytes reserved; Removed ones have left the site.
enum class LinkState : uint8_t { Staged, Active, Suspended, Removed };

// Kinds whose links pass on to the next one: through a `next` stub, or (a
// pointer swap) through the plugin's `previous` cell.
bool chains(ChangeKind kind) {
    return kind == ChangeKind::FunctionHook || kind == ChangeKind::CallRedirect || kind == ChangeKind::PointerSwap ||
           kind == ChangeKind::ImportReplacement;
}

// Jump stub: jmp qword ptr [rip+2]; int3 x2; dq target. The target sits 8-byte
// aligned at +8, so re-pointing a link is one atomic store.
constexpr size_t kStubSize = 16;

void set_stub(uint8_t* stub, uintptr_t target) {
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(stub + 8), static_cast<LONG64>(target));
}

// Context stub of mid hooks and block replacements: SafetyHook 0.7.0's x64
// Windows mid-hook stub (src/mid_hook.cpp, MIT), byte for byte, so the saved
// context is its Context64. It pushes the qword at +383 as `rip`, saves
// RFLAGS, the GPRs and XMM0-15, calls the function at +375 with rcx = the
// context, restores everything and returns to the context's `rip`. Anvil
// stores the site's CpuStub at +383; dispatch() replaces it with the real
// resume address before any plugin sees the context.
constexpr uint8_t kCpuStub[391] = {
    0xFF, 0x35, 0x79, 0x01, 0x00, 0x00, 0x54, 0x54, 0x55, 0x50, 0x53, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51,
    0x41, 0x52, 0x41, 0x53, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x9C, 0x48, 0x81, 0xEC, 0x00, 0x01, 0x00,
    0x00, 0xF3, 0x44, 0x0F, 0x7F, 0xBC, 0x24, 0xF0, 0x00, 0x00, 0x00, 0xF3, 0x44, 0x0F, 0x7F, 0xB4, 0x24, 0xE0, 0x00,
    0x00, 0x00, 0xF3, 0x44, 0x0F, 0x7F, 0xAC, 0x24, 0xD0, 0x00, 0x00, 0x00, 0xF3, 0x44, 0x0F, 0x7F, 0xA4, 0x24, 0xC0,
    0x00, 0x00, 0x00, 0xF3, 0x44, 0x0F, 0x7F, 0x9C, 0x24, 0xB0, 0x00, 0x00, 0x00, 0xF3, 0x44, 0x0F, 0x7F, 0x94, 0x24,
    0xA0, 0x00, 0x00, 0x00, 0xF3, 0x44, 0x0F, 0x7F, 0x8C, 0x24, 0x90, 0x00, 0x00, 0x00, 0xF3, 0x44, 0x0F, 0x7F, 0x84,
    0x24, 0x80, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x7F, 0x7C, 0x24, 0x70, 0xF3, 0x0F, 0x7F, 0x74, 0x24, 0x60, 0xF3, 0x0F,
    0x7F, 0x6C, 0x24, 0x50, 0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x40, 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x30, 0xF3, 0x0F, 0x7F,
    0x54, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x10, 0xF3, 0x0F, 0x7F, 0x04, 0x24, 0x48, 0x8B, 0x8C, 0x24, 0x80,
    0x01, 0x00, 0x00, 0x48, 0x83, 0xC1, 0x10, 0x48, 0x89, 0x8C, 0x24, 0x80, 0x01, 0x00, 0x00, 0x48, 0x8D, 0x0C, 0x24,
    0x48, 0x89, 0xE3, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x83, 0xE4, 0xF0, 0xFF, 0x15, 0xA8, 0x00, 0x00, 0x00, 0x48, 0x89,
    0xDC, 0xF3, 0x0F, 0x6F, 0x04, 0x24, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x10, 0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x20, 0xF3,
    0x0F, 0x6F, 0x5C, 0x24, 0x30, 0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x40, 0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x50, 0xF3, 0x0F,
    0x6F, 0x74, 0x24, 0x60, 0xF3, 0x0F, 0x6F, 0x7C, 0x24, 0x70, 0xF3, 0x44, 0x0F, 0x6F, 0x84, 0x24, 0x80, 0x00, 0x00,
    0x00, 0xF3, 0x44, 0x0F, 0x6F, 0x8C, 0x24, 0x90, 0x00, 0x00, 0x00, 0xF3, 0x44, 0x0F, 0x6F, 0x94, 0x24, 0xA0, 0x00,
    0x00, 0x00, 0xF3, 0x44, 0x0F, 0x6F, 0x9C, 0x24, 0xB0, 0x00, 0x00, 0x00, 0xF3, 0x44, 0x0F, 0x6F, 0xA4, 0x24, 0xC0,
    0x00, 0x00, 0x00, 0xF3, 0x44, 0x0F, 0x6F, 0xAC, 0x24, 0xD0, 0x00, 0x00, 0x00, 0xF3, 0x44, 0x0F, 0x6F, 0xB4, 0x24,
    0xE0, 0x00, 0x00, 0x00, 0xF3, 0x44, 0x0F, 0x6F, 0xBC, 0x24, 0xF0, 0x00, 0x00, 0x00, 0x48, 0x81, 0xC4, 0x00, 0x01,
    0x00, 0x00, 0x9D, 0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58,
    0x5F, 0x5E, 0x5A, 0x59, 0x5B, 0x58, 0x5D, 0x48, 0x8D, 0x64, 0x24, 0x08, 0x5C, 0xC3, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr size_t kCpuStubDestination = 375;
constexpr size_t kCpuStubState = 383;
// After the stub's `call qword [rip+0xA8]` (to +375): a return address there
// means rbx holds the saved context (the stub's `mov rbx, rsp`).
constexpr size_t kCpuStubReturn = 207;
static_assert(kCpuStub[kCpuStubReturn - 6] == 0xFF && kCpuStub[kCpuStubReturn - 5] == 0x15 &&
              kCpuStubReturn + 0xA8 == kCpuStubDestination);
static_assert(kCpuStub[kCpuStubReturn - 17] == 0x48 && kCpuStub[kCpuStubReturn - 16] == 0x89 &&
              kCpuStub[kCpuStubReturn - 15] == 0xE3);  // mov rbx, rsp (rsp = the context), then the alignment

// Call stub: the original `call rel32` of a code site, so mid hooks at a call
// pass on to the callee or to the redirect chain with the game's own return
// address. lea rsp,[rsp-8]; mov dword [rsp], lo; mov dword [rsp+4], hi;
// jmp qword ptr [rip+6]; int3 x6; dq target. No register or flag changes;
// the target sits 8-byte aligned at +32.
constexpr size_t kCallStubSize = 40;

// Counting stub (profile = true), in front of a function hook's or call
// redirect's detour: push rax; mov rax, counter; lock inc qword [rax]; pop rax;
// jmp qword ptr [rip+2]; int3 x2; dq target. Flags are dead at a function
// entry and at a call, so only rax needs saving. The target sits at +24.
constexpr size_t kCountingStubSize = 32;

// Counters live apart from code, one per cache line: increments neither touch
// code bytes nor contend with another change's counter.
struct alignas(64) CallCounter {
    std::atomic<uint64_t> calls{0};
    uint8_t rest_of_the_line[56] = {};
};
static_assert(sizeof(CallCounter) == 64);

void set_call_stub(uint8_t* stub, uintptr_t target) {
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(stub + 32), static_cast<LONG64>(target));
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

// An aligned pointer in one atomic store, in any memory: the page is made
// writable for the store (keeping it executable if it was) and restored.
bool store_pointer(uintptr_t address, uintptr_t value) {
    constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) {
        return false;
    }
    auto* slot = reinterpret_cast<void*>(address);
    const bool code = executable(address);
    const bool unprotect = (mbi.Protect & kWritable) == 0;
    DWORD old = 0;
    if (unprotect && !VirtualProtect(slot, sizeof(value), code ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old)) {
        return false;
    }
    InterlockedExchangePointer(static_cast<void* volatile*>(slot), reinterpret_cast<void*>(value));
    DWORD ignored = 0;
    if (unprotect) VirtualProtect(slot, sizeof(value), old, &ignored);
    if (code) FlushInstructionCache(GetCurrentProcess(), slot, sizeof(value));
    return true;
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

// A replaced block must be whole instructions: the game resumes at its end.
bool whole_instructions(const uint8_t* code, size_t size) {
    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) return false;
    size_t at = 0;
    while (at < size) {
        ZydisDecodedInstruction instruction;
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, nullptr, code + at, size - at, &instruction))) {
            return false;
        }
        at += instruction.length;
    }
    return at == size;
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

// A site's original bytes: up to 16 in place (stolen bytes, a call, a slot,
// a short patch), a longer byte patch on the heap. The part of std::vector a
// site uses, without a heap block for the usual few bytes.
class OriginalBytes {
public:
    OriginalBytes() = default;
    OriginalBytes(const OriginalBytes&) = delete;
    OriginalBytes& operator=(const OriginalBytes&) = delete;
    ~OriginalBytes() { resize(0); }

    void resize(size_t size) {
        if (size_ > sizeof(local_)) delete[] heap_;
        size_ = size;
        if (size_ > sizeof(local_)) heap_ = new uint8_t[size_]();
        else std::memset(local_, 0, sizeof(local_));
    }
    void assign(const uint8_t* first, const uint8_t* last) {
        resize(static_cast<size_t>(last - first));
        std::memcpy(data(), first, size_);
    }
    uint8_t* data() { return size_ > sizeof(local_) ? heap_ : local_; }
    const uint8_t* data() const { return size_ > sizeof(local_) ? heap_ : local_; }
    size_t size() const { return size_; }
    uint8_t operator[](size_t i) const { return data()[i]; }

private:
    union {
        uint8_t local_[16] = {};
        uint8_t* heap_;
    };
    size_t size_ = 0;
};

}  // namespace

// One change: a link of a chain, or one owner of a (shared) byte patch. Kept
// small: there is one per change and the budget is per change (design §17).
struct ChangeRegistry::Change {
    ChangeId id = 0;
    Site* site = nullptr;
    OwnerId owner = 0;
    uint32_t span = 0;           // block: its size, owned from the site's address
    ChangeKind kind = ChangeKind::BytePatch;
    LinkState state = LinkState::Staged;
    uintptr_t detour = 0;        // chain kinds: where this link goes; mid hook, block: the callback
    uint8_t* next = nullptr;     // chain kinds but pointer swaps: this link's own next stub
    void** previous = nullptr;   // pointer swap: the plugin's cell for the next value, or none
    void* user = nullptr;        // mid hook, block: the callback's argument
    uintptr_t resume = 0;        // block: where the game goes on
    std::unique_ptr<uint8_t[]> bytes;      // byte patch: the site's footprint
    std::unique_ptr<char[]> names;         // the label, then the declared symbol ("" for none)
    std::unique_ptr<CallCounter> counter;  // profile = true: function hook, redirect, mid hook, block
    uint8_t* counting = nullptr;           // profile = true, chain kinds in code: the counting stub

    const char* label() const { return names.get(); }
    const char* symbol() const { return names.get() + std::strlen(names.get()) + 1; }
};

// The callbacks behind a code site's context stub, published as a whole so a
// thread in the stub reads one consistent list. One heap block: the header,
// then `count` items. Never freed while the registry lives: a thread may still
// read an older one, so each list keeps the one it replaced.
struct ChangeRegistry::Dispatch {
    struct Item {
        AnvilCpuCallback callback;
        void* user;
        OwnerId owner;
        bool mid;  // a mid hook (a rip change ends the chain); else the block replacement
        std::atomic<uint64_t>* counter;  // profile = true
    };
    const Dispatch* older = nullptr;
    uintptr_t resume = 0;  // where the game goes on unless a callback writes rip
    size_t count = 0;      // the mid hooks in chain order, then the block replacement
    const Item* items() const { return reinterpret_cast<const Item*>(this + 1); }
    Item* items() { return reinterpret_cast<Item*>(this + 1); }
};

// A code site's context stub (kCpuStub) and what dispatch() needs.
struct ChangeRegistry::CpuStub {
    uint8_t* code = nullptr;
    ChangeRegistry* registry = nullptr;
    uintptr_t address = 0;
    std::atomic<const Dispatch*> current{nullptr};  // with every older list behind it
    std::atomic<bool> rip_noticed{false};           // the first rip change here is reported, once

    CpuStub() = default;
    CpuStub(const CpuStub&) = delete;
    CpuStub& operator=(const CpuStub&) = delete;
    ~CpuStub() {
        for (const Dispatch* list = current.load(); list != nullptr;) {
            const Dispatch* older = list->older;
            ::operator delete(const_cast<Dispatch*>(list));
            list = older;
        }
    }
};

// Everything at one address of one kind. The game is changed only while a site
// has Active links; a site without links stays (its stubs and hook are never
// freed) and is reused by the next request of the same kind. Function hooks,
// mid hooks and block replacements share one code site (kind FunctionHook).
struct ChangeRegistry::Site {
    ChangeKind kind = ChangeKind::BytePatch;
    uintptr_t address = 0;
    size_t footprint = 0;           // bytes this site owns: stolen bytes, call, slot, patch
    OriginalBytes original;         // those bytes as the game shipped them
    uintptr_t terminal = 0;         // where the innermost next leads: trampoline (or call stub), callee, pointer
    uint8_t* entry = nullptr;       // code site, redirect: the stub the game jumps to first
    std::unique_ptr<safetyhook::InlineHook> hook;  // code site
    std::unique_ptr<CpuStub> cpu;   // code site with mid hooks or a block replacement (once)
    uint8_t* call_stub = nullptr;   // code site on a `call rel32`: the call, to the callee or the redirect chain
    bool installed = false;         // the game's bytes are changed
    std::vector<Change*> links;     // not Removed, in chain order (owner, then request)
};

struct ChangeRegistry::Owner {
    std::string name;
    bool group = false;
    bool suspended = false;  // its enabled changes are Suspended, new ones too
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

void ChangeRegistry::set_notice_sink(NoticeSink sink) {
    std::lock_guard lock(notice_mutex_);
    notice_ = std::move(sink);
}

void ChangeRegistry::set_profile(bool on) {
    std::lock_guard lock(mutex_);
    profile_ = on;
}

// Called by a code site's context stub on the game's thread, without the
// registry's lock: it reads only the published list. dispatch_counted is the
// stub's target instead when the site was created with profile = true.
void ChangeRegistry::dispatch(AnvilCpuContext* cpu) { run_callbacks<false>(cpu); }
void ChangeRegistry::dispatch_counted(AnvilCpuContext* cpu) { run_callbacks<true>(cpu); }

template <bool Count> void ChangeRegistry::run_callbacks(AnvilCpuContext* cpu) {
    auto* stub = reinterpret_cast<CpuStub*>(cpu->rip);  // kCpuStubState, pushed as rip
    const Dispatch* list = stub->current.load(std::memory_order_acquire);
    cpu->rip = list->resume;
    const Dispatch::Item* const end = list->items() + list->count;
    for (const Dispatch::Item* at = list->items(); at != end; ++at) {
        const Dispatch::Item& item = *at;
        if constexpr (Count) {
            if (item.counter != nullptr) item.counter->fetch_add(1, std::memory_order_relaxed);
        }
        item.callback(cpu, item.user);
        if (cpu->rip == list->resume) continue;
        // A new rip ends the chain here (§8.2): the rest is skipped.
        if (item.mid && !stub->rip_noticed.exchange(true)) {
            try {
                ChangeRegistry& registry = *stub->registry;
                const std::string message = "mid hook " + registry.where(stub->address) + " changed rip to " +
                                            registry.where(cpu->rip) +
                                            ": the rest of the chain at this address is skipped (reported once)";
                std::lock_guard lock(registry.notice_mutex_);
                if (registry.notice_) registry.notice_(item.owner, message);
            } catch (...) {
                // Never unwind into the game.
            }
        }
        return;
    }
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

// The bytes a live site owns: a block replacement owns its whole block.
size_t ChangeRegistry::extent(const Site& site) const {
    size_t size = site.footprint;
    for (const Change* c : site.links) size = std::max<size_t>(size, c->span);
    return size;
}

// The kinds of a site's links, e.g. "mid hook and function hook".
std::string ChangeRegistry::site_name(const Site& site) const {
    if (site.kind != ChangeKind::FunctionHook) return change_kind_name(site.kind);
    std::string name;
    for (const ChangeKind kind : {ChangeKind::MidHook, ChangeKind::BlockReplacement, ChangeKind::FunctionHook}) {
        if (std::none_of(site.links.begin(), site.links.end(), [&](const Change* c) { return c->kind == kind; })) {
            continue;
        }
        name += (name.empty() ? "" : " and ") + std::string(change_kind_name(kind));
    }
    return name.empty() ? change_kind_name(site.kind) : name;
}

// `partner`: a site at the same address that may stay (a redirected call
// under mid hooks).
ChangeStatus ChangeRegistry::check_overlap(OwnerId owner, const Site* joining, uintptr_t address, size_t size,
                                           ChangeKind kind, const std::string& what, const Site* partner) {
    for (const auto& s : sites_) {
        if (s.get() == joining || s.get() == partner || !live(*s)) continue;
        const size_t owned = extent(*s);
        if (!(address < s->address + owned && s->address < address + size)) continue;
        std::string owners;
        for (const Change* c : s->links) {
            const std::string& name = owners_[c->owner].name;
            if (owners.find("'" + name + "'") != std::string::npos) continue;
            owners += (owners.empty() ? "'" : ", '") + name + "'";
        }
        std::string message = what + " overlaps the " + site_name(*s) + " " + where(s->address) + " (" +
                              std::to_string(owned) + " B) of " + owners;
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

// Points a chained link at `target`: its next stub, or the value in its
// previous cell. One atomic store either way.
void ChangeRegistry::point(const Change& link, uintptr_t target) {
    if (link.next != nullptr) {
        set_stub(link.next, target);
    } else if (link.previous != nullptr) {
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(link.previous), reinterpret_cast<void*>(target));
    }
}

// Where a chain enters a link: its counting stub when it counts, else its detour.
uintptr_t ChangeRegistry::entry_of(const Change& link) {
    return link.counting != nullptr ? reinterpret_cast<uintptr_t>(link.counting) : link.detour;
}

// The next chained link (function hook, redirect, pointer swap, import) after `link`.
uintptr_t ChangeRegistry::successor(const Site& site, const Change& link) const {
    bool after = false;
    for (const Change* c : site.links) {
        if (c == &link) {
            after = true;
        } else if (after && chains(c->kind) && c->state == LinkState::Active) {
            return entry_of(*c);
        }
    }
    return site.terminal;
}

// Makes `callbacks` with `resume` the list the site's context stub runs; a
// new list only when it differs from the current one.
void ChangeRegistry::publish(Site& site, uintptr_t resume, const std::vector<Change*>& callbacks) {
    CpuStub& cpu = *site.cpu;
    const Dispatch* now = cpu.current.load(std::memory_order_relaxed);
    const auto same = [&] {
        if (now == nullptr || now->resume != resume || now->count != callbacks.size()) return false;
        for (size_t i = 0; i < callbacks.size(); ++i) {
            const Dispatch::Item& item = now->items()[i];
            const std::atomic<uint64_t>* counter = callbacks[i]->counter ? &callbacks[i]->counter->calls : nullptr;
            if (item.callback != reinterpret_cast<AnvilCpuCallback>(callbacks[i]->detour) ||
                item.user != callbacks[i]->user || item.owner != callbacks[i]->owner || item.counter != counter) {
                return false;
            }
        }
        return true;
    };
    if (same()) return;
    static_assert(sizeof(Dispatch) % alignof(Dispatch::Item) == 0 && std::is_trivially_destructible_v<Dispatch> &&
                  std::is_trivially_destructible_v<Dispatch::Item>);
    auto* list = new (::operator new(sizeof(Dispatch) + callbacks.size() * sizeof(Dispatch::Item))) Dispatch;
    list->older = now;
    list->resume = resume;
    list->count = callbacks.size();
    for (size_t i = 0; i < callbacks.size(); ++i) {
        const Change* c = callbacks[i];
        new (list->items() + i) Dispatch::Item{reinterpret_cast<AnvilCpuCallback>(c->detour), c->user, c->owner,
                                               c->kind == ChangeKind::MidHook,
                                               c->counter ? &c->counter->calls : nullptr};
    }
    cpu.current.store(list, std::memory_order_release);
}

// Points every link and the site's entry at the Active links, innermost first,
// so a thread entering the chain at any moment finds a complete one; installs
// or uninstalls the site when its first link comes or its last one goes.
bool ChangeRegistry::relink(Site& site) {
    std::vector<Change*> active;
    for (Change* c : site.links) {
        if (c->state == LinkState::Active) active.push_back(c);
    }
    // The chained links (function hooks, redirects, pointer swaps, imports):
    // each next (or previous) leads to the following Active one.
    std::vector<Change*> chained;
    for (Change* c : site.links) {
        if (!chains(c->kind)) continue;
        if (c->state == LinkState::Active) {
            chained.push_back(c);
        } else {
            point(*c, successor(site, *c));
        }
    }
    for (size_t i = chained.size(); i-- > 0;) {
        point(*chained[i], i + 1 < chained.size() ? entry_of(*chained[i + 1]) : site.terminal);
    }
    const uintptr_t chain = chained.empty() ? site.terminal : entry_of(*chained.front());
    const bool want = !active.empty();
    switch (site.kind) {
    case ChangeKind::FunctionHook:
    case ChangeKind::MidHook:
    case ChangeKind::BlockReplacement: {
        // Mid hooks first, then the block replacement or the function-hook
        // chain (or, at a call, the call stub and so the redirect chain).
        std::vector<Change*> callbacks;
        Change* block = nullptr;
        for (Change* c : active) {
            if (c->kind == ChangeKind::MidHook) callbacks.push_back(c);
            if (c->kind == ChangeKind::BlockReplacement) block = c;
        }
        if (block != nullptr) callbacks.push_back(block);
        if (site.cpu) publish(site, block != nullptr ? block->resume : chain, callbacks);
        // Uninstalled, the entry leads past every link: a thread already on
        // its way in skips the removed ones.
        set_stub(site.entry, callbacks.empty() ? chain : reinterpret_cast<uintptr_t>(site.cpu->code));
        if (want != site.installed) {
            if (!(want ? site.hook->enable() : site.hook->disable())) return false;
            if (!want) {
                // SafetyHook wrote back the bytes it found: a redirect at this
                // call writes its own again.
                site.installed = false;
                if (Site* call = find_site(site.address, ChangeKind::CallRedirect)) return relink(*call);
            }
        }
        break;
    }
    case ChangeKind::CallRedirect: {
        set_stub(site.entry, chain);
        // Under an installed code site (mid hooks) the bytes are the code
        // site's; its call stub leads to this entry.
        const Site* code = find_site(site.address, ChangeKind::FunctionHook);
        if (code != nullptr && code->installed) break;
        uint8_t call[5] = {0xE8, 0, 0, 0, 0};
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(site.entry) -
                                                 static_cast<intptr_t>(site.address + 5));
        std::memcpy(call + 1, &rel, sizeof(rel));
        const uint8_t* bytes = want ? call : site.original.data();
        if (!readable(site.address, 5)) return false;
        if (std::memcmp(reinterpret_cast<const void*>(site.address), bytes, 5) != 0 &&
            !write_code_or_data(site.address, bytes, 5)) {
            return false;
        }
        break;
    }
    case ChangeKind::PointerSwap:
    case ChangeKind::ImportReplacement: {
        // The slot last: every previous already leads on.
        const uintptr_t value = want ? chain : site.terminal;
        if (want || site.installed) {
            uintptr_t current = 0;
            std::memcpy(&current, reinterpret_cast<const void*>(site.address), sizeof(current));
            if (current != value && !store_pointer(site.address, value)) return false;
        }
        break;
    }
    case ChangeKind::BytePatch:
        if (want != site.installed) {
            const uint8_t* bytes = want ? active.front()->bytes.get() : site.original.data();
            if (!write_code_or_data(site.address, bytes, site.footprint)) return false;
        }
        break;
    }
    site.installed = want;
    return true;
}

ChangeStatus ChangeRegistry::add_link(OwnerId owner, Site& site, ChangeKind kind, uintptr_t detour,
                                      std::unique_ptr<uint8_t[]> bytes, const std::string& label, void** next,
                                      ChangeId* out, void* user, uintptr_t resume, size_t span, const char* symbol) {
    auto change = std::make_unique<Change>();
    change->id = changes_.size() + 1;
    change->owner = owner;
    change->site = &site;
    change->kind = kind;
    change->detour = detour;
    change->user = user;
    change->resume = resume;
    change->span = static_cast<uint32_t>(span);
    change->bytes = std::move(bytes);
    // One block for both strings: the label, a NUL, the symbol, a NUL.
    const size_t symbol_size = symbol != nullptr ? std::strlen(symbol) : 0;
    change->names = std::make_unique<char[]>(label.size() + symbol_size + 2);  // zeroed
    std::memcpy(change->names.get(), label.data(), label.size());
    if (symbol_size != 0) std::memcpy(change->names.get() + label.size() + 1, symbol, symbol_size);
    if (profile_ && (kind == ChangeKind::FunctionHook || kind == ChangeKind::CallRedirect ||
                     kind == ChangeKind::MidHook || kind == ChangeKind::BlockReplacement)) {
        change->counter = std::make_unique<CallCounter>();
        if (kind == ChangeKind::FunctionHook || kind == ChangeKind::CallRedirect) {
            change->counting = static_cast<uint8_t*>(alloc_near_locked(kCountingStubSize));
            if (change->counting == nullptr) {
                return refuse(owner, ChangeStatus::OutOfMemory, label + ": no stub memory");
            }
            const uintptr_t counter = reinterpret_cast<uintptr_t>(&change->counter->calls);
            const uint8_t code[24] = {0x50,                                      // push rax
                                      0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,        // mov rax, counter
                                      0xF0, 0x48, 0xFF, 0x00,                    // lock inc qword [rax]
                                      0x58,                                      // pop rax
                                      0xFF, 0x25, 0x02, 0x00, 0x00, 0x00,        // jmp qword [rip+2]
                                      0xCC, 0xCC};
            std::memcpy(change->counting, code, sizeof(code));
            std::memcpy(change->counting + 3, &counter, sizeof(counter));
            std::memcpy(change->counting + 24, &detour, sizeof(detour));
            FlushInstructionCache(GetCurrentProcess(), change->counting, kCountingStubSize);
        }
    }
    if (kind == ChangeKind::PointerSwap) {
        change->previous = next;  // a value has no stub: the plugin's cell is re-pointed
    } else if (chains(kind)) {
        change->next = new_stub(site.terminal);
        if (change->next == nullptr) return refuse(owner, ChangeStatus::OutOfMemory, label + ": no stub memory");
    }
    Change* link = change.get();
    const auto position = std::find_if(site.links.begin(), site.links.end(), [&](const Change* c) {
        return c->owner > owner;  // same owner: later requests sit further in
    });
    site.links.insert(position, link);
    changes_.push_back(std::move(change));
    if (chains(kind)) point(*link, successor(site, *link));
    if (next != nullptr && kind != ChangeKind::PointerSwap) *next = link->next;
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
        c.state = owners_[c.owner].suspended ? LinkState::Suspended : LinkState::Active;
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

// The code site at target.address, created on first use: one SafetyHook hook
// whose stolen bytes are checked for overlaps and unsafe instructions. `span`
// is what a block replacement owns from the address. A redirected call at the
// same address stays only under mid hooks, which then pass on to it.
ChangeStatus ChangeRegistry::code_site(OwnerId owner, const ChangeTarget& target, ChangeKind kind, size_t span,
                                       const std::string& label, Site*& out) {
    Site* site = find_site(target.address, ChangeKind::FunctionHook);
    Site* call = find_site(target.address, ChangeKind::CallRedirect);
    const Site* partner = kind == ChangeKind::MidHook ? call : nullptr;
    if (site != nullptr) {
        // At one address: mid hooks, then a block replacement or function hooks.
        for (const Change* c : site->links) {
            const bool clash = kind == ChangeKind::BlockReplacement ? c->kind != ChangeKind::MidHook
                                                                    : kind == ChangeKind::FunctionHook &&
                                                                          c->kind == ChangeKind::BlockReplacement;
            if (!clash) continue;
            return refuse(owner, ChangeStatus::Conflict,
                          label + ": " + where(target.address) + " already has a " + change_kind_name(c->kind) +
                              " of '" + owners_[c->owner].name + "'; refused");
        }
        if (span != 0 && span < site->footprint) {
            return refuse(owner, ChangeStatus::UnsafeSite,
                          label + ": the hook's jump (" + std::to_string(site->footprint) +
                              " B) is longer than the block");
        }
        if (partner != nullptr && site->call_stub == nullptr) partner = nullptr;  // reported as an overlap
        if (const ChangeStatus s = check_overlap(owner, site, site->address, std::max(extent(*site), span), kind,
                                                 label, partner);
            s != ChangeStatus::Ok) {
            return s;
        }
    } else {
        // No change covers these bytes (that would be a conflict, a redirected
        // call aside), so SafetyHook copies the original bytes or that call.
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
        if (span != 0 && span < length) {
            return refuse(owner, ChangeStatus::UnsafeSite,
                          label + ": the hook's jump (" + std::to_string(length) + " B) is longer than the block");
        }
        if (const ChangeStatus s =
                check_overlap(owner, nullptr, target.address, std::max(length, span), kind, label, partner);
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
            ChangeStatus s = ChangeStatus::Ok;
            if (span != 0 && span < stolen) {
                s = refuse(owner, ChangeStatus::UnsafeSite,
                           label + ": the hook's jump (" + std::to_string(stolen) + " B) is longer than the block");
            } else if (partner != nullptr && live(*partner)) {
                s = refuse(owner, ChangeStatus::UnsafeSite,
                           label + ": the hook's jump (" + std::to_string(stolen) + " B) is longer than the call");
            } else {
                s = check_overlap(owner, nullptr, target.address, std::max(stolen, span), kind, label, partner);
            }
            if (s != ChangeStatus::Ok) {
                (void)hook.release();  // never free a trampoline
                return s;
            }
        }
        site = &add_site(target.address, ChangeKind::FunctionHook, stolen);
        // The shipped bytes: SafetyHook may have found a redirect's call.
        site->original.resize(stolen);
        read_original_locked(target.address, site->original.data(), stolen);
        site->terminal = hook->trampoline().address();
        site->entry = entry;
        site->hook = std::move(hook);
        if (stolen == 5 && site->original[0] == 0xE8 && !add_call_stub(*site, call)) {
            return refuse(owner, ChangeStatus::OutOfMemory, label + ": no stub memory");
        }
        set_stub(entry, site->terminal);
    }
    if (kind != ChangeKind::FunctionHook && !site->cpu) {
        auto* code = static_cast<uint8_t*>(alloc_near_locked(sizeof(kCpuStub)));
        if (code == nullptr) return refuse(owner, ChangeStatus::OutOfMemory, label + ": no stub memory");
        auto cpu = std::make_unique<CpuStub>();
        std::memcpy(code, kCpuStub, sizeof(kCpuStub));
        const uintptr_t destination = reinterpret_cast<uintptr_t>(profile_ ? &ChangeRegistry::dispatch_counted
                                                                           : &ChangeRegistry::dispatch);
        const uintptr_t state = reinterpret_cast<uintptr_t>(cpu.get());
        std::memcpy(code + kCpuStubDestination, &destination, sizeof(destination));
        std::memcpy(code + kCpuStubState, &state, sizeof(state));
        FlushInstructionCache(GetCurrentProcess(), code, sizeof(kCpuStub));
        cpu->code = code;
        cpu->registry = this;
        cpu->address = site->address;
        site->cpu = std::move(cpu);
    }
    out = site;
    return ChangeStatus::Ok;
}

// The call stub of a code site on `call rel32`: to the redirect chain's entry
// when `call` exists, else to the original callee. It becomes the site's
// terminal, so mid hooks pass on with the game's own return address.
bool ChangeRegistry::add_call_stub(Site& code, const Site* call) {
    auto* stub = static_cast<uint8_t*>(alloc_near_locked(kCallStubSize));
    if (stub == nullptr) return false;
    const uintptr_t back = code.address + 5;
    const uint32_t low = static_cast<uint32_t>(back), high = static_cast<uint32_t>(back >> 32);
    const uint8_t head[32] = {0x48, 0x8D, 0x64, 0x24, 0xF8,                 // lea rsp, [rsp-8]
                              0xC7, 0x04, 0x24, 0, 0, 0, 0,                 // mov dword [rsp], low
                              0xC7, 0x44, 0x24, 0x04, 0, 0, 0, 0,           // mov dword [rsp+4], high
                              0xFF, 0x25, 0x06, 0x00, 0x00, 0x00,           // jmp qword [rip+6]
                              0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    std::memcpy(stub, head, sizeof(head));
    std::memcpy(stub + 8, &low, sizeof(low));
    std::memcpy(stub + 16, &high, sizeof(high));
    int32_t rel = 0;
    std::memcpy(&rel, code.original.data() + 1, sizeof(rel));
    const uintptr_t callee = code.address + 5 + static_cast<intptr_t>(rel);
    set_call_stub(stub, call != nullptr ? reinterpret_cast<uintptr_t>(call->entry) : callee);
    FlushInstructionCache(GetCurrentProcess(), stub, kCallStubSize);
    code.call_stub = stub;
    code.terminal = reinterpret_cast<uintptr_t>(stub);
    return true;
}

ChangeStatus ChangeRegistry::hook_function(OwnerId owner, const ChangeTarget& target, void* detour, void** next,
                                           ChangeId* out) {
    if (detour == nullptr || next == nullptr) return ChangeStatus::InvalidArgument;
    std::lock_guard lock(mutex_);
    if (check_owner(owner) != ChangeStatus::Ok) return ChangeStatus::InvalidArgument;
    const std::string label = "function hook " + where(target.address);
    // At least the 5 bytes of the jump; the exact stolen length follows below.
    if (const ChangeStatus s = check_target(owner, target, 5, "function hook"); s != ChangeStatus::Ok) return s;
    Site* site = nullptr;
    if (const ChangeStatus s = code_site(owner, target, ChangeKind::FunctionHook, 0, label, site); s != ChangeStatus::Ok) {
        return s;
    }
    return add_link(owner, *site, ChangeKind::FunctionHook, reinterpret_cast<uintptr_t>(detour), {}, label, next, out,
                    nullptr, 0, 0, target.symbol);
}

ChangeStatus ChangeRegistry::mid_hook(OwnerId owner, const ChangeTarget& target, AnvilCpuCallback callback,
                                      void* user, ChangeId* out) {
    if (callback == nullptr) return ChangeStatus::InvalidArgument;
    std::lock_guard lock(mutex_);
    if (check_owner(owner) != ChangeStatus::Ok) return ChangeStatus::InvalidArgument;
    const std::string label = "mid hook " + where(target.address);
    if (const ChangeStatus s = check_target(owner, target, 5, "mid hook"); s != ChangeStatus::Ok) return s;
    Site* site = nullptr;
    if (const ChangeStatus s = code_site(owner, target, ChangeKind::MidHook, 0, label, site); s != ChangeStatus::Ok) {
        return s;
    }
    return add_link(owner, *site, ChangeKind::MidHook, reinterpret_cast<uintptr_t>(callback), {}, label, nullptr, out,
                    user, 0, 0, target.symbol);
}

ChangeStatus ChangeRegistry::replace_block(OwnerId owner, const ChangeTarget& target, size_t size, uintptr_t resume,
                                           AnvilCpuCallback callback, void* user, ChangeId* out) {
    if (callback == nullptr || size > 4096) return ChangeStatus::InvalidArgument;
    std::lock_guard lock(mutex_);
    if (check_owner(owner) != ChangeStatus::Ok) return ChangeStatus::InvalidArgument;
    const std::string label = "block replacement " + where(target.address) + " (" + std::to_string(size) + " B)";
    if (size < 5) {
        return refuse(owner, ChangeStatus::UnsafeSite, label + ": shorter than the hook's 5-byte jump");
    }
    if (const ChangeStatus s = check_target(owner, target, size, "block replacement"); s != ChangeStatus::Ok) return s;
    std::vector<uint8_t> block(size);
    if (!read_original_locked(target.address, block.data(), size)) return ChangeStatus::InvalidArgument;
    if (!whole_instructions(block.data(), size)) {
        return refuse(owner, ChangeStatus::UnsafeSite, label + ": does not end on an instruction boundary");
    }
    const uintptr_t end = target.address + size;
    if (resume == 0) resume = end;
    if (resume != end && resume >= target.address && resume < end) {
        return refuse(owner, ChangeStatus::InvalidArgument, label + ": resumes inside the block");
    }
    if (!executable(resume)) {
        return refuse(owner, ChangeStatus::InvalidArgument, label + ": resumes " + where(resume) + ", not code");
    }
    Site* site = nullptr;
    if (const ChangeStatus s = code_site(owner, target, ChangeKind::BlockReplacement, size, label, site);
        s != ChangeStatus::Ok) {
        return s;
    }
    return add_link(owner, *site, ChangeKind::BlockReplacement, reinterpret_cast<uintptr_t>(callback), {}, label,
                    nullptr, out, user, resume, size, target.symbol);
}

ChangeStatus ChangeRegistry::redirect_call(OwnerId owner, const ChangeTarget& target, uintptr_t expected_callee,
                                           void* new_target, void** next, ChangeId* out) {
    if (new_target == nullptr || next == nullptr) return ChangeStatus::InvalidArgument;
    std::lock_guard lock(mutex_);
    if (check_owner(owner) != ChangeStatus::Ok) return ChangeStatus::InvalidArgument;
    const std::string label = "call redirect " + where(target.address);
    if (const ChangeStatus s = check_target(owner, target, 5, "call redirect"); s != ChangeStatus::Ok) return s;
    Site* site = find_site(target.address, ChangeKind::CallRedirect);
    // Mid hooks at this call stay: they run first and pass on through their
    // call stub. Function hooks and block replacements here are a conflict.
    Site* code_site = find_site(target.address, ChangeKind::FunctionHook);
    const Site* partner = code_site;
    if (code_site == nullptr || code_site->call_stub == nullptr ||
        std::any_of(code_site->links.begin(), code_site->links.end(),
                    [](const Change* c) { return c->kind != ChangeKind::MidHook; })) {
        partner = nullptr;
    }
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
        if (const ChangeStatus s =
                check_overlap(owner, nullptr, target.address, 5, ChangeKind::CallRedirect, label, partner);
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
        if (code_site != nullptr && code_site->call_stub != nullptr) {
            set_call_stub(code_site->call_stub, reinterpret_cast<uintptr_t>(entry));
        }
    } else {
        if (expected_callee != 0 && site->terminal != expected_callee) {
            return refuse(owner, ChangeStatus::BytesMismatch,
                          label + ": calls " + where(site->terminal) + ", expected " + where(expected_callee));
        }
        if (const ChangeStatus s = check_overlap(owner, site, site->address, 5, site->kind, label, partner);
            s != ChangeStatus::Ok) {
            return s;
        }
    }
    return add_link(owner, *site, ChangeKind::CallRedirect, reinterpret_cast<uintptr_t>(new_target), {}, label, next,
                    out, nullptr, 0, 0, target.symbol);
}

ChangeStatus ChangeRegistry::patch_bytes(OwnerId owner, const ChangeTarget& target, const void* bytes, size_t size,
                                         ChangeId* out) {
    if (bytes == nullptr || size == 0 || size > 4096) return ChangeStatus::InvalidArgument;
    std::lock_guard lock(mutex_);
    if (check_owner(owner) != ChangeStatus::Ok) return ChangeStatus::InvalidArgument;
    const std::string label = "byte patch " + where(target.address) + " (" + std::to_string(size) + " B)";
    if (const ChangeStatus s = check_target(owner, target, size, "byte patch"); s != ChangeStatus::Ok) return s;
    const auto* data = static_cast<const uint8_t*>(bytes);
    // Identical byte patches share ownership (§8.3).
    Site* site = nullptr;
    for (const auto& s : sites_) {
        if (s->kind != ChangeKind::BytePatch || s->address != target.address || s->footprint != size) continue;
        if (live(*s) && std::memcmp(s->links.front()->bytes.get(), data, size) != 0) continue;  // reported below
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
    auto patch = std::make_unique_for_overwrite<uint8_t[]>(size);
    std::memcpy(patch.get(), data, size);
    return add_link(owner, *site, ChangeKind::BytePatch, 0, std::move(patch), label, nullptr, out);
}

ChangeStatus ChangeRegistry::swap_pointer(OwnerId owner, const ChangeTarget& target, uintptr_t expected_value,
                                          void* value, void** previous, ChangeId* out) {
    if (previous != nullptr && reinterpret_cast<uintptr_t>(previous) % alignof(void*) != 0) {
        return ChangeStatus::InvalidArgument;  // stored atomically like the slot
    }
    std::lock_guard lock(mutex_);
    if (check_owner(owner) != ChangeStatus::Ok) return ChangeStatus::InvalidArgument;
    const std::string label = "pointer swap " + where(target.address);
    if (const ChangeStatus s = check_target(owner, target, sizeof(uintptr_t), "pointer swap"); s != ChangeStatus::Ok) {
        return s;
    }
    if (target.address % sizeof(uintptr_t) != 0) {
        return refuse(owner, ChangeStatus::UnsafeSite, label + ": not 8-byte aligned, so not atomic");
    }
    Site* site = find_site(target.address, ChangeKind::PointerSwap);
    if (const ChangeStatus s =
            check_overlap(owner, site, target.address, sizeof(uintptr_t), ChangeKind::PointerSwap, label);
        s != ChangeStatus::Ok) {
        return s;
    }
    if (site != nullptr && site->links.empty()) {
        // Unused since its last link left: take the pointer as it is now.
        std::memcpy(site->original.data(), reinterpret_cast<const void*>(target.address), sizeof(uintptr_t));
        std::memcpy(&site->terminal, site->original.data(), sizeof(uintptr_t));
    }
    uintptr_t original = 0;
    if (site != nullptr) {
        original = site->terminal;
    } else if (!read_original_locked(target.address, &original, sizeof(original))) {
        return ChangeStatus::InvalidArgument;
    }
    if (expected_value != 0 && original != expected_value) {
        return refuse(owner, ChangeStatus::BytesMismatch,
                      label + ": holds " + where(original) + ", expected " + where(expected_value));
    }
    if (site == nullptr) {
        site = &add_site(target.address, ChangeKind::PointerSwap, sizeof(uintptr_t));
        site->original.resize(sizeof(uintptr_t));
        std::memcpy(site->original.data(), &original, sizeof(original));
        site->terminal = original;
    }
    return add_link(owner, *site, ChangeKind::PointerSwap, reinterpret_cast<uintptr_t>(value), {}, label, previous,
                    out);
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
            return add_link(owner, *site, ChangeKind::ImportReplacement, reinterpret_cast<uintptr_t>(replacement), {},
                            label, next, out);
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
    if (!relink(*c.site)) return refuse(owner, ChangeStatus::Internal, std::string(c.label()) + ": cannot restore the game's memory");
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
        undone += c.state == LinkState::Active || c.state == LinkState::Suspended ? 1 : 0;
        c.state = LinkState::Removed;
        std::erase(c.site->links, &c);
        relink(*c.site);
    }
    owners_[owner].group = false;
    owners_[owner].staged.clear();
    return undone;
}

// Moves the owner's enabled links between Active and Suspended, one site at a
// time. A site whose memory cannot be written gets its links back.
ChangeStatus ChangeRegistry::move_links(OwnerId owner, bool suspend) {
    const LinkState from = suspend ? LinkState::Active : LinkState::Suspended;
    const LinkState to = suspend ? LinkState::Suspended : LinkState::Active;
    std::vector<Site*> sites;
    for (const auto& c : changes_) {
        if (c->owner != owner || c->state != from) continue;
        if (std::find(sites.begin(), sites.end(), c->site) == sites.end()) sites.push_back(c->site);
    }
    ChangeStatus status = ChangeStatus::Ok;
    for (Site* site : sites) {
        std::vector<Change*> moved;
        for (Change* c : site->links) {
            if (c->owner != owner || c->state != from) continue;
            c->state = to;
            moved.push_back(c);
        }
        if (relink(*site)) continue;
        for (Change* c : moved) c->state = from;
        relink(*site);
        status = refuse(owner, ChangeStatus::Internal,
                        std::string(change_kind_name(site->kind)) + " " + where(site->address) +
                            ": cannot write the game's memory; not " + (suspend ? "suspended" : "resumed") + " there");
    }
    return status;
}

ChangeStatus ChangeRegistry::suspend(OwnerId owner, bool* changed) {
    std::lock_guard lock(mutex_);
    if (changed != nullptr) *changed = false;
    if (check_owner(owner) != ChangeStatus::Ok) return ChangeStatus::InvalidArgument;
    if (owners_[owner].suspended) return ChangeStatus::Ok;
    owners_[owner].suspended = true;
    if (changed != nullptr) *changed = true;
    return move_links(owner, true);
}

ChangeStatus ChangeRegistry::resume(OwnerId owner, bool* changed) {
    std::lock_guard lock(mutex_);
    if (changed != nullptr) *changed = false;
    if (check_owner(owner) != ChangeStatus::Ok) return ChangeStatus::InvalidArgument;
    if (!owners_[owner].suspended) return ChangeStatus::Ok;
    owners_[owner].suspended = false;
    if (changed != nullptr) *changed = true;
    return move_links(owner, false);
}

bool ChangeRegistry::suspended(OwnerId owner) const {
    std::lock_guard lock(mutex_);
    return owner < owners_.size() && owners_[owner].suspended;
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
        if (c->owner == owner && c->state == LinkState::Active) out.push_back(c->label());
    }
    return out;
}

size_t ChangeRegistry::count(OwnerId owner) const {
    std::lock_guard lock(mutex_);
    size_t n = 0;
    for (const auto& c : changes_) n += c->owner == owner && c->state == LinkState::Active ? 1 : 0;
    return n;
}

size_t ChangeRegistry::suspended_count(OwnerId owner) const {
    std::lock_guard lock(mutex_);
    size_t n = 0;
    for (const auto& c : changes_) n += c->owner == owner && c->state == LinkState::Suspended ? 1 : 0;
    return n;
}

size_t ChangeRegistry::staged_count(OwnerId owner) const {
    std::lock_guard lock(mutex_);
    return owner < owners_.size() ? owners_[owner].staged.size() : 0;
}

bool ChangeRegistry::call_counts(std::vector<CallCount>& out, bool wait) const {
    std::unique_lock lock(mutex_, std::defer_lock);
    if (wait) {
        lock.lock();
    } else if (!lock.try_lock()) {
        return false;
    }
    out.clear();
    for (const auto& c : changes_) {
        if (!c->counter) continue;
        const uint64_t calls = c->counter->calls.load(std::memory_order_relaxed);
        if (c->state == LinkState::Removed && calls == 0) continue;
        out.push_back({c->owner, c->kind, c->label(), c->symbol(), calls});
    }
    return true;
}

std::vector<OwnerId> ChangeRegistry::chain_at(uintptr_t address) const {
    std::lock_guard lock(mutex_);
    std::vector<OwnerId> out;
    const auto add = [&](const Site& s, bool callbacks) {
        for (const Change* c : s.links) {
            const bool callback = c->kind == ChangeKind::MidHook;
            if (c->state == LinkState::Active && callback == callbacks) out.push_back(c->owner);
        }
    };
    // In the order the game runs them: mid hooks, the rest of the code site,
    // then a redirect at the same call.
    for (const ChangeKind kind : {ChangeKind::FunctionHook, ChangeKind::CallRedirect, ChangeKind::BytePatch,
                                  ChangeKind::PointerSwap, ChangeKind::ImportReplacement}) {
        const Site* s = find_site(address, kind);
        if (kind == ChangeKind::BytePatch) {
            s = nullptr;
            for (const auto& p : sites_) {
                if (p->address == address && p->kind == kind && p->installed) s = p.get();
            }
        }
        if (s == nullptr || !s->installed) continue;
        add(*s, true);
        add(*s, false);
    }
    return out;
}

size_t ChangeRegistry::context_stub_return() { return kCpuStubReturn; }

bool ChangeRegistry::attribute(uintptr_t address, AddressAttribution& out) const {
    out = AddressAttribution{};
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    const auto inside = [address](const void* start, size_t size) {
        const auto begin = reinterpret_cast<uintptr_t>(start);
        return start != nullptr && address >= begin && address - begin < size;
    };
    const auto add = [&out](const Change& c) {
        if (out.count < AddressAttribution::kMaxChanges) {
            out.changes[out.count++] = {c.id, c.owner, c.kind, c.label(), c.symbol()};
        }
        ++out.total;
    };
    // The site's acting links; while none acts (staged, suspended), all of them.
    const auto add_site = [&](const Site& site) {
        out.site = site.address;
        const bool any_active =
            std::any_of(site.links.begin(), site.links.end(), [](const Change* c) { return c->state == LinkState::Active; });
        for (const Change* c : site.links) {
            if (!any_active || c->state == LinkState::Active) add(*c);
        }
    };
    // A link's own stubs belong to that link, even after its removal: a
    // thread may still pass through them.
    for (const auto& c : changes_) {
        if (inside(c->next, kStubSize) || inside(c->counting, kCountingStubSize)) {
            out.role = AddressRole::Stub;
            out.site = c->site != nullptr ? c->site->address : 0;
            add(*c);
            return true;
        }
    }
    for (const auto& site : sites_) {
        const Site& s = *site;
        if (inside(s.entry, kStubSize) || inside(s.call_stub, kCallStubSize) ||
            (s.cpu && inside(s.cpu->code, sizeof(kCpuStub)))) {
            out.role = AddressRole::Stub;
            out.context_return = s.cpu && address == reinterpret_cast<uintptr_t>(s.cpu->code) + kCpuStubReturn;
            add_site(s);
            return true;
        }
        if (s.hook) {
            const auto& trampoline = s.hook->trampoline();
            if (inside(reinterpret_cast<const void*>(trampoline.address()), trampoline.size())) {
                out.role = AddressRole::Trampoline;
                // The stolen instructions are copied one to one (SafetyHook
                // refuses what it would have to widen); past them is its jump back.
                const size_t offset = address - trampoline.address();
                if (offset < s.hook->original_bytes().size()) out.original = s.address + offset;
                add_site(s);
                return true;
            }
        }
    }
    for (const auto& site : sites_) {
        const Site& s = *site;
        if (!s.installed || s.links.empty() || !inside(reinterpret_cast<const void*>(s.address), extent(s))) continue;
        out.role = AddressRole::ChangedBytes;
        add_site(s);
        // Shared byte patches and a redirect inside a code site: keep looking
        // only to count the other owners, the role stays.
    }
    if (out.role != AddressRole::None) return true;
    for (const Arena& a : arenas_) {
        if (inside(a.base, a.size)) {
            out.role = AddressRole::StubMemory;
            return true;
        }
    }
    return true;
}

}  // namespace anvil
