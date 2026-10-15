#pragma once

#include <anvil/anvil_plugin.h>

#include <windows.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace anvil {

// All modifications of game memory go through one registry, which
//  - verifies expected bytes / call targets / import targets before writing,
//  - refuses a patch that overlaps a patch of another owner,
//  - remembers original bytes so an owner's patches can be rolled back.
// Addresses here are absolute; the host converts RVAs.
class PatchRegistry {
public:
    PatchRegistry(uintptr_t image_base, uint32_t image_size);
    ~PatchRegistry();
    PatchRegistry(const PatchRegistry&) = delete;
    PatchRegistry& operator=(const PatchRegistry&) = delete;

    AnvilResult patch_bytes(const std::string& owner, uintptr_t address, const void* expected, const void* replacement,
                            size_t size);
    AnvilResult redirect_call(const std::string& owner, uintptr_t call_address, uintptr_t expected_target,
                              void* new_target);
    AnvilResult hook_function(const std::string& owner, uintptr_t address, const void* expected_prologue,
                              size_t prologue_size, void* detour, void** original);
    AnvilResult replace_import(const std::string& owner, HMODULE image, const char* module, const char* function,
                               void* replacement, void** original);
    void* alloc_near(size_t size);

    // Undo every patch of `owner`, newest first. Returns the number undone.
    size_t rollback(const std::string& owner);
    std::vector<std::string> describe(const std::string& owner) const;
    size_t count(const std::string& owner) const;

private:
    struct Entry;
    bool overlaps_other_owner(const std::string& owner, uintptr_t address, size_t size, std::string& other) const;
    bool in_image(uintptr_t address, size_t size) const;
    void* alloc_near_locked(size_t size);

    uintptr_t image_base_;
    uint32_t image_size_;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Entry>> entries_;
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
