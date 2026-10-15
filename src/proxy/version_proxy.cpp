// Forwarding half of the proxy: every export of the system version.dll is
// implemented here with the exact signature and passed to the real function.
// The real DLL is loaded lazily, by full path from the system directory, on
// the first call (never from DllMain), so the proxy works even when no plugin
// is installed and Anvil itself is disabled.
#include <windows.h>

#include <atomic>

namespace {

HMODULE RealVersionDll() noexcept {
    static std::atomic<HMODULE> cached{nullptr};
    HMODULE module = cached.load(std::memory_order_acquire);
    if (module != nullptr) {
        return module;
    }
    wchar_t path[MAX_PATH + 16] = {};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return nullptr;
    }
    lstrcatW(path, L"\\version.dll");
    module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) {
        module = LoadLibraryW(path);
    }
    if (module != nullptr) {
        HMODULE expected = nullptr;
        if (!cached.compare_exchange_strong(expected, module, std::memory_order_acq_rel)) {
            FreeLibrary(module);  // another thread won; keep its handle
            module = expected;
        }
    }
    return module;
}

template <typename Fn>
Fn Real(const char* name) noexcept {
    HMODULE module = RealVersionDll();
    return module ? reinterpret_cast<Fn>(GetProcAddress(module, name)) : nullptr;
}

}  // namespace

#define ANVIL_FORWARD(name, failure, ...)                         \
    static const auto fn = Real<decltype(&::name)>(#name);       \
    if (fn == nullptr) {                                          \
        SetLastError(ERROR_PROC_NOT_FOUND);                       \
        return failure;                                           \
    }                                                             \
    return fn(__VA_ARGS__)

extern "C" {

BOOL WINAPI AnvilProxy_GetFileVersionInfoA(LPCSTR file, DWORD handle, DWORD length, LPVOID data) {
    ANVIL_FORWARD(GetFileVersionInfoA, FALSE, file, handle, length, data);
}

// Undocumented in the SDK headers: BOOL (DWORD flags, HANDLE file, DWORD length, LPVOID data).
BOOL WINAPI AnvilProxy_GetFileVersionInfoByHandle(DWORD flags, HANDLE file, DWORD length, LPVOID data) {
    using Fn = BOOL(WINAPI*)(DWORD, HANDLE, DWORD, LPVOID);
    static const auto fn = Real<Fn>("GetFileVersionInfoByHandle");
    if (fn == nullptr) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }
    return fn(flags, file, length, data);
}

BOOL WINAPI AnvilProxy_GetFileVersionInfoExA(DWORD flags, LPCSTR file, DWORD handle, DWORD length, LPVOID data) {
    ANVIL_FORWARD(GetFileVersionInfoExA, FALSE, flags, file, handle, length, data);
}

BOOL WINAPI AnvilProxy_GetFileVersionInfoExW(DWORD flags, LPCWSTR file, DWORD handle, DWORD length, LPVOID data) {
    ANVIL_FORWARD(GetFileVersionInfoExW, FALSE, flags, file, handle, length, data);
}

DWORD WINAPI AnvilProxy_GetFileVersionInfoSizeA(LPCSTR file, LPDWORD handle) {
    ANVIL_FORWARD(GetFileVersionInfoSizeA, 0, file, handle);
}

DWORD WINAPI AnvilProxy_GetFileVersionInfoSizeExA(DWORD flags, LPCSTR file, LPDWORD handle) {
    ANVIL_FORWARD(GetFileVersionInfoSizeExA, 0, flags, file, handle);
}

DWORD WINAPI AnvilProxy_GetFileVersionInfoSizeExW(DWORD flags, LPCWSTR file, LPDWORD handle) {
    ANVIL_FORWARD(GetFileVersionInfoSizeExW, 0, flags, file, handle);
}

DWORD WINAPI AnvilProxy_GetFileVersionInfoSizeW(LPCWSTR file, LPDWORD handle) {
    ANVIL_FORWARD(GetFileVersionInfoSizeW, 0, file, handle);
}

BOOL WINAPI AnvilProxy_GetFileVersionInfoW(LPCWSTR file, DWORD handle, DWORD length, LPVOID data) {
    ANVIL_FORWARD(GetFileVersionInfoW, FALSE, file, handle, length, data);
}

DWORD WINAPI AnvilProxy_VerFindFileA(DWORD flags, LPCSTR file, LPCSTR windowsDir, LPCSTR appDir, LPSTR currentDir,
                                     PUINT currentDirLength, LPSTR destDir, PUINT destDirLength) {
    ANVIL_FORWARD(VerFindFileA, 0, flags, file, windowsDir, appDir, currentDir, currentDirLength, destDir,
                  destDirLength);
}

DWORD WINAPI AnvilProxy_VerFindFileW(DWORD flags, LPCWSTR file, LPCWSTR windowsDir, LPCWSTR appDir, LPWSTR currentDir,
                                     PUINT currentDirLength, LPWSTR destDir, PUINT destDirLength) {
    ANVIL_FORWARD(VerFindFileW, 0, flags, file, windowsDir, appDir, currentDir, currentDirLength, destDir,
                  destDirLength);
}

DWORD WINAPI AnvilProxy_VerInstallFileA(DWORD flags, LPCSTR srcFile, LPCSTR destFile, LPCSTR srcDir, LPCSTR destDir,
                                        LPCSTR currentDir, LPSTR tempFile, PUINT tempFileLength) {
    ANVIL_FORWARD(VerInstallFileA, 0, flags, srcFile, destFile, srcDir, destDir, currentDir, tempFile, tempFileLength);
}

DWORD WINAPI AnvilProxy_VerInstallFileW(DWORD flags, LPCWSTR srcFile, LPCWSTR destFile, LPCWSTR srcDir,
                                        LPCWSTR destDir, LPCWSTR currentDir, LPWSTR tempFile, PUINT tempFileLength) {
    ANVIL_FORWARD(VerInstallFileW, 0, flags, srcFile, destFile, srcDir, destDir, currentDir, tempFile, tempFileLength);
}

DWORD WINAPI AnvilProxy_VerLanguageNameA(DWORD language, LPSTR buffer, DWORD size) {
    ANVIL_FORWARD(VerLanguageNameA, 0, language, buffer, size);
}

DWORD WINAPI AnvilProxy_VerLanguageNameW(DWORD language, LPWSTR buffer, DWORD size) {
    ANVIL_FORWARD(VerLanguageNameW, 0, language, buffer, size);
}

BOOL WINAPI AnvilProxy_VerQueryValueA(LPCVOID block, LPCSTR subBlock, LPVOID* buffer, PUINT length) {
    ANVIL_FORWARD(VerQueryValueA, FALSE, block, subBlock, buffer, length);
}

BOOL WINAPI AnvilProxy_VerQueryValueW(LPCVOID block, LPCWSTR subBlock, LPVOID* buffer, PUINT length) {
    ANVIL_FORWARD(VerQueryValueW, FALSE, block, subBlock, buffer, length);
}

}  // extern "C"
