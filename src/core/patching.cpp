#include "patching.hpp"

#include <safetyhook.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace anvil {

struct PatchRegistry::Entry {
    enum class Kind { Bytes, Call, Hook, Import } kind;
    std::string owner;
    uintptr_t address = 0;
    std::vector<uint8_t> original;  // bytes to restore (not used for Hook)
    std::unique_ptr<safetyhook::InlineHook> hook;
    std::string what;
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

PatchRegistry::PatchRegistry(uintptr_t image_base, uint32_t image_size)
    : image_base_(image_base), image_size_(image_size) {}

PatchRegistry::~PatchRegistry() = default;  // patches stay applied for the process lifetime

bool PatchRegistry::in_image(uintptr_t address, size_t size) const {
    return address >= image_base_ && size <= image_size_ && address - image_base_ <= image_size_ - size;
}

bool PatchRegistry::overlaps_other_owner(const std::string& owner, uintptr_t address, size_t size,
                                         std::string& other) const {
    for (const auto& e : entries_) {
        const size_t length = e->kind == Entry::Kind::Hook ? e->hook->original_bytes().size() : e->original.size();
        if (address < e->address + length && e->address < address + size) {
            if (e->owner != owner) {
                other = e->owner;
                return true;
            }
        }
    }
    return false;
}

static bool readable(uintptr_t address, size_t size) {
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

static std::string hex(uintptr_t value) {
    std::ostringstream s;
    s << "0x" << std::hex << std::uppercase << value;
    return s.str();
}

AnvilResult PatchRegistry::patch_bytes(const std::string& owner, uintptr_t address, const void* expected,
                                       const void* replacement, size_t size) {
    if (expected == nullptr || replacement == nullptr || size == 0 || size > 4096) return ANVIL_ERROR_INVALID_ARGUMENT;
    std::lock_guard lock(mutex_);
    if (!in_image(address, size)) return ANVIL_ERROR_INVALID_ARGUMENT;
    std::string other;
    if (overlaps_other_owner(owner, address, size, other)) return ANVIL_ERROR_CONFLICT;
    if (std::memcmp(reinterpret_cast<const void*>(address), expected, size) != 0) return ANVIL_ERROR_BYTES_MISMATCH;
    auto entry = std::make_unique<Entry>();
    entry->kind = Entry::Kind::Bytes;
    entry->owner = owner;
    entry->address = address;
    entry->original.assign(static_cast<const uint8_t*>(expected), static_cast<const uint8_t*>(expected) + size);
    entry->what = "bytes rva " + hex(address - image_base_) + " (" + std::to_string(size) + " B)";
    if (!write_memory(reinterpret_cast<void*>(address), replacement, size)) return ANVIL_ERROR_INTERNAL;
    entries_.push_back(std::move(entry));
    return ANVIL_OK;
}

AnvilResult PatchRegistry::expect_bytes(const std::string& owner, uintptr_t address, const void* expected,
                                        const void* mask, size_t size, std::string* mismatch) const {
    if (expected == nullptr || size == 0 || size > ANVIL_MAX_EXPECT_BYTES) return ANVIL_ERROR_INVALID_ARGUMENT;
    std::lock_guard lock(mutex_);
    if (!in_image(address, size) || !readable(address, size)) return ANVIL_ERROR_INVALID_ARGUMENT;
    for (const auto& e : entries_) {
        if (e->owner == owner) return ANVIL_ERROR_WRONG_PHASE;  // dependencies are verified before patching
    }
    const auto* live = reinterpret_cast<const uint8_t*>(address);
    std::vector<uint8_t> actual(live, live + size);
    // Compare with the game's own bytes: undo other owners' patches in the copy.
    for (const auto& e : entries_) {
        const auto& original = e->kind == Entry::Kind::Hook ? e->hook->original_bytes() : e->original;
        const uintptr_t begin = std::max(address, e->address);
        const uintptr_t end = std::min(address + size, e->address + original.size());
        for (uintptr_t a = begin; a < end; ++a) actual[a - address] = original[a - e->address];
    }
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
        return ANVIL_ERROR_BYTES_MISMATCH;
    }
    return ANVIL_OK;
}

void* PatchRegistry::alloc_near_locked(size_t size) {
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

void* PatchRegistry::alloc_near(size_t size) {
    std::lock_guard lock(mutex_);
    return alloc_near_locked(size);
}

AnvilResult PatchRegistry::redirect_call(const std::string& owner, uintptr_t call_address, uintptr_t expected_target,
                                         void* new_target) {
    if (new_target == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    std::lock_guard lock(mutex_);
    if (!in_image(call_address, 5)) return ANVIL_ERROR_INVALID_ARGUMENT;
    std::string other;
    if (overlaps_other_owner(owner, call_address, 5, other)) return ANVIL_ERROR_CONFLICT;
    const auto* code = reinterpret_cast<const uint8_t*>(call_address);
    int32_t rel = 0;
    std::memcpy(&rel, code + 1, 4);
    if (code[0] != 0xE8 || call_address + 5 + static_cast<intptr_t>(rel) != expected_target) {
        return ANVIL_ERROR_BYTES_MISMATCH;
    }
    uintptr_t target = reinterpret_cast<uintptr_t>(new_target);
    const auto reach = [&](uintptr_t t) {
        const intptr_t delta = static_cast<intptr_t>(t) - static_cast<intptr_t>(call_address + 5);
        return delta >= INT32_MIN && delta <= INT32_MAX;
    };
    if (!reach(target)) {
        // jmp qword ptr [rip+0]; dq new_target
        auto* stub = static_cast<uint8_t*>(alloc_near_locked(14));
        if (stub == nullptr) return ANVIL_ERROR_OUT_OF_MEMORY;
        const uint8_t jump[6] = {0xFF, 0x25, 0, 0, 0, 0};
        std::memcpy(stub, jump, sizeof(jump));
        std::memcpy(stub + 6, &target, sizeof(target));
        FlushInstructionCache(GetCurrentProcess(), stub, 14);
        target = reinterpret_cast<uintptr_t>(stub);
        if (!reach(target)) return ANVIL_ERROR_OUT_OF_MEMORY;
    }
    const int32_t newRel = static_cast<int32_t>(static_cast<intptr_t>(target) - static_cast<intptr_t>(call_address + 5));
    auto entry = std::make_unique<Entry>();
    entry->kind = Entry::Kind::Call;
    entry->owner = owner;
    entry->address = call_address;
    entry->original.assign(code, code + 5);
    entry->what = "call rva " + hex(call_address - image_base_) + " -> plugin";
    uint8_t patched[5] = {0xE8, 0, 0, 0, 0};
    std::memcpy(patched + 1, &newRel, 4);
    if (!write_memory(reinterpret_cast<void*>(call_address), patched, 5)) return ANVIL_ERROR_INTERNAL;
    entries_.push_back(std::move(entry));
    return ANVIL_OK;
}

AnvilResult PatchRegistry::hook_function(const std::string& owner, uintptr_t address, const void* expected_prologue,
                                         size_t prologue_size, void* detour, void** original) {
    if (expected_prologue == nullptr || prologue_size == 0 || prologue_size > 64 || detour == nullptr ||
        original == nullptr) {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard lock(mutex_);
    if (!in_image(address, prologue_size)) return ANVIL_ERROR_INVALID_ARGUMENT;
    std::string other;
    if (overlaps_other_owner(owner, address, prologue_size < 14 ? 14 : prologue_size, other)) return ANVIL_ERROR_CONFLICT;
    if (std::memcmp(reinterpret_cast<const void*>(address), expected_prologue, prologue_size) != 0) {
        return ANVIL_ERROR_BYTES_MISMATCH;
    }
    auto created = safetyhook::InlineHook::create(reinterpret_cast<void*>(address), detour);
    if (!created) return ANVIL_ERROR_INTERNAL;
    auto entry = std::make_unique<Entry>();
    entry->kind = Entry::Kind::Hook;
    entry->owner = owner;
    entry->address = address;
    entry->hook = std::make_unique<safetyhook::InlineHook>(std::move(*created));
    entry->what = "hook rva " + hex(address - image_base_);
    *original = entry->hook->original<void*>();
    entries_.push_back(std::move(entry));
    return ANVIL_OK;
}

AnvilResult PatchRegistry::replace_import(const std::string& owner, HMODULE image, const char* module,
                                          const char* function, void* replacement, void** original) {
    if (image == nullptr || module == nullptr || function == nullptr || replacement == nullptr) {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    auto* base = reinterpret_cast<uint8_t*>(image);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) return ANVIL_ERROR_NOT_FOUND;
    std::lock_guard lock(mutex_);
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
            std::string other;
            if (overlaps_other_owner(owner, slot, sizeof(uintptr_t), other)) return ANVIL_ERROR_CONFLICT;
            auto entry = std::make_unique<Entry>();
            entry->kind = Entry::Kind::Import;
            entry->owner = owner;
            entry->address = slot;
            const auto* current = reinterpret_cast<const uint8_t*>(slot);
            entry->original.assign(current, current + sizeof(uintptr_t));
            entry->what = std::string("import ") + module + "!" + function;
            void* previous = reinterpret_cast<void*>(slots->u1.Function);
            const uintptr_t value = reinterpret_cast<uintptr_t>(replacement);
            if (!write_memory(reinterpret_cast<void*>(slot), &value, sizeof(value))) return ANVIL_ERROR_INTERNAL;
            if (original != nullptr) *original = previous;
            entries_.push_back(std::move(entry));
            return ANVIL_OK;
        }
    }
    return ANVIL_ERROR_NOT_FOUND;
}

size_t PatchRegistry::rollback(const std::string& owner) {
    std::lock_guard lock(mutex_);
    size_t undone = 0;
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        Entry& e = **it;
        if (e.owner != owner) continue;
        if (e.kind == Entry::Kind::Hook) {
            e.hook->reset();
        } else {
            write_memory(reinterpret_cast<void*>(e.address), e.original.data(), e.original.size());
        }
        ++undone;
    }
    std::erase_if(entries_, [&](const std::unique_ptr<Entry>& e) { return e->owner == owner; });
    return undone;
}

std::vector<std::string> PatchRegistry::describe(const std::string& owner) const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    for (const auto& e : entries_) {
        if (e->owner == owner) out.push_back(e->what);
    }
    return out;
}

size_t PatchRegistry::count(const std::string& owner) const {
    std::lock_guard lock(mutex_);
    size_t n = 0;
    for (const auto& e : entries_) n += e->owner == owner ? 1 : 0;
    return n;
}

}  // namespace anvil
