// A plugin's symbols for a test's synthetic image (ADR 0003). The team's
// symbols.toml files verify each symbol by `sha256` and `size`, and the
// repository holds none of eu4.exe's code, so a test cannot place the game's
// bytes at a symbol's RVA. Instead the test lays out code of its own where it
// runs or hooks the game's functions, and synthetic_plugin():
//
//   1. fills each code symbol the test left empty (all zero) with NOPs, so a
//      hook can relocate its first instructions and a call runs through it;
//   2. writes every `values` entry at its offset: `is` when the file gives it
//      (the constant `anvil gen` compiled into the plugin), otherwise the
//      value the test passes for "<symbol>.<value>"; in a filled code symbol a
//      value of up to four bytes rides in the displacement of a 7-byte NOP
//      (0F 1F 80 disp32), or of `mov eax, imm32` when it lies too close to
//      the start, and an eight-byte one in `mov rax, imm64`, so the bytes
//      still decode as whole instructions;
//   3. digests each symbol's bytes as they now are in the image, with the
//      `any` offsets zeroed;
//   4. writes a copy of the plugin's folder whose symbols.toml declares the
//      one build with those digests, and returns its path.
//
// The plugin's code is the same DLL: only the digests differ from its real
// symbols.toml, and its symbol IDs, constants and signatures are those of the
// real file. A symbol the test wants to run is laid out by the test outside
// its declared bytes (a jump just after them, for example), because the
// declared bytes are what this helper owns.
#pragma once

#include "core/game_build.hpp"
#include "core/manifest.hpp"
#include "core/pe_image.hpp"
#include "core/symbols.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace synthetic {

inline std::string hex(uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof text, "0x%llX", static_cast<unsigned long long>(value));
    return text;
}

inline std::string quoted(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

// Writes `size` bytes at base + rva, making the pages writable for it.
inline void write(uint8_t* base, uint32_t rva, const void* data, size_t size) {
    for (size_t page = rva & ~size_t{0xFFF}; page < rva + size; page += 0x1000) {
        MEMORY_BASIC_INFORMATION info{};
        VirtualQuery(base + page, &info, sizeof info);
        if (info.State != MEM_COMMIT) VirtualAlloc(base + page, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    }
    DWORD old = 0;
    VirtualProtect(base + rva, size, PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(base + rva, data, size);
    DWORD ignored = 0;
    VirtualProtect(base + rva, size, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), base + rva, size);
}

inline bool readable(const uint8_t* address, size_t size) {
    for (const uint8_t* at = address; at < address + size;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(at, &info, sizeof info) != sizeof info || info.State != MEM_COMMIT) return false;
        at = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

// The value a test gives a symbol's `values` entry that has no `is`:
// "<symbol>.<value>" -> the value (an RVA for a rip_relative one).
using Values = std::map<std::string, uint64_t>;

// Lays out `plugin_dir`'s symbols for `build` in the image at `base` (see
// above) and returns the folder of the copy; empty, with the reason printed,
// when the file does not parse or a symbol cannot be placed.
inline std::filesystem::path synthetic_plugin(const std::filesystem::path& plugin_dir, uint8_t* base,
                                              uint32_t image_size, const char* build, const Values& values = {}) {
    namespace fs = std::filesystem;
    const auto fail = [&](const std::string& why) {
        std::printf("  synthetic symbols of %s: %s\n", plugin_dir.filename().string().c_str(), why.c_str());
        return fs::path();
    };
    const auto manifest = anvil::load_manifest(plugin_dir);
    if (!manifest.manifest) return fail(manifest.error);
    const fs::path symbols_name = manifest.manifest->symbols;
    anvil::SymbolFileResult parsed = anvil::load_symbols(plugin_dir / symbols_name);
    if (!parsed.file) return fail(parsed.error);
    anvil::SymbolFile& file = *parsed.file;
    if (!anvil::select_build(file, build)) return fail(std::string("no build ") + build);

    const anvil::MemoryImageReader reader(reinterpret_cast<uintptr_t>(base), image_size);
    std::string error;
    const auto image = anvil::PeImage::parse(reader, error);
    if (!image) return fail(error);

    struct Placed {
        uint32_t rva = 0;
        uint32_t size = 0;
        bool code = false;
        bool filled = false;
    };
    std::vector<Placed> placed(file.symbols.size());
    for (size_t i = 0; i < file.symbols.size(); ++i) {
        const anvil::Symbol& s = file.symbols[i];
        if (s.absent) return fail(s.name + " is absent on " + build + ", which this helper does not write");
        Placed& p = placed[i];
        if (!s.export_name.empty()) {
            const auto rva = image->find_export(s.export_name, error);
            if (!rva) return fail(s.name + ": export " + s.export_name + " not in the image");
            p.rva = *rva;
        } else if (s.rva) {
            p.rva = *s.rva;
        } else {
            return fail(s.name + " has neither rva nor export");
        }
        p.size = static_cast<uint32_t>(s.mask.size());
        const anvil::Section* section = image->section_of(p.rva, std::max<uint32_t>(p.size, 1));
        p.code = section != nullptr && section->kind == anvil::MemoryKind::Code;
    }

    // 1. NOPs in each empty code symbol.
    for (size_t i = 0; i < file.symbols.size(); ++i) {
        Placed& p = placed[i];
        if (!p.code || p.size == 0) continue;
        if (readable(base + p.rva, p.size)) {
            bool empty = true;
            for (uint32_t k = 0; k < p.size && empty; ++k) empty = base[p.rva + k] == 0;
            if (!empty) continue;
        }
        const std::vector<uint8_t> nops(p.size, 0x90);
        write(base, p.rva, nops.data(), nops.size());
        p.filled = true;
    }

    // 2. The values, then what carries them in a filled code symbol.
    for (size_t i = 0; i < file.symbols.size(); ++i) {
        const anvil::Symbol& s = file.symbols[i];
        const Placed& p = placed[i];
        std::vector<bool> taken(p.size, false);
        for (const anvil::SymbolValue& v : s.values) {
            const uint32_t width = v.rip_relative > 0 ? 4 : v.size;
            for (uint32_t k = v.at; k < v.at + width && k < p.size; ++k) taken[k] = true;
        }
        for (const anvil::SymbolValue& v : s.values) {
            uint64_t value = 0;
            if (v.is) {
                value = *v.is;
            } else if (const auto given = values.find(s.name + "." + v.name); given != values.end()) {
                value = given->second;
            } else {
                continue;  // the bytes the test put there give it
            }
            const uint32_t width = v.rip_relative > 0 ? 4 : v.size;
            if (v.rip_relative > 0) {
                const int64_t disp = static_cast<int64_t>(value) - (static_cast<int64_t>(p.rva) + v.rip_relative);
                const int32_t d32 = static_cast<int32_t>(disp);
                write(base, p.rva + v.at, &d32, 4);
            } else {
                write(base, p.rva + v.at, &value, width);
            }
            if (!p.filled) continue;
            const auto free_before = [&](uint32_t n) {
                if (v.at < n) return false;
                for (uint32_t k = v.at - n; k < v.at; ++k) {
                    if (taken[k] || base[p.rva + k] != 0x90) return false;
                }
                return true;
            };
            if (width == 8 && free_before(2)) {
                const uint8_t mov_rax[] = {0x48, 0xB8};
                write(base, p.rva + v.at - 2, mov_rax, 2);
            } else if (width <= 4 && v.at + 4 <= p.size && free_before(3)) {
                const uint8_t nop7[] = {0x0F, 0x1F, 0x80};
                write(base, p.rva + v.at - 3, nop7, 3);
                for (uint32_t k = v.at; k < v.at + 4; ++k) taken[k] = true;
            } else if (width <= 4 && v.at + 4 <= p.size && free_before(1)) {
                const uint8_t mov_eax = 0xB8;
                write(base, p.rva + v.at - 1, &mov_eax, 1);
                for (uint32_t k = v.at; k < v.at + 4; ++k) taken[k] = true;
            }
        }
    }

    // 3 and 4. The digests, and the copy of the folder.
    std::ostringstream out;
    out << "# Written by tests/synthetic_symbols.hpp for a test image: the digests are\n"
           "# of the test's own bytes, not of eu4.exe.\n"
        << "build = " << quoted(build) << "\n";
    for (size_t i = 0; i < file.symbols.size(); ++i) {
        const anvil::Symbol& s = file.symbols[i];
        const Placed& p = placed[i];
        out << "\n[symbol." << s.name << "]\n";
        if (!s.export_name.empty()) {
            out << "export = " << quoted(s.export_name) << "\n";
        } else {
            out << "rva = " << hex(p.rva) << "\n";
        }
        if (p.size > 0) {
            if (!readable(base + p.rva, p.size)) return fail(s.name + ": its bytes are not in the image");
            std::vector<uint8_t> bytes(base + p.rva, base + p.rva + p.size);
            std::string any;
            for (uint32_t k = 0; k < p.size; ++k) {
                if (s.mask[k] != 0) continue;
                bytes[k] = 0;
                any += (any.empty() ? "" : ", ") + std::to_string(k);
            }
            out << "sha256 = " << quoted(anvil::sha256_bytes(bytes.data(), bytes.size())) << "\n"
                << "size = " << p.size << "\n";
            if (!any.empty()) out << "any = [" << any << "]\n";
        }
        if (s.points_to) out << "points_to = " << hex(*s.points_to) << "\n";
        if (s.memory) out << "memory = " << quoted(anvil::memory_kind_name(*s.memory)) << "\n";
        if (!s.type.empty()) out << "type = " << quoted(s.type) << "\n";
        if (!s.signature.empty()) out << "signature = " << quoted(s.signature) << "\n";
        if (!s.resume.empty()) out << "resume = " << quoted(s.resume) << "\n";
        for (const anvil::SymbolValue& v : s.values) {
            out << "values." << v.name << " = { at = " << v.at;
            if (v.rip_relative > 0) {
                out << ", rip_relative = " << v.rip_relative;
            } else {
                out << ", size = " << v.size;
            }
            if (!v.type.empty()) out << ", type = " << quoted(v.type);
            if (v.is) out << ", is = " << hex(*v.is);
            out << " }\n";
        }
    }

    static int copies = 0;
    const fs::path root = fs::temp_directory_path() /
                          ("anvil_synthetic_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(++copies));
    const fs::path copy = root / plugin_dir.filename();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    fs::copy(plugin_dir, copy, fs::copy_options::recursive, ec);
    if (ec) return fail("cannot copy the folder: " + ec.message());
    std::ofstream(copy / symbols_name, std::ios::binary | std::ios::trunc) << out.str();
    return copy;
}

}  // namespace synthetic
