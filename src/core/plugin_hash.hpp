#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace anvil {

// The most a plugin folder from a mod may hold, so hashing it stays cheap.
inline constexpr size_t kMaxPluginFiles = 4096;
inline constexpr uint64_t kMaxPluginBytes = 256ull << 20;

struct HashedFile {
    std::filesystem::path path;
    std::string relative;  // UTF-8, '/' between folders, ASCII letters lower-cased
    uint64_t size = 0;
    std::string sha256;    // lower-case hex
};

struct FolderHash {
    std::string sha256;             // lower-case hex; "" with `error`
    std::vector<HashedFile> files;  // by `relative`, in byte order
    uint64_t bytes = 0;
    std::string error;
};

// Files opened by hash_plugin_folder that stay open, so they cannot be
// written, renamed or deleted until this goes (the loader keeps them across
// LoadLibrary).
class FileLocks {
public:
    FileLocks() = default;
    FileLocks(const FileLocks&) = delete;
    FileLocks& operator=(const FileLocks&) = delete;
    ~FileLocks() { release(); }
    void add(HANDLE handle) { handles_.push_back(handle); }
    void release() {
        for (HANDLE h : handles_) CloseHandle(h);
        handles_.clear();
    }

private:
    std::vector<HANDLE> handles_;
};

// The hash of a plugin folder that approvals cover (design §14, #60): the
// SHA-256 of the lines `<relative path> \0 <size> \0 <SHA-256 of the file>
// \n` of every file below `folder`, sorted by path. The path is relative to
// the folder in UTF-8 with '/' and ASCII letters lower-cased, the size is
// decimal and the file's SHA-256 lower-case hex. Empty folders do not count;
// a link (junction, symbolic link), two paths that differ only in case, more
// than kMaxPluginFiles files or kMaxPluginBytes bytes, or a file that cannot
// be read is an error. With `locks` every file stays open without sharing
// write or delete access.
FolderHash hash_plugin_folder(const std::filesystem::path& folder, FileLocks* locks = nullptr);

// "1a2b3c4d…" (the first 8 hex digits and an ellipsis), for messages.
std::string short_hash(const std::string& sha256);

}  // namespace anvil
