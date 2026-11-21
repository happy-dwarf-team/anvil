#include "instructions.hpp"

#include <Zydis/Zydis.h>

#include <cstdio>
#include <optional>
#include <vector>

namespace anvil {
namespace {

constexpr size_t kMaxInstruction = 15;  // the longest x86 instruction

std::string offset_text(size_t offset, uint32_t rva) {
    char text[64];
    std::snprintf(text, sizeof(text), "+0x%zX (rva 0x%llX)", offset, static_cast<unsigned long long>(rva) + offset);
    return text;
}

// Instructions SafetyHook mis-encodes when it relocates them: their rel8 has
// no rel32 form.
const char* unrelocatable(ZydisMnemonic mnemonic) {
    switch (mnemonic) {
    case ZYDIS_MNEMONIC_JRCXZ: return "jrcxz";
    case ZYDIS_MNEMONIC_JECXZ: return "jecxz";
    case ZYDIS_MNEMONIC_LOOP: return "loop";
    case ZYDIS_MNEMONIC_LOOPE: return "loope";
    case ZYDIS_MNEMONIC_LOOPNE: return "loopne";
    default: return nullptr;
    }
}

// A short (rel8) jcc or jmp: SafetyHook's E9 trampoline holds it as a near one.
bool widened_branch(const ZydisDecodedInstruction& instruction) {
    return instruction.meta.branch_type == ZYDIS_BRANCH_TYPE_SHORT &&
           (instruction.meta.category == ZYDIS_CATEGORY_COND_BR ||
            instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR);
}

}  // namespace

bool has_instruction_boundaries(const Symbol& symbol, MemoryKind section_kind) {
    return section_kind == MemoryKind::Code &&
           (symbol.kind == SymbolKind::Function || symbol.kind == SymbolKind::Site) &&
           symbol.bytes.size() >= kHookJumpSize;
}

std::string check_instruction_boundaries(const Symbol& symbol, uint32_t rva, const ImageReader& reader) {
    const size_t size = symbol.bytes.size();
    // The declared bytes and as many bytes after them as the image has (up to
    // one instruction), so an instruction that runs past the end still decodes.
    std::vector<uint8_t> code(size + kMaxInstruction);
    if (!reader.read(rva, code.data(), size)) return "instruction boundaries: the bytes cannot be read";
    size_t available = size;
    for (size_t extra = kMaxInstruction; extra > 0; --extra) {
        if (reader.read(rva + static_cast<uint32_t>(size), code.data() + size, extra)) {
            available += extra;
            break;
        }
    }

    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    size_t offset = 0;
    size_t stolen = 0;                             // the end of the stolen bytes, once known
    std::optional<size_t> short_branch;            // a stolen short branch, while it is the last one seen
    while (offset < size) {
        ZydisDecodedInstruction instruction;
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, nullptr, code.data() + offset, available - offset,
                                                        &instruction))) {
            return "instruction boundaries: no valid x86-64 instruction at " + offset_text(offset, rva);
        }
        if (offset < kHookJumpSize) {
            if (const char* name = unrelocatable(instruction.mnemonic)) {
                return "instruction boundaries: " + std::string(name) + " at " + offset_text(offset, rva) +
                       " lies in the first " + std::to_string(kHookJumpSize) +
                       " bytes, which a hook relocates, and cannot be relocated";
            }
            // SafetyHook widens a short branch in its copy, so the instructions
            // after it sit further on there; a thread it moves between the code
            // and the copy past such a branch lands inside an instruction.
            if (short_branch) {
                return "instruction boundaries: the short branch at " + offset_text(*short_branch, rva) +
                       " is not the last of the instructions in the first " + std::to_string(kHookJumpSize) +
                       " bytes, which a hook relocates; hook the code at or after it instead";
            }
            if (widened_branch(instruction)) short_branch = offset;
        }
        const size_t end = offset + instruction.length;
        if (end > size) {
            return "instruction boundaries: the " + std::to_string(instruction.length) + "-byte instruction at " +
                   offset_text(offset, rva) + " ends past the declared " + std::to_string(size) +
                   " bytes; declare whole instructions (" + std::to_string(end) + " bytes)";
        }
        if (offset < kHookJumpSize) stolen = end;
        offset = end;
    }
    // A hook writes over the stolen bytes: at least one of them must be fixed,
    // so the jump never lands on code that only "??" bytes matched.
    if (!symbol.mask.empty()) {
        bool fixed = false;
        for (size_t i = 0; i < stolen && i < symbol.mask.size(); ++i) fixed |= symbol.mask[i] == 0xFF;
        if (!fixed) {
            return "instruction boundaries: the " + std::to_string(stolen) +
                   " bytes a hook overwrites are all \"??\"; at least one of them must be a fixed byte";
        }
    }
    return {};
}

}  // namespace anvil
