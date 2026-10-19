#pragma once

#include "pe_image.hpp"
#include "symbols.hpp"

#include <cstdint>
#include <string>

namespace anvil {

// The bytes a hook overwrites at least: a 5-byte `jmp rel32`. SafetyHook
// relocates the whole instructions that cover them (the stolen bytes).
inline constexpr uint32_t kHookJumpSize = 5;

// Whether the instruction-boundary rule (design §16) applies to a symbol: a
// function or a site in code with at least kHookJumpSize declared bytes. Data
// and pointer slots are not code; shorter sites can only take a byte patch.
bool has_instruction_boundaries(const Symbol& symbol, MemoryKind section_kind);

// Decodes the symbol's declared bytes at `rva` as x86-64 code (Zydis, read
// through `reader`, so "??" bytes take the image's values) and checks that
//   - every byte belongs to a valid instruction and the last instruction ends
//     exactly at the end of the declared bytes: a block replacement owns whole
//     instructions, and a hook's stolen bytes lie inside what was verified;
//   - the stolen bytes (the instructions that start in the first
//     kHookJumpSize bytes) contain no JRCXZ, JECXZ or LOOP/LOOPE/LOOPNE,
//     which SafetyHook cannot relocate (design §8.3).
// Returns the first problem, or an empty string.
std::string check_instruction_boundaries(const Symbol& symbol, uint32_t rva, const ImageReader& reader);

}  // namespace anvil
