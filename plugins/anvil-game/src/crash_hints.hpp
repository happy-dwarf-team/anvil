// anvil-game's note in Anvil's crash reports (anvil.control set_crash_note):
// it names the engine's own bugs of anvil#426 when a crash looks like one of
// them. Apart from the plugin so that tests run it without the game.
#pragma once

#include <cstddef>
#include <cstdint>

namespace anvil_game::crash_hints {

// Reads `size` bytes at `address` without faulting (ReadProcessMemory in the
// plugin); false when they cannot be read.
using Reader = bool (*)(std::uintptr_t address, void* out, std::size_t size);

// Where the game keeps its provinces, as anvil-game.world reads them.
struct ProvinceLayout {
    std::uintptr_t root = 0;           // the address of the game root's global; 0: unknown
    std::uint32_t provinces = 0;       // root: the vector of Province*, entry 0 included
    std::uint32_t provinces_end = 0;
    std::uint32_t pointer_shift = 0;   // its 8-byte entries
    std::uint32_t province_array = 0;  // root: the array of provinces, indexed by ID
    std::uint32_t province_size = 0;
};

// How many entries before the array or past the map's last province a fault
// still counts as a read of a province ID: the IDs a script or a save gives.
inline constexpr std::int64_t kReach = 65536;

// How much of the crashed thread's stack the note looks through, from rsp up.
inline constexpr std::size_t kStackBytes = 256 * 1024;

// The idea-loop hint needs a read below this address: a field of the null idea
// past the group's end (anvil#426 read 0x10).
inline constexpr std::uintptr_t kNullReach = 0x10000;

struct Crash {
    bool access_violation = false;
    bool write = false;           // the access violation wrote, rather than read or ran
    std::uintptr_t address = 0;   // the address it could not access
    std::uintptr_t rsp = 0;       // the crashed thread's rsp, 0 when unknown
    std::uintptr_t stack_high = 0;
};

// Where an address falls among the provinces.
struct ProvinceEntry {
    bool found = false;      // outside provinces 1 to count - 1, but within kReach entries of them
    std::int64_t id = 0;     // the province ID whose entry would hold the address
    std::uint64_t offset = 0;  // its offset into that entry
    std::int64_t count = 0;  // the entries of the map, entry 0 included
};

ProvinceEntry province_entry(std::uintptr_t array, std::int64_t count, std::uint32_t size, std::uintptr_t address);

// The note: lines of text, each ended by '\n', into `out`; returns the bytes
// written (0: nothing to say). `idea_return` is the return address of the
// call in CalcTechAndIdeaModifiers' idea loop, 0 when unknown; its hint needs
// that address on the stack and a read below kNullReach. It allocates
// nothing, takes no lock and reads only through `read`.
std::size_t write_note(const ProvinceLayout& layout, std::uintptr_t image_base, std::uintptr_t idea_return,
                       const Crash& crash, Reader read, char* out, std::size_t capacity);

}  // namespace anvil_game::crash_hints
