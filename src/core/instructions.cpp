#include "instructions.hpp"

#include <Zydis/Zydis.h>

#include <cstdio>
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
        }
        const size_t end = offset + instruction.length;
        if (end > size) {
            return "instruction boundaries: the " + std::to_string(instruction.length) + "-byte instruction at " +
                   offset_text(offset, rva) + " ends past the declared " + std::to_string(size) +
                   " bytes; declare whole instructions (" + std::to_string(end) + " bytes)";
        }
        offset = end;
    }
    return {};
}

}  // namespace anvil
