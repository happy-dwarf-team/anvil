// A small synthetic 64-bit PE image for the symbols and CLI tests: built in
// memory, then used mapped (MemoryImageReader) or as a file (FileImageReader,
// anvil check --exe).
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

// A 64-bit image of 0x4000 bytes:
//   .text  0x1000 code   speed_object 0x1000, root_and_date 0x1010, a far
//                        RIP-relative load 0x1020, Exported_fn 0x1040
//   .rdata 0x2000 read   pointer slot 0x2000 -> 0x1000, export table 0x2100
//   .data  0x3000 write  0x1000 bytes in memory, only 0x200 in the file
struct TestImage {
    static constexpr uint32_t kSize = 0x4000;
    static constexpr uint64_t kPreferredBase = 0x140000000ull;
    std::vector<uint8_t> mapped = std::vector<uint8_t>(kSize);

    void put(uint32_t rva, std::initializer_list<uint8_t> bytes) { std::memcpy(&mapped[rva], bytes.begin(), bytes.size()); }
    template <typename T> void put_value(uint32_t rva, T value) { std::memcpy(&mapped[rva], &value, sizeof(value)); }
    void put_text(uint32_t rva, const char* text) { std::memcpy(&mapped[rva], text, std::strlen(text) + 1); }

    struct Layout {
        const char* name;
        uint32_t rva, virtual_size, raw_size, file_offset, flags;
    };
    static constexpr Layout kSections[] = {
        {".text", 0x1000, 0x100, 0x200, 0x200, IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ},
        {".rdata", 0x2000, 0x200, 0x200, 0x400, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ},
        {".data", 0x3000, 0x1000, 0x200, 0x600, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE},
    };

    TestImage() {
        IMAGE_DOS_HEADER dos{};
        dos.e_magic = IMAGE_DOS_SIGNATURE;
        dos.e_lfanew = 0x80;
        put_value(0, dos);
        IMAGE_NT_HEADERS64 nt{};
        nt.Signature = IMAGE_NT_SIGNATURE;
        nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt.FileHeader.NumberOfSections = 3;
        nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt.OptionalHeader.ImageBase = kPreferredBase;
        nt.OptionalHeader.SectionAlignment = 0x1000;
        nt.OptionalHeader.FileAlignment = 0x200;
        nt.OptionalHeader.SizeOfImage = kSize;
        nt.OptionalHeader.SizeOfHeaders = 0x200;
        nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = {0x2100, 0x100};
        put_value(0x80, nt);
        uint32_t at = 0x80 + sizeof(IMAGE_NT_HEADERS64);
        for (const auto& l : kSections) {
            IMAGE_SECTION_HEADER s{};
            std::memcpy(s.Name, l.name, std::strlen(l.name));
            s.VirtualAddress = l.rva;
            s.Misc.VirtualSize = l.virtual_size;
            s.SizeOfRawData = l.raw_size;
            s.PointerToRawData = l.file_offset;
            s.Characteristics = l.flags;
            put_value(at, s);
            at += sizeof(s);
        }

        put(0x1000, {0x48, 0x8B, 0x81, 0x18, 0x04, 0x00, 0x00, 0xC3});
        // mov rax, [rip+0x1FF1] -> 0x1017 + 0x1FF1 = 0x3008; mov ebx, [rax+0x1DD0]
        put(0x1010, {0x48, 0x8B, 0x05, 0xF1, 0x1F, 0x00, 0x00, 0x8B, 0x98, 0xD0, 0x1D, 0x00, 0x00});
        // mov rax, [rip+0x10000000]: far outside the image
        put(0x1020, {0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x10});
        put(0x1040, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20});

        put_value<uint64_t>(0x2000, kPreferredBase + 0x1000);
        IMAGE_EXPORT_DIRECTORY exports{};
        exports.Name = 0x21A0;
        exports.Base = 1;
        exports.NumberOfFunctions = 2;
        exports.NumberOfNames = 2;
        exports.AddressOfFunctions = 0x2140;
        exports.AddressOfNames = 0x2150;
        exports.AddressOfNameOrdinals = 0x2160;
        put_value(0x2100, exports);
        put_value<uint32_t>(0x2140, 0x1040);
        put_value<uint32_t>(0x2144, 0x2190);  // inside the export directory: forwarded
        put_value<uint32_t>(0x2150, 0x2170);
        put_value<uint32_t>(0x2154, 0x2180);
        put_value<uint16_t>(0x2160, 0);
        put_value<uint16_t>(0x2162, 1);
        put_text(0x2170, "Exported_fn");
        put_text(0x2180, "Forwarded_fn");
        put_text(0x2190, "OTHER.fn");
        put_text(0x21A0, "test.exe");
    }

    std::vector<uint8_t> file() const {
        std::vector<uint8_t> out(0x800);
        std::memcpy(out.data(), mapped.data(), 0x200);
        for (const auto& l : kSections) std::memcpy(&out[l.file_offset], &mapped[l.rva], l.raw_size);
        return out;
    }
};
