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
        if (hash_ == nullptr ||
            BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), 0) != 0) {
            failed_ = true;
        }
        return !failed_;
    }
    // Lower-case hex; "" on failure, also of any update before (T6, #545):
    // a hash of part of the bytes is never a hash.
    std::string finish() {
        uint8_t digest[32] = {};
        if (hash_ == nullptr || failed_ || BCryptFinishHash(hash_, digest, sizeof(digest), 0) != 0) return {};
        char hex[65] = {};
        for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
        return hex;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    bool failed_ = false;
};

// UTF-8 with forward slashes. generic_u8string() throws on a name that is not
// valid UTF-16 (a lone surrogate, which NTFS allows), and nothing above the
// inventory catches it: one such file in any enabled mod stopped every plugin.
std::string relative_name(const fs::path& folder, const fs::path& file) {
    std::string out = narrow(file.lexically_relative(folder).generic_wstring());
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool valid_utf16(const std::wstring& name) {
    return name.empty() || WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()),
                                               nullptr, 0, nullptr, nullptr) > 0;
}

// Every file below `dir`; false with `error` at a link, a name that is not
// valid Unicode (it has no UTF-8 name to hash) or past the limits.
bool collect(const fs::path& root, const fs::path& dir, std::vector<HashedFile>& out, std::string& error) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        const fs::path path = it->path();
        if (!valid_utf16(path.filename().wstring())) {
            error = relative_name(root, path) + " has a name that is not valid Unicode; a plugin folder may not have one";
            return false;
        }
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

HeldFolder::HeldFolder(const fs::path& folder) : folder_(folder), hash_(hash_plugin_folder(folder, &locks_)) {
    if (!hash_.error.empty()) locks_.release();
}

long long HeldFolder::find(const fs::path& relative) const {
    if (!ok() || relative.empty() || relative.is_absolute() || relative.has_root_name()) return -1;
    const fs::path normal = relative.lexically_normal();
    if (normal.empty() || *normal.begin() == "..") return -1;
    std::string name = narrow(normal.generic_wstring());
    for (char& c : name) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    const auto it = std::lower_bound(hash_.files.begin(), hash_.files.end(), name,
                                     [](const HashedFile& f, const std::string& n) { return f.relative < n; });
    if (it == hash_.files.end() || it->relative != name) return -1;
    return it - hash_.files.begin();
}

bool HeldFolder::contains(const fs::path& relative) const { return find(relative) >= 0; }

HeldFolder::Read HeldFolder::read(const fs::path& relative, uint64_t max_bytes, std::string& out) const {
    out.clear();
    const long long index = find(relative);
    if (index < 0) return Read::missing;
    const HashedFile& file = hash_.files[static_cast<size_t>(index)];
    if (file.size > max_bytes) return Read::too_large;
    const HANDLE handle = locks_.at(static_cast<size_t>(index));
    LARGE_INTEGER start{};
    if (handle == INVALID_HANDLE_VALUE || !SetFilePointerEx(handle, start, nullptr, FILE_BEGIN)) return Read::unreadable;
    // The size the hash read: nobody could write the file since.
    out.resize(static_cast<size_t>(file.size));
    size_t done = 0;
    while (done < out.size()) {
        DWORD read = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(out.size() - done, 1u << 20));
        if (!ReadFile(handle, out.data() + done, want, &read, nullptr) || read == 0) {
            out.clear();
            return Read::unreadable;
        }
        done += read;
    }
    return Read::ok;
}

std::string short_hash(const std::string& sha256) { return sha256.size() > 8 ? sha256.substr(0, 8) + "…" : sha256; }

}  // namespace anvil
