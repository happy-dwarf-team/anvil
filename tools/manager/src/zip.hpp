#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::manager {

// A file of a zip archive whose name is safe to write below a folder.
struct ZipEntry {
    std::string path;  // relative, '/'-separated, UTF-8; no "." or ".." parts
    uint64_t size = 0;
    uint32_t index = 0;  // in the archive
};

// Limits for archives the player hands to the Manager (plugin zips) and for
// the package it carries.
inline constexpr size_t kMaxZipEntries = 4096;
inline constexpr uint64_t kMaxZipFileBytes = 128ull << 20;
inline constexpr uint64_t kMaxZipTotalBytes = 256ull << 20;

// A zip archive held in memory (miniz). open() checks every name before
// anything is read: a path that is absolute, has a drive or stream (':'),
// "." or ".." parts, characters Windows does not allow, a reserved device
// name or a part ending in a dot or space is refused, as are two names that
// differ only in case, encrypted entries and the limits above. Directories
// are not listed; files imply them.
class ZipArchive {
public:
    static std::optional<ZipArchive> open(std::vector<uint8_t> bytes, std::string& error);
    static std::optional<ZipArchive> open_file(const std::filesystem::path& file, std::string& error);

    ZipArchive(ZipArchive&&) noexcept;
    ZipArchive& operator=(ZipArchive&&) noexcept;
    ~ZipArchive();

    const std::vector<ZipEntry>& entries() const;
    const ZipEntry* find(std::string_view path) const;  // exact name
    // The entry's bytes, checked against its CRC-32.
    std::optional<std::string> read(const ZipEntry& entry, std::string& error) const;

private:
    struct State;
    explicit ZipArchive(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

// Why `path` is not a safe relative path for a file below a folder; empty
// when it is. Backslashes count as separators.
std::string unsafe_path_reason(std::string_view path);

// A zip of `files` (path, bytes), deflated; for the tests.
std::vector<uint8_t> make_zip(const std::vector<std::pair<std::string, std::string>>& files);

}  // namespace anvil::manager
