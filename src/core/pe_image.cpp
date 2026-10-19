#include "pe_image.hpp"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>

namespace anvil {
namespace {

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

// [offset, offset + size) lies inside a buffer of `total` bytes.
bool fits(uint64_t offset, uint64_t size, uint64_t total) { return size <= total && offset <= total - size; }

uint32_t align_up(uint32_t value, uint32_t alignment) {
    if (alignment == 0) return value;
    const uint64_t aligned = (static_cast<uint64_t>(value) + alignment - 1) / alignment * alignment;
    return aligned > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(aligned);
}

MemoryKind kind_of(uint32_t characteristics) {
    if (characteristics & IMAGE_SCN_MEM_EXECUTE) return MemoryKind::Code;
    if (characteristics & IMAGE_SCN_MEM_WRITE) return MemoryKind::Write;
    return MemoryKind::Read;
}

}  // namespace

const char* memory_kind_name(MemoryKind kind) {
    switch (kind) {
    case MemoryKind::Code: return "code";
    case MemoryKind::Read: return "read";
    case MemoryKind::Write: return "write";
    }
    return "?";
}

bool MemoryImageReader::read(uint32_t rva, void* out, size_t size) const {
    if (!fits(rva, size, size_)) return false;
    if (size == 0) return true;
    if (!readable(base_ + rva, size)) return false;
    std::memcpy(out, reinterpret_cast<const void*>(base_ + rva), size);
    return true;
}

std::optional<FileImageReader> FileImageReader::from_bytes(std::vector<uint8_t> file, std::string& error) {
    error = "not a 64-bit PE file";
    IMAGE_DOS_HEADER dos{};
    if (file.size() < sizeof(dos)) return std::nullopt;
    std::memcpy(&dos, file.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) return std::nullopt;
    IMAGE_NT_HEADERS64 nt{};
    if (!fits(static_cast<uint32_t>(dos.e_lfanew), sizeof(nt), file.size())) return std::nullopt;
    std::memcpy(&nt, file.data() + dos.e_lfanew, sizeof(nt));
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return std::nullopt;

    const uint64_t table = static_cast<uint64_t>(dos.e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                           nt.FileHeader.SizeOfOptionalHeader;
    const uint16_t count = nt.FileHeader.NumberOfSections;
    if (!fits(table, static_cast<uint64_t>(count) * sizeof(IMAGE_SECTION_HEADER), file.size())) {
        error = "section table outside the file";
        return std::nullopt;
    }
    FileImageReader reader;
    reader.image_base_ = nt.OptionalHeader.ImageBase;
    reader.size_of_image_ = nt.OptionalHeader.SizeOfImage;
    const uint32_t alignment = nt.OptionalHeader.SectionAlignment;
    const uint32_t headers = nt.OptionalHeader.SizeOfHeaders;
    reader.ranges_.push_back({0, align_up(headers, alignment), 0,
                              static_cast<uint32_t>(std::min<uint64_t>(headers, file.size()))});
    for (uint16_t i = 0; i < count; ++i) {
        IMAGE_SECTION_HEADER s{};
        std::memcpy(&s, file.data() + table + i * sizeof(s), sizeof(s));
        const uint32_t size = s.Misc.VirtualSize != 0 ? s.Misc.VirtualSize : s.SizeOfRawData;
        const uint32_t raw = std::min<uint32_t>(s.SizeOfRawData, size);
        if (raw != 0 && !fits(s.PointerToRawData, raw, file.size())) {
            error = "section " + std::to_string(i) + " lies past the end of the file";
            return std::nullopt;
        }
        reader.ranges_.push_back({s.VirtualAddress, align_up(size, alignment), s.PointerToRawData, raw});
    }
    reader.file_ = std::move(file);
    error.clear();
    return reader;
}

std::optional<FileImageReader> FileImageReader::load(const std::filesystem::path& file, std::string& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "cannot open the file";
        return std::nullopt;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return from_bytes(std::move(bytes), error);
}

bool FileImageReader::read(uint32_t rva, void* out, size_t size) const {
    if (!fits(rva, size, size_of_image_)) return false;
    auto* dst = static_cast<uint8_t*>(out);
    uint64_t at = rva;
    const uint64_t end = static_cast<uint64_t>(rva) + size;
    while (at < end) {
        const Range* range = nullptr;
        for (const auto& r : ranges_) {
            if (at >= r.rva && at < static_cast<uint64_t>(r.rva) + r.size) {
                range = &r;
                break;
            }
        }
        if (range == nullptr) return false;  // not mapped
        const uint64_t chunk = std::min(end, static_cast<uint64_t>(range->rva) + range->size) - at;
        for (uint64_t i = 0; i < chunk; ++i) {
            const uint64_t offset = at + i - range->rva;
            *dst++ = offset < range->raw_size ? file_[range->offset + offset] : 0;
        }
        at += chunk;
    }
    return true;
}

std::optional<PeImage> PeImage::parse(const ImageReader& reader, std::string& error) {
    error = "not a 64-bit PE image";
    IMAGE_DOS_HEADER dos{};
    if (!reader.read(0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
        return std::nullopt;
    }
    IMAGE_NT_HEADERS64 nt{};
    const auto nt_rva = static_cast<uint32_t>(dos.e_lfanew);
    if (!reader.read(nt_rva, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return std::nullopt;
    }
    PeImage image(reader);
    image.size_of_image_ = nt.OptionalHeader.SizeOfImage;
    if (nt.OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT) {
        image.export_rva_ = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        image.export_size_ = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
    }
    const uint64_t table =
        static_cast<uint64_t>(nt_rva) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    std::vector<IMAGE_SECTION_HEADER> headers(nt.FileHeader.NumberOfSections);
    if (table > 0xFFFFFFFFull ||
        !reader.read(static_cast<uint32_t>(table), headers.data(), headers.size() * sizeof(IMAGE_SECTION_HEADER))) {
        error = "cannot read the section table";
        return std::nullopt;
    }
    for (const auto& h : headers) {
        Section s;
        s.name.assign(reinterpret_cast<const char*>(h.Name), strnlen(reinterpret_cast<const char*>(h.Name), 8));
        s.rva = h.VirtualAddress;
        s.size = h.Misc.VirtualSize != 0 ? h.Misc.VirtualSize : h.SizeOfRawData;
        s.characteristics = h.Characteristics;
        s.kind = kind_of(h.Characteristics);
        image.sections_.push_back(std::move(s));
    }
    error.clear();
    return image;
}

const Section* PeImage::section_of(uint32_t rva, uint32_t size) const {
    if (size == 0) size = 1;
    for (const auto& s : sections_) {
        if (rva >= s.rva && fits(rva - s.rva, size, s.size)) return &s;
    }
    return nullptr;
}

std::optional<uint32_t> PeImage::find_export(std::string_view name, std::string& error) const {
    error.clear();
    if (export_rva_ == 0 || name.empty()) return std::nullopt;
    IMAGE_EXPORT_DIRECTORY dir{};
    if (!reader_->read(export_rva_, &dir, sizeof(dir))) {
        error = "cannot read the export table";
        return std::nullopt;
    }
    if (dir.NumberOfNames > (1u << 20)) {
        error = "the export table is damaged";
        return std::nullopt;
    }
    std::vector<uint32_t> names(dir.NumberOfNames);
    std::vector<uint16_t> ordinals(dir.NumberOfNames);
    if (!reader_->read(dir.AddressOfNames, names.data(), names.size() * sizeof(uint32_t)) ||
        !reader_->read(dir.AddressOfNameOrdinals, ordinals.data(), ordinals.size() * sizeof(uint16_t))) {
        error = "cannot read the export names";
        return std::nullopt;
    }
    std::string candidate(name.size() + 1, '\0');
    for (size_t i = 0; i < names.size(); ++i) {
        // A match is the name followed by its terminator; a shorter name near
        // the end of the image may not be readable that far and is no match.
        if (!reader_->read(names[i], candidate.data(), candidate.size())) continue;
        if (candidate.back() != '\0' || std::string_view(candidate.data(), name.size()) != name) continue;
        const uint16_t ordinal = ordinals[i];
        uint32_t rva = 0;
        if (ordinal >= dir.NumberOfFunctions ||
            !reader_->read(dir.AddressOfFunctions + ordinal * 4u, &rva, sizeof(rva))) {
            error = "the export table is damaged";
            return std::nullopt;
        }
        if (rva >= export_rva_ && rva - export_rva_ < export_size_) {
            error = "export " + std::string(name) + " is forwarded to another module";
            return std::nullopt;
        }
        return rva;
    }
    return std::nullopt;
}

}  // namespace anvil
