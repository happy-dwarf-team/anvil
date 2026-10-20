#include "plugin_hash.hpp"

#include "log.hpp"

#include <bcrypt.h>

#include <algorithm>
#include <cstdio>

namespace anvil {

namespace fs = std::filesystem;

namespace {

class Sha256 {
public:
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) algorithm_ = nullptr;
        if (algorithm_ != nullptr && BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) != 0) hash_ = nullptr;
    }
    ~Sha256() {
        if (hash_ != nullptr) BCryptDestroyHash(hash_);
        if (algorithm_ != nullptr) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    bool update(const void* data, size_t size) {
        return hash_ != nullptr &&
               BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), 0) == 0;
    }
    // Lower-case hex; "" on failure.
    std::string finish() {
        uint8_t digest[32] = {};
        if (hash_ == nullptr || BCryptFinishHash(hash_, digest, sizeof(digest), 0) != 0) return {};
        char hex[65] = {};
        for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
        return hex;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

std::string relative_name(const fs::path& folder, const fs::path& file) {
    const auto u8 = file.lexically_relative(folder).generic_u8string();
    std::string out(u8.begin(), u8.end());
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// Every file below `dir`; false with `error` at a link or past the limits.
bool collect(const fs::path& root, const fs::path& dir, std::vector<HashedFile>& out, std::string& error) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        const fs::path path = it->path();
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            error = "cannot read " + relative_name(root, path) + " (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            error = relative_name(root, path) + " is a link (junction or symbolic link); a plugin folder may not have one";
            return false;
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (!collect(root, path, out, error)) return false;
            continue;
        }
        if (out.size() >= kMaxPluginFiles) {
            error = "more than " + std::to_string(kMaxPluginFiles) + " files";
            return false;
        }
        out.push_back({path, relative_name(root, path), 0, {}});
    }
    if (ec) {
        error = "cannot list " + narrow(dir.wstring()) + " (error " + std::to_string(ec.value()) + ")";
        return false;
    }
    return true;
}

}  // namespace

FolderHash hash_plugin_folder(const fs::path& folder, FileLocks* locks) {
    FolderHash result;
    const DWORD attributes = GetFileAttributesW(folder.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        result.error = narrow(folder.wstring()) + " is not a folder";
        return result;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        result.error = narrow(folder.wstring()) + " is a link (junction or symbolic link)";
        return result;
    }
    if (!collect(folder, folder, result.files, result.error)) {
        result.files.clear();
        return result;
    }
    std::sort(result.files.begin(), result.files.end(),
              [](const HashedFile& a, const HashedFile& b) { return a.relative < b.relative; });
    for (size_t i = 1; i < result.files.size(); ++i) {
        if (result.files[i].relative == result.files[i - 1].relative) {
            result.error = "two files differ only in case: " + result.files[i].relative;
            result.files.clear();
            return result;
        }
    }

    std::vector<uint8_t> buffer(1 << 20);
    Sha256 folder_hash;
    for (HashedFile& file : result.files) {
        const DWORD share = locks != nullptr ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_DELETE;
        const HANDLE handle =
            CreateFileW(file.path.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            result.error = "cannot read " + file.relative + " (error " + std::to_string(GetLastError()) + ")";
            result.files.clear();
            return result;
        }
        Sha256 file_hash;
        bool ok = true;
        DWORD read = 0;
        while ((ok = ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) != 0) && read > 0) {
            file.size += read;
            result.bytes += read;
            if (result.bytes > kMaxPluginBytes) {
                CloseHandle(handle);
                result.error = "more than " + std::to_string(kMaxPluginBytes >> 20) + " MB";
                result.files.clear();
                return result;
            }
            if (!file_hash.update(buffer.data(), read)) break;
        }
        const DWORD error = GetLastError();
        if (locks != nullptr) locks->add(handle);
        else CloseHandle(handle);
        file.sha256 = file_hash.finish();
        if (!ok || file.sha256.empty()) {
            result.error = "cannot read " + file.relative + " (error " + std::to_string(error) + ")";
            result.files.clear();
            return result;
        }
        const std::string line = file.relative + '\0' + std::to_string(file.size) + '\0' + file.sha256 + '\n';
        if (!folder_hash.update(line.data(), line.size())) break;
    }
    result.sha256 = folder_hash.finish();
    if (result.sha256.empty()) {
        result.error = "cannot compute SHA-256";
        result.files.clear();
    }
    return result;
}

std::string short_hash(const std::string& sha256) { return sha256.size() > 8 ? sha256.substr(0, 8) + "…" : sha256; }

}  // namespace anvil
