#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil {

// The kind of memory a section gives, from its flags: code (executable),
// write (writable data) or read (read-only data).
enum class MemoryKind { Code, Read, Write };

const char* memory_kind_name(MemoryKind kind);

// Bytes of a PE image addressed by RVA, laid out as the image is mapped.
// Symbol verification reads only through this interface, so the same rules
// apply to the running game (the loader, which also shows other plugins'
// changes in their original form) and to an eu4.exe file (anvil check --exe).
class ImageReader {
public:
    virtual ~ImageReader() = default;
    // Copies `size` bytes at `rva` to `out`; false when any of them lies
    // outside the image or cannot be read.
    virtual bool read(uint32_t rva, void* out, size_t size) const = 0;
    // The address the image's absolute pointers are relative to: the load
    // address in memory, OptionalHeader.ImageBase in a file.
    virtual uint64_t base() const = 0;
};

// An image mapped in this process, e.g. eu4.exe at its load address.
class MemoryImageReader final : public ImageReader {
public:
    MemoryImageReader(uintptr_t base, uint32_t size_of_image) : base_(base), size_(size_of_image) {}
    bool read(uint32_t rva, void* out, size_t size) const override;
    uint64_t base() const override { return base_; }

private:
    uintptr_t base_;
    uint32_t size_;
};

// A PE file held in memory and mapped through its section table: bytes past a
// section's raw data read as zero up to its virtual size.
class FileImageReader final : public ImageReader {
public:
    static std::optional<FileImageReader> from_bytes(std::vector<uint8_t> file, std::string& error);
    static std::optional<FileImageReader> load(const std::filesystem::path& file, std::string& error);

    bool read(uint32_t rva, void* out, size_t size) const override;
    uint64_t base() const override { return image_base_; }

private:
    struct Range {
        uint32_t rva;
        uint32_t size;      // in the image
        uint32_t offset;    // in the file
        uint32_t raw_size;  // bytes present in the file
    };
    FileImageReader() = default;

    std::vector<uint8_t> file_;
    std::vector<Range> ranges_;  // headers first, then the sections
    uint64_t image_base_ = 0;
    uint32_t size_of_image_ = 0;
};

struct Section {
    std::string name;
    uint32_t rva = 0;
    uint32_t size = 0;  // VirtualSize (SizeOfRawData when that is 0)
    uint32_t characteristics = 0;
    MemoryKind kind = MemoryKind::Read;
};

// The headers, sections and export table of a 64-bit PE image. Keeps a
// reference to `reader`, which must outlive it.
class PeImage {
public:
    static std::optional<PeImage> parse(const ImageReader& reader, std::string& error);

    const ImageReader& reader() const { return *reader_; }
    uint32_t size_of_image() const { return size_of_image_; }
    const std::vector<Section>& sections() const { return sections_; }

    // The section holding all of [rva, rva + size), or nullptr.
    const Section* section_of(uint32_t rva, uint32_t size) const;

    // The RVA of an export by name. nullopt with an empty `error` when the
    // name is not exported; with `error` set for a forwarded export or a
    // damaged export table.
    std::optional<uint32_t> find_export(std::string_view name, std::string& error) const;

private:
    explicit PeImage(const ImageReader& reader) : reader_(&reader) {}

    const ImageReader* reader_;
    uint32_t size_of_image_ = 0;
    uint32_t export_rva_ = 0;
    uint32_t export_size_ = 0;
    std::vector<Section> sections_;
};

}  // namespace anvil
