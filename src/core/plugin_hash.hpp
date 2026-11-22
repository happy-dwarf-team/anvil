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
    // The handle of the `index`th file hash_plugin_folder opened (its files'
    // order); INVALID_HANDLE_VALUE past the last.
    HANDLE at(size_t index) const { return index < handles_.size() ? handles_[index] : INVALID_HANDLE_VALUE; }
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

// A plugin folder hashed with every file held open without write or delete
// sharing (T3, #545). While it lives no file it hashed can be written,
// renamed or deleted, and read() gives a file's bytes through the handle the
// hash read it with: what an approval of this hash covers. A file that is
// not among the hashed ones (added after the hash) reads as missing.
class HeldFolder {
public:
    explicit HeldFolder(const std::filesystem::path& folder);
    HeldFolder(const HeldFolder&) = delete;
    HeldFolder& operator=(const HeldFolder&) = delete;

    const std::filesystem::path& folder() const { return folder_; }
    const FolderHash& hash() const { return hash_; }
    bool ok() const { return hash_.error.empty(); }

    enum class Read { ok, missing, unreadable, too_large };
    // `relative` as plugin.toml names a file of the folder, matched as the
    // hash names it ('/' between folders, ASCII letters lower-cased, as the
    // file system matches names). Nothing reads while the hash failed.
    Read read(const std::filesystem::path& relative, uint64_t max_bytes, std::string& out) const;
    bool contains(const std::filesystem::path& relative) const;

private:
    // The index of `relative` in hash_.files; -1 for none.
    long long find(const std::filesystem::path& relative) const;

    std::filesystem::path folder_;
    FileLocks locks_;
    FolderHash hash_;
};

// "1a2b3c4d…" (the first 8 hex digits and an ellipsis), for messages.
std::string short_hash(const std::string& sha256);

}  // namespace anvil
