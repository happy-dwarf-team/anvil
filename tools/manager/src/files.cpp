#include "files.hpp"

#include "core/log.hpp"

namespace anvil::manager {

namespace fs = std::filesystem;

namespace {

DWORD attributes(const fs::path& path) { return GetFileAttributesW(path.c_str()); }

bool is_link(DWORD attributes) {
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

}  // namespace

void list_files(const fs::path& folder, std::vector<fs::path>& out) {
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const DWORD a = attributes(it->path());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0 && !is_link(a)) {
            list_files(it->path(), out);
        } else {
            out.push_back(it->path());
        }
    }
}

DWORD remove_tree(const fs::path& path) {
    const DWORD a = attributes(path);
    if (a == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ? 0 : code;
    }
    if ((a & FILE_ATTRIBUTE_READONLY) != 0) SetFileAttributesW(path.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
    if ((a & FILE_ATTRIBUTE_DIRECTORY) == 0) return DeleteFileW(path.c_str()) ? 0 : GetLastError();
    if (!is_link(a)) {
        std::error_code ec;
        std::vector<fs::path> children;
        for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) children.push_back(it->path());
        if (ec) return static_cast<DWORD>(ec.value());
        for (const fs::path& child : children) {
            if (const DWORD code = remove_tree(child); code != 0) return code;
        }
    }
    return RemoveDirectoryW(path.c_str()) ? 0 : GetLastError();
}

bool write_file_atomic(const std::filesystem::path& file, std::string_view bytes, std::string& error,
                       bool* access_denied) {
    const auto fail = [&](const std::filesystem::path& what, const char* verb, DWORD code) {
        error = std::string(verb) + " " + narrow(what.wstring()) + " (error " + std::to_string(code) + ")";
        if (access_denied != nullptr && code == ERROR_ACCESS_DENIED) *access_denied = true;
        return false;
    };
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) return fail(file.parent_path(), "cannot create", static_cast<DWORD>(ec.value()));
    std::filesystem::path temporary = file;
    temporary += L".anvil-new";
    const HANDLE out =
        CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return fail(temporary, "cannot write", GetLastError());
    DWORD written = 0;
    const bool wrote = bytes.size() < MAXDWORD &&
                       (bytes.empty() || (WriteFile(out, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                                                    nullptr) &&
                                          written == bytes.size())) &&
                       FlushFileBuffers(out);
    const DWORD write_error = GetLastError();
    CloseHandle(out);
    if (!wrote) {
        DeleteFileW(temporary.c_str());
        return fail(temporary, "cannot write", write_error);
    }
    if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        DeleteFileW(temporary.c_str());
        return fail(file, "cannot replace", code);
    }
    return true;
}

}  // namespace anvil::manager
