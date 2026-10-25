#include "first_opens.hpp"

namespace anvil {
namespace {

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

FirstOpens* g_opens = nullptr;
void* g_next = nullptr;  // the rest of the chain, written by the registry

HANDLE WINAPI metered_create_file(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                                  DWORD disposition, DWORD flags, HANDLE template_file) {
    const auto next = reinterpret_cast<CreateFileWFn>(g_next);
    if (path == nullptr || !g_opens->counting() || !counts_as_file_open(access, disposition, flags)) {
        return next(path, access, share, security, disposition, flags, template_file);
    }
    LARGE_INTEGER begin{}, end{};
    QueryPerformanceCounter(&begin);
    const HANDLE file = next(path, access, share, security, disposition, flags, template_file);
    QueryPerformanceCounter(&end);
    if (file != INVALID_HANDLE_VALUE) {
        // The game reads GetLastError after the call (ERROR_ALREADY_EXISTS and such).
        const DWORD error = GetLastError();
        try {
            g_opens->add(path, end.QuadPart - begin.QuadPart);
        } catch (...) {
            // Out of memory: this open is not counted.
        }
        SetLastError(error);
    }
    return file;
}

}  // namespace

uint64_t open_path_key(const wchar_t* path) {
    uint64_t hash = 14695981039346656037ull;  // FNV-1a
    for (; *path != L'\0'; ++path) {
        wchar_t c = *path;
        if (c == L'/') c = L'\\';
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        hash = (hash ^ static_cast<uint16_t>(c)) * 1099511628211ull;
    }
    return hash;
}

bool counts_as_file_open(DWORD access, DWORD disposition, DWORD flags) {
    return (access & (GENERIC_READ | FILE_READ_DATA)) != 0 && disposition == OPEN_EXISTING &&
           (flags & FILE_FLAG_BACKUP_SEMANTICS) == 0;
}

void FirstOpens::add(const wchar_t* path, int64_t ticks) {
    const uint64_t key = open_path_key(path);
    std::lock_guard lock(mutex_);
    if (!counting() || !seen_.insert(key).second) return;
    ++files_;
    ticks_ += ticks;
}

BenchmarkOpenCost FirstOpens::total(int64_t frequency) const {
    std::lock_guard lock(mutex_);
    return {files_, frequency > 0 ? static_cast<double>(ticks_) * 1000.0 / static_cast<double>(frequency) : 0.0};
}

ChangeStatus install_first_open_meter(ChangeRegistry& changes, OwnerId owner, HMODULE image, FirstOpens& opens) {
    g_opens = &opens;
    return changes.replace_import(owner, image, "KERNEL32.dll", "CreateFileW",
                                  reinterpret_cast<void*>(&metered_create_file), &g_next);
}

}  // namespace anvil
