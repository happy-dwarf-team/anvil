// DllMain of version.dll (Anvil).
//
// DllMain itself does the minimum: when the host process is eu4.exe, it
// overwrites the first 14 bytes of the executable's entry point with an
// absolute jump to AnvilEntry. The Windows loader finishes initializing all
// DLLs, then the main thread reaches the entry point and jumps here — outside
// the loader lock and before the game's C runtime starts. AnvilEntry restores
// the original entry bytes, runs the plugin loader and continues into the game.
// In any other process (or if anything fails) the DLL is a pure proxy.
#include "core/log.hpp"
#include "core/changes.hpp"
#include "core/runtime.hpp"

#include <windows.h>

#include <cstring>
#include <filesystem>

namespace {

using EntryFn = int(__stdcall*)(void*);

uint8_t g_savedEntry[14] = {};
uint8_t* g_entry = nullptr;

bool is_game_process(const std::filesystem::path& exe) {
    std::wstring name = exe.filename().wstring();
    for (auto& c : name) c = static_cast<wchar_t>(towlower(c));
    if (name == L"eu4.exe") return true;
    wchar_t value[8] = {};
    return GetEnvironmentVariableW(L"ANVIL_ANY_EXE", value, 8) == 1 && value[0] == L'1';  // lab/testing
}

}  // namespace

extern "C" int __stdcall AnvilEntry(void* parameter) {
    // Restore first, so plugins and [[expect]] checks see the unmodified image.
    anvil::write_memory(g_entry, g_savedEntry, sizeof(g_savedEntry));
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (length > 0 && length < std::size(path)) {
        anvil::startup(std::filesystem::path(path).parent_path());
    }
    return reinterpret_cast<EntryFn>(g_entry)(parameter);
}

namespace {

bool install_entry_hook() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.AddressOfEntryPoint == 0) {
        return false;
    }
    g_entry = base + nt->OptionalHeader.AddressOfEntryPoint;
    std::memcpy(g_savedEntry, g_entry, sizeof(g_savedEntry));
    uint8_t jump[14] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp qword ptr [rip+0]; dq AnvilEntry
    const auto target = reinterpret_cast<uintptr_t>(&AnvilEntry);
    std::memcpy(jump + 6, &target, sizeof(target));
    return anvil::write_memory(g_entry, jump, sizeof(jump));
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        anvil::shutdown();  // the call counters, if profile = true
        return TRUE;
    }
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(instance);
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path) || !is_game_process(path)) return TRUE;
    if (!install_entry_hook()) {
        anvil::log_error("could not hook the game entry point; plugins disabled for this start");
    }
    return TRUE;
}
