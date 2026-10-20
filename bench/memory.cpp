// anvil_bench --memory: the memory budgets of design §17 (core <= 2 MB,
// <= 1 KB per change, <= 2 KB per mid hook or block replacement).
//
// Core: a child process (anvil_bench --memory-core DIR) runs the real
// anvil::startup() on DIR, a game folder with anvil/anvil.toml and an empty
// anvil/plugins, and compares the process before the call and after its
// main-phase thread has ended: the Runtime, the change registry, the
// ServiceBus, the log, the settings, safe mode, the gameplay policy, the build
// table lookup, crash capture with its helper thread, and the DLLs it loads
// (dbghelp.dll and its dependencies). The image of version.dll is not private
// memory: its writable sections, read from its headers, are added to the
// core; its code and read-only data are shared with the file and only
// reported. A child process starts clean, keeps the crash handlers out of the
// per-change part, and lets the parent delete DIR once the log is closed.
//
// Per change: for each kind, N changes of one owner at N distinct sites of a
// synthetic image (executable scratch memory, a PE header with N imports for
// import replacements), measured every N/10 changes; the result is the slope
// of a least-squares line, so the first allocations (an arena, a vector's
// capacity) count as their share of N rather than as a fixed cost. Each kind
// runs in its own child process (anvil_bench --memory-kind NAME N RESULT):
// in one process, the heap commit one kind left behind or the commit step it
// ended on fell on the next kind (a block replacement read 1644 B in 1.0
// right after the mid hooks, and some 1450 B alone).
//
// Every snapshot reads:
//   - private: PROCESS_MEMORY_COUNTERS_EX.PrivateUsage, the commit charge
//     (what the budgets check);
//   - heap: the busy blocks of every process heap (HeapWalk), the bytes asked
//     for without the heap's own headers;
//   - executable: committed private pages with an execute protection
//     (VirtualQuery), i.e. Anvil's stub arenas and SafetyHook's trampolines;
//   - reserved: private address space reserved but not committed (a thread's
//     stack reservation), which is not memory and not budgeted.
#include "core/changes.hpp"
#include "core/runtime.hpp"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace {

using namespace anvil;

constexpr uint64_t kCoreBudget = 2 * 1024 * 1024;
constexpr double kChangeBudget = 1024;
// A mid hook or a block replacement has its own stub with a saved context and
// a callback list, so its budget is larger.
constexpr double kStubChangeBudget = 2048;

double change_budget(const std::string& kind) {
    return kind == "mid_hook" || kind == "block_replacement" ? kStubChangeBudget : kChangeBudget;
}
constexpr int kCheckpoints = 10;

struct Snapshot {
    double private_bytes = 0;
    double heap_bytes = 0;
    double heap_blocks = 0;
    double executable_bytes = 0;
    double reserved_bytes = 0;
};

// Without allocating, so a snapshot does not change what the next one reads.
Snapshot snapshot() {
    Snapshot s;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        s.private_bytes = static_cast<double>(counters.PrivateUsage);
    }
    static HANDLE heaps[256];
    const DWORD count = std::min<DWORD>(GetProcessHeaps(static_cast<DWORD>(std::size(heaps)), heaps),
                                        static_cast<DWORD>(std::size(heaps)));
    for (DWORD i = 0; i < count; ++i) {
        if (!HeapLock(heaps[i])) continue;
        PROCESS_HEAP_ENTRY entry{};
        while (HeapWalk(heaps[i], &entry)) {
            if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
                s.heap_bytes += entry.cbData;
                s.heap_blocks += 1;
            }
        }
        HeapUnlock(heaps[i]);
    }
    constexpr DWORD kExecute = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    MEMORY_BASIC_INFORMATION mbi{};
    for (const uint8_t* p = nullptr; VirtualQuery(p, &mbi, sizeof(mbi)) != 0;
         p = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.Type != MEM_PRIVATE) continue;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & kExecute)) s.executable_bytes += static_cast<double>(mbi.RegionSize);
        if (mbi.State == MEM_RESERVE) s.reserved_bytes += static_cast<double>(mbi.RegionSize);
    }
    return s;
}

Snapshot operator-(const Snapshot& a, const Snapshot& b) {
    return {a.private_bytes - b.private_bytes, a.heap_bytes - b.heap_bytes, a.heap_blocks - b.heap_blocks,
            a.executable_bytes - b.executable_bytes, a.reserved_bytes - b.reserved_bytes};
}

// This process's threads, into a fixed array: the id and whether the thread
// started in this executable (Anvil is linked into it, so its threads do;
// the loader's and the thread pool's start in ntdll).
struct ThreadInfo {
    DWORD id;
    bool ours;
};

size_t threads(ThreadInfo* out, size_t capacity) {
    using QueryFn = LONG(WINAPI*)(HANDLE, int, void*, ULONG, ULONG*);
    static const auto query =
        reinterpret_cast<QueryFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const size_t size = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew)->OptionalHeader.SizeOfImage;
    size_t n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snap, &entry); more && n < capacity; more = Thread32Next(snap, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
        uintptr_t start = 0;
        if (HANDLE t = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID)) {
            constexpr int kWin32StartAddress = 9;  // ThreadQuerySetWin32StartAddress
            if (query != nullptr) query(t, kWin32StartAddress, &start, sizeof(start), nullptr);
            CloseHandle(t);
        }
        out[n++] = {entry.th32ThreadID, start >= reinterpret_cast<uintptr_t>(base) &&
                                            start < reinterpret_cast<uintptr_t>(base) + size};
    }
    CloseHandle(snap);
    return n;
}

// True when `path` contains `needle`; reads into a static buffer (no heap).
bool file_contains(const wchar_t* path, const char* needle) {
    static char text[1 << 16];
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD read = 0;
    const BOOL ok = ReadFile(f, text, sizeof(text) - 1, &read, nullptr);
    CloseHandle(f);
    if (!ok) return false;
    text[read] = 0;
    return std::strstr(text, needle) != nullptr;
}

std::wstring self_path() {
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return path;
}

// --- core -----------------------------------------------------------------

// What the core result file holds: "name value" lines and "module name size" lines.
struct Module {
    std::string name;
    double size_of_image = 0;
    double writable_bytes = 0;
};

struct Core {
    Snapshot delta;
    double threads_added = 0;   // threads Anvil started and that still run (the crash helper)
    double other_threads = 0;   // threads the system started meanwhile (a thread-pool worker)
    double threads_exited = 0;  // threads of the process before start-up that ended meanwhile
    double wait_ms = 0;         // from startup() until the main-phase thread had ended
    std::vector<Module> modules;  // DLLs the start-up loaded (dbghelp.dll and what it needs)
};

// Sizes from a loaded module's headers.
Module module_info(HMODULE m) {
    Module info;
    wchar_t name[MAX_PATH] = {};
    GetModuleBaseNameW(GetCurrentProcess(), m, name, MAX_PATH);
    for (const wchar_t* c = name; *c; ++c) info.name += static_cast<char>(*c < 128 ? *c : '?');
    const auto* base = reinterpret_cast<const uint8_t*>(m);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    info.size_of_image = nt->OptionalHeader.SizeOfImage;
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (sections[i].Characteristics & IMAGE_SCN_MEM_WRITE) info.writable_bytes += sections[i].Misc.VirtualSize;
    }
    return info;
}

// The child: anvil::startup() on game_dir, then the result into result_file.
int core_child(const std::filesystem::path& game_dir, const std::filesystem::path& result_file) {
    const std::wstring log = (game_dir / "anvil" / "anvil.log").wstring();
    static ThreadInfo before_threads[256], now_threads[256];
    static HMODULE before_modules[1024], now_modules[1024];
    DWORD bytes = 0;
    // Warm-up: the first call of each API may allocate.
    snapshot();
    threads(now_threads, std::size(now_threads));
    file_contains(log.c_str(), "status: ");
    EnumProcessModules(GetCurrentProcess(), now_modules, sizeof(now_modules), &bytes);

    EnumProcessModules(GetCurrentProcess(), before_modules, sizeof(before_modules), &bytes);
    const size_t before_module_count = bytes / sizeof(HMODULE);
    const size_t before_count = threads(before_threads, std::size(before_threads));
    const Snapshot before = snapshot();
    LARGE_INTEGER t0{}, t1{}, frequency{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&t0);
    anvil::startup(game_dir);
    // The main phase runs on its own thread: wait until it has logged its
    // summary and ended, so that only Anvil's threads that stay are counted.
    // Loading dbghelp.dll may make the system's thread pool start a worker
    // that idles for longer than this waits; such threads are in the
    // measurement (a stack and a TEB, some 40 KB) and in other_threads_added.
    size_t added = 0, others = 0, exited = 0;
    bool done = false;
    const ULONGLONG deadline = GetTickCount64() + 20000;
    while (!done && GetTickCount64() < deadline) {
        const size_t n = threads(now_threads, std::size(now_threads));
        added = others = 0;
        for (size_t a = 0; a < n; ++a) {
            bool known = false;
            for (size_t b = 0; b < before_count && !known; ++b) known = before_threads[b].id == now_threads[a].id;
            if (!known) ++(now_threads[a].ours ? added : others);
        }
        exited = before_count + added + others - n;
        done = added <= 1 && file_contains(log.c_str(), "status: ");
        if (!done) Sleep(1);
    }
    QueryPerformanceCounter(&t1);
    const Snapshot after = snapshot();
    if (!done) {
        std::fprintf(stderr, "anvil_bench: the core start-up did not finish within 20 s\n");
        return 1;
    }
    EnumProcessModules(GetCurrentProcess(), now_modules, sizeof(now_modules), &bytes);
    const size_t module_count = std::min<size_t>(bytes / sizeof(HMODULE), std::size(now_modules));
    FILE* f = nullptr;
    if (_wfopen_s(&f, result_file.c_str(), L"wb") != 0 || f == nullptr) return 1;
    const Snapshot d = after - before;
    std::fprintf(f, "private %.0f\nheap %.0f\nheap_blocks %.0f\nexecutable %.0f\nreserved %.0f\n", d.private_bytes,
                 d.heap_bytes, d.heap_blocks, d.executable_bytes, d.reserved_bytes);
    std::fprintf(f, "threads_added %zu\nother_threads_added %zu\nthreads_exited %zu\nwait_ms %.1f\n", added, others,
                 exited,
                 static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart));
    for (size_t i = 0; i < module_count; ++i) {
        if (std::find(before_modules, before_modules + before_module_count, now_modules[i]) !=
            before_modules + before_module_count) {
            continue;
        }
        const Module m = module_info(now_modules[i]);
        std::fprintf(f, "module %s %.0f %.0f\n", m.name.c_str(), m.size_of_image, m.writable_bytes);
    }
    std::fclose(f);
    return 0;
}

// Runs this executable with `arguments` and waits up to timeout_ms; false
// with `error` when it cannot start, takes longer or exits with an error.
bool run_child(const std::wstring& arguments, DWORD timeout_ms, const std::string& what, std::string& error) {
    std::wstring command = L"\"" + self_path() + L"\" " + arguments;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        error = "cannot start the " + what + " child process (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, timeout_ms) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        error = "the " + what + " child process did not finish within " + std::to_string(timeout_ms / 1000) + " s";
    }
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (error.empty() && code != 0) {
        error = "the " + what + " child process failed (exit code " + std::to_string(code) + ")";
    }
    return error.empty();
}

bool run_core(Core& core, std::string& error) {
    std::error_code ec;
    const auto dir =
        std::filesystem::temp_directory_path(ec) / ("anvil-bench-memory-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / "anvil" / "plugins", ec);
    if (ec) {
        error = "cannot create " + dir.string();
        return false;
    }
    // The anvil.toml Anvil ships (its values are the defaults).
    std::filesystem::copy_file(ANVIL_SOURCE_DIR "/dist/anvil.toml", dir / "anvil" / "anvil.toml", ec);
    const auto result_file = dir / "core-result.txt";
    SetEnvironmentVariableW(L"ANVIL_DISABLE", nullptr);  // the child inherits it; its start-up must load
    FILE* f = nullptr;
    if (run_child(L"--memory-core \"" + dir.wstring() + L"\" \"" + result_file.wstring() + L"\"", 60000, "core",
                  error) &&
        (_wfopen_s(&f, result_file.c_str(), L"rb") != 0 || f == nullptr)) {
        error = "the core child process wrote no result";
    }
    if (f != nullptr) {
        char line[512];
        while (std::fgets(line, sizeof(line), f) != nullptr) {
            char name[256] = {};
            double value = 0, writable = 0;
            if (sscanf_s(line, "module %255s %lf %lf", name, static_cast<unsigned>(sizeof(name)), &value,
                            &writable) == 3) {
                core.modules.push_back({name, value, writable});
                continue;
            }
            if (sscanf_s(line, "%255s %lf", name, static_cast<unsigned>(sizeof(name)), &value) != 2) continue;
            const std::string n = name;
            if (n == "private") core.delta.private_bytes = value;
            else if (n == "heap") core.delta.heap_bytes = value;
            else if (n == "heap_blocks") core.delta.heap_blocks = value;
            else if (n == "executable") core.delta.executable_bytes = value;
            else if (n == "reserved") core.delta.reserved_bytes = value;
            else if (n == "threads_added") core.threads_added = value;
            else if (n == "other_threads_added") core.other_threads = value;
            else if (n == "threads_exited") core.threads_exited = value;
            else if (n == "wait_ms") core.wait_ms = value;
        }
        std::fclose(f);
    }
    std::filesystem::remove_all(dir, ec);
    return error.empty();
}

// version.dll's image from its headers: not private memory, so not in the core
// snapshot. Writable sections become private copies of the pages written.
struct Image {
    bool found = false;
    std::string path;
    double file_bytes = 0;
    double size_of_image = 0;
    double writable_bytes = 0;  // virtual size of the writable sections
};

Image read_image(const std::filesystem::path& path) {
    Image image;
    image.path = path.filename().string();
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr) return image;
    std::vector<uint8_t> head(4096);
    head.resize(std::fread(head.data(), 1, head.size(), f));
    std::fclose(f);
    if (head.size() < sizeof(IMAGE_DOS_HEADER)) return image;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(head.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > head.size()) {
        return image;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(head.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return image;
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    if (reinterpret_cast<const uint8_t*>(sections + nt->FileHeader.NumberOfSections) > head.data() + head.size()) {
        return image;
    }
    image.found = true;
    std::error_code ec;
    image.file_bytes = static_cast<double>(std::filesystem::file_size(path, ec));
    image.size_of_image = nt->OptionalHeader.SizeOfImage;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (sections[i].Characteristics & IMAGE_SCN_MEM_WRITE) image.writable_bytes += sections[i].Misc.VirtualSize;
    }
    return image;
}

// --- per change -------------------------------------------------------------

// Executable scratch memory as the registry's image.
struct Scratch {
    uint8_t* base;
    size_t size;
    explicit Scratch(size_t bytes) : size(bytes) {
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        if (base == nullptr) std::abort();
        std::memset(base, 0xCC, bytes);
    }
    uintptr_t at(size_t offset) const { return reinterpret_cast<uintptr_t>(base) + offset; }
    void put(size_t offset, const void* code, size_t n) { std::memcpy(base + offset, code, n); }
};

// f(x) = x + 1: lea eax, [rcx+1]; 9x nop; ret.
const uint8_t kAddOne[] = {0x8D, 0x41, 0x01, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xC3};
// lea eax, [rcx+1]; add eax, 2; add eax, 4; add eax, 8; ret (the block: +3, 9 bytes).
const uint8_t kSteps[] = {0x8D, 0x41, 0x01, 0x83, 0xC0, 0x02, 0x83, 0xC0, 0x04, 0x83, 0xC0, 0x08, 0xC3};
constexpr size_t kSpacing = 0x20;  // one function per 32 bytes

uint32_t detour(uint32_t x) { return x; }  // never called
void callback(AnvilCpuContext*, void*) {}

struct Kind {
    std::string name;
    std::string what;
    std::vector<double> counts;
    std::vector<Snapshot> points;  // relative to the first
    // Slopes (bytes per change), the private intercept and the last point.
    Snapshot slope;
    double fixed_private = 0;
    Snapshot total;
};

void fit(Kind& k) {
    const size_t n = k.counts.size();
    double mx = 0;
    for (double x : k.counts) mx += x / static_cast<double>(n);
    double sxx = 0;
    for (double x : k.counts) sxx += (x - mx) * (x - mx);
    auto slope = [&](double Snapshot::*field) {
        double my = 0, sxy = 0;
        for (const auto& p : k.points) my += p.*field / static_cast<double>(n);
        for (size_t i = 0; i < n; ++i) sxy += (k.counts[i] - mx) * (k.points[i].*field - my);
        return std::make_pair(sxy / sxx, my - sxy / sxx * mx);
    };
    k.slope.private_bytes = slope(&Snapshot::private_bytes).first;
    k.fixed_private = slope(&Snapshot::private_bytes).second;
    k.slope.heap_bytes = slope(&Snapshot::heap_bytes).first;
    k.slope.heap_blocks = slope(&Snapshot::heap_blocks).first;
    k.slope.executable_bytes = slope(&Snapshot::executable_bytes).first;
    k.total = k.points.back();
}

// Adds n changes through add(i), with a snapshot before the first and every n/10.
bool measure(Kind& k, size_t n, const std::function<ChangeStatus(size_t)>& add) {
    k.counts.reserve(kCheckpoints + 1);
    k.points.reserve(kCheckpoints + 1);
    const Snapshot base = snapshot();
    k.counts.push_back(0);
    k.points.push_back({});
    for (int c = 1; c <= kCheckpoints; ++c) {
        const size_t end = n * c / kCheckpoints;
        for (size_t i = n * (c - 1) / kCheckpoints; i < end; ++i) {
            if (const ChangeStatus st = add(i); st != ChangeStatus::Ok) {
                std::fprintf(stderr, "anvil_bench: %s %zu refused: %s\n", k.name.c_str(), i, change_status_name(st));
                return false;
            }
        }
        k.counts.push_back(static_cast<double>(end));
        k.points.push_back(snapshot() - base);
    }
    fit(k);
    return true;
}

// A PE header in scratch memory importing f0000 ... from bench.dll.
size_t build_imports(Scratch& s, size_t n, std::vector<std::string>& names) {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(s.base);
    std::memset(s.base, 0, 0x400);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(s.base + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt->OptionalHeader.SizeOfImage = static_cast<DWORD>(s.size);
    const size_t descriptors = 0x200, module = 0x240, function = 0x280;  // function: a ret
    const size_t lookup = 0x1000, slots = lookup + 8 * (n + 1), by_name = slots + 8 * (n + 1);
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = {static_cast<DWORD>(descriptors),
                                                                      2 * sizeof(IMAGE_IMPORT_DESCRIPTOR)};
    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(s.base + descriptors);
    std::memset(desc, 0, 2 * sizeof(*desc));
    desc->OriginalFirstThunk = static_cast<DWORD>(lookup);
    desc->FirstThunk = static_cast<DWORD>(slots);
    desc->Name = static_cast<DWORD>(module);
    std::memcpy(s.base + module, "bench.dll", 10);
    s.base[function] = 0xC3;
    auto* lookups = reinterpret_cast<uint64_t*>(s.base + lookup);
    auto* iat = reinterpret_cast<uint64_t*>(s.base + slots);
    for (size_t i = 0; i < n; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "f%05zu", i);
        names.emplace_back(name);
        const size_t at = by_name + 16 * i;
        std::memset(s.base + at, 0, 16);
        std::memcpy(s.base + at + 2, name, std::strlen(name) + 1);
        lookups[i] = at;
        iat[i] = s.at(function);
    }
    lookups[n] = iat[n] = 0;
    return by_name + 16 * n;
}

std::string json_number(double v) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.1f", v);
    return b;
}

std::string json_snapshot(const Snapshot& s) {
    return "\"private_bytes\": " + json_number(s.private_bytes) + ", \"heap_bytes\": " + json_number(s.heap_bytes) +
           ", \"heap_blocks\": " + json_number(s.heap_blocks) +
           ", \"executable_bytes\": " + json_number(s.executable_bytes) +
           ", \"reserved_bytes\": " + json_number(s.reserved_bytes);
}

}  // namespace

int memory_core_main(int argc, char** argv) {
    if (argc != 4) return 2;
    return core_child(std::filesystem::path(argv[2]), std::filesystem::path(argv[3]));
}

namespace {

constexpr const char* kKindNames[] = {"function_hook", "call_redirect",  "byte_patch",        "mid_hook",
                                      "block_replacement", "pointer_swap", "import_replacement"};

// Measures the kind named `only` in this process. Everything a plugin would
// own (its next pointers, names) and every image exist before the first
// snapshot; registries stay alive.
bool measure_kind(size_t n, const std::string& only, Kind& out) {
    std::vector<void*> nexts(n);
    std::vector<std::string> import_names;
    import_names.reserve(n);
    std::vector<std::unique_ptr<Scratch>> images;
    std::vector<std::unique_ptr<ChangeRegistry>> registries;
    auto image_for = [&](size_t bytes) -> Scratch& {
        images.push_back(std::make_unique<Scratch>((bytes + 0xFFFF) & ~size_t{0xFFFF}));
        return *images.back();
    };
    Scratch& hooks = image_for(n * kSpacing);
    Scratch& redirects = image_for(kSpacing + n * kSpacing);
    Scratch& patches = image_for(n * 0x10);
    Scratch& mids = image_for(n * kSpacing);
    Scratch& blocks = image_for(n * kSpacing);
    Scratch& slots = image_for(0x1000 + n * 8);
    Scratch& imports = image_for(0x1000 + n * 32 + 16);
    for (size_t i = 0; i < n; ++i) {
        hooks.put(i * kSpacing, kAddOne, sizeof(kAddOne));
        mids.put(i * kSpacing, kAddOne, sizeof(kAddOne));
        blocks.put(i * kSpacing, kSteps, sizeof(kSteps));
        // A caller: sub rsp, 28h; call rel32 (the shared callee at 0); add rsp, 28h; ret.
        const size_t at = kSpacing + i * kSpacing;
        const auto rel = static_cast<int32_t>(-static_cast<intptr_t>(at + 4 + 5));
        uint8_t code[] = {0x48, 0x83, 0xEC, 0x28, 0xE8, 0, 0, 0, 0, 0x48, 0x83, 0xC4, 0x28, 0xC3};
        std::memcpy(&code[5], &rel, sizeof(rel));
        redirects.put(at, code, sizeof(code));
        const uintptr_t value = slots.at(0);
        slots.put(0x1000 + i * 8, &value, sizeof(value));
    }
    redirects.put(0, kAddOne, sizeof(kAddOne));
    build_imports(imports, n, import_names);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

    for (Scratch* s : {&hooks, &redirects, &patches, &mids, &blocks, &slots, &imports}) {
        registries.push_back(std::make_unique<ChangeRegistry>(s->at(0), static_cast<uint32_t>(s->size)));
        registries.back()->set_refusal_sink(
            [](OwnerId, const std::string& m) { std::fprintf(stderr, "anvil_bench: refused: %s\n", m.c_str()); });
        registries.back()->add_owner("plugin");
    }
    auto target = [](const Scratch& s, size_t offset) { return ChangeTarget{s.at(offset), true, nullptr, nullptr, 0}; };
    const uint8_t nops[4] = {0x90, 0x90, 0x90, 0x90};
    const auto fn = reinterpret_cast<void*>(&detour);
    struct Spec {
        const char* name;
        const char* what;
        std::function<ChangeStatus(size_t)> add;
    };
    const std::vector<Spec> specs = {
        {"function_hook", "function hook at its own function",
         [&](size_t i) { return registries[0]->hook_function(0, target(hooks, i * kSpacing), fn, &nexts[i]); }},
        {"call_redirect", "call redirect at its own call rel32 site",
         [&](size_t i) {
             return registries[1]->redirect_call(0, target(redirects, kSpacing + i * kSpacing + 4), redirects.at(0), fn,
                                                 &nexts[i]);
         }},
        {"byte_patch", "byte patch of 4 bytes",
         [&](size_t i) { return registries[2]->patch_bytes(0, target(patches, i * 0x10), nops, sizeof(nops)); }},
        {"mid_hook", "mid hook at its own function",
         [&](size_t i) { return registries[3]->mid_hook(0, target(mids, i * kSpacing), &callback, nullptr); }},
        {"block_replacement", "block replacement of 9 bytes",
         [&](size_t i) {
             return registries[4]->replace_block(0, target(blocks, i * kSpacing + 3), 9, 0, &callback, nullptr);
         }},
        {"pointer_swap", "pointer swap of its own slot",
         [&](size_t i) { return registries[5]->swap_pointer(0, target(slots, 0x1000 + i * 8), 0, fn, &nexts[i]); }},
        {"import_replacement", "import replacement of its own import",
         [&](size_t i) {
             return registries[6]->replace_import(0, reinterpret_cast<HMODULE>(imports.base), "bench.dll",
                                                  import_names[i].c_str(), fn, &nexts[i]);
         }},
    };
    static_assert(std::size(kKindNames) == 7);
    for (const auto& spec : specs) {
        if (spec.name != only) continue;
        out = {spec.name, spec.what, {}, {}, {}, 0, {}};
        return measure(out, n, spec.add);
    }
    std::fprintf(stderr, "anvil_bench: no kind of change named %s\n", only.c_str());
    return false;
}

// Each kind in a child process: the result file holds "name value" lines.
bool run_kind(const char* name, size_t n, const std::filesystem::path& dir, Kind& k, std::string& error) {
    const auto result_file = dir / (std::string(name) + ".txt");
    const std::wstring wide_name(name, name + std::strlen(name));
    FILE* f = nullptr;
    if (!run_child(L"--memory-kind " + wide_name + L" " + std::to_wstring(n) + L" \"" + result_file.wstring() + L"\"",
                   600000, name, error)) {
        return false;
    }
    if (_wfopen_s(&f, result_file.c_str(), L"rb") != 0 || f == nullptr) {
        error = std::string("the ") + name + " child process wrote no result";
        return false;
    }
    k = {name, "", {}, {}, {}, 0, {}};
    char line[512];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::strncmp(line, "what ", 5) == 0) {
            k.what.assign(line + 5, std::strcspn(line + 5, "\r\n"));
            continue;
        }
        char key[64] = {};
        double value = 0;
        if (sscanf_s(line, "%63s %lf", key, static_cast<unsigned>(sizeof(key)), &value) != 2) continue;
        const std::string field = key;
        if (field == "private") k.slope.private_bytes = value;
        else if (field == "heap") k.slope.heap_bytes = value;
        else if (field == "heap_blocks") k.slope.heap_blocks = value;
        else if (field == "executable") k.slope.executable_bytes = value;
        else if (field == "fixed_private") k.fixed_private = value;
        else if (field == "total_private") k.total.private_bytes = value;
        else if (field == "total_heap") k.total.heap_bytes = value;
        else if (field == "total_executable") k.total.executable_bytes = value;
    }
    std::fclose(f);
    return true;
}

}  // namespace

int memory_kind_main(int argc, char** argv) {
    if (argc != 5) return 2;
    const size_t n = std::strtoull(argv[3], nullptr, 10);
    Kind k;
    if (n < 10 || !measure_kind(n, argv[2], k)) return 1;
    FILE* f = nullptr;
    if (_wfopen_s(&f, std::filesystem::path(argv[4]).c_str(), L"wb") != 0 || f == nullptr) return 1;
    std::fprintf(f, "private %.3f\nheap %.3f\nheap_blocks %.3f\nexecutable %.3f\nfixed_private %.3f\n",
                 k.slope.private_bytes, k.slope.heap_bytes, k.slope.heap_blocks, k.slope.executable_bytes,
                 k.fixed_private);
    std::fprintf(f, "total_private %.0f\ntotal_heap %.0f\ntotal_executable %.0f\nwhat %s\n", k.total.private_bytes,
                 k.total.heap_bytes, k.total.executable_bytes, k.what.c_str());
    std::fclose(f);
    return 0;
}

int memory_main(size_t n, const char* out_path) {
    // Core, in a child process.
    Core core;
    if (std::string error; !run_core(core, error)) {
        std::fprintf(stderr, "anvil_bench: core: %s\n", error.c_str());
        return 1;
    }
    const Image image = read_image(std::filesystem::path(self_path()).parent_path() / "version.dll");

    // Per change, each kind in its own child process.
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec) /
                     ("anvil-bench-memory-kinds-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::fprintf(stderr, "anvil_bench: cannot create %s\n", dir.string().c_str());
        return 1;
    }
    std::vector<Kind> kinds(std::size(kKindNames));
    for (size_t i = 0; i < kinds.size(); ++i) {
        if (std::string error; !run_kind(kKindNames[i], n, dir, kinds[i], error)) {
            std::fprintf(stderr, "anvil_bench: %s\n", error.c_str());
            std::filesystem::remove_all(dir, ec);
            return 1;
        }
    }
    std::filesystem::remove_all(dir, ec);

    // --- JSON
    // The core budget counts commit: the start-up's private bytes (which
    // include the writable pages of the DLLs it loads) and the writable
    // sections of version.dll. Code and read-only data are file-backed and
    // shared; their size is reported, not budgeted.
    const double core_total = core.delta.private_bytes + image.writable_bytes;
    const bool core_within = image.found && core_total <= kCoreBudget;
    double module_images = 0;
    std::string modules;
    for (const auto& m : core.modules) {
        module_images += m.size_of_image;
        modules += std::string(modules.empty() ? "" : ", ") + "{\"name\": \"" + m.name +
                   "\", \"size_of_image\": " + json_number(m.size_of_image) +
                   ", \"writable_bytes\": " + json_number(m.writable_bytes) + "}";
    }
    std::string json = "{\n  \"format\": 1,\n  \"mode\": \"memory\",\n  \"anvil\": \"" ANVIL_VERSION "\",\n";
    json += "  \"unit\": \"bytes; private = commit charge (PrivateUsage), heap = busy heap blocks without headers, "
            "executable = committed private executable pages, reserved = private address space only reserved\",\n";
    json += "  \"core\": {" + json_snapshot(core.delta) + ", \"threads_added\": " + json_number(core.threads_added) +
            ", \"other_threads_added\": " + json_number(core.other_threads) +
            ", \"threads_exited\": " + json_number(core.threads_exited) + ", \"wait_ms\": " +
            json_number(core.wait_ms) + ", \"modules_loaded\": [" + modules + "]" +
            ", \"what\": \"anvil::startup() with the shipped anvil.toml and no plugins, in a child process, until the "
            "main-phase thread has ended\"},\n";
    if (image.found) {
        json += "  \"image\": {\"file\": \"" + image.path + "\", \"file_bytes\": " + json_number(image.file_bytes) +
                ", \"size_of_image\": " + json_number(image.size_of_image) +
                ", \"writable_bytes\": " + json_number(image.writable_bytes) + "},\n";
    } else {
        json += "  \"image\": null,\n";
    }
    json += "  \"changes\": " + std::to_string(n) + ",\n  \"kinds\": {\n";
    for (size_t i = 0; i < kinds.size(); ++i) {
        const Kind& k = kinds[i];
        json += "    \"" + k.name + "\": {\"per_change\": {\"private_bytes\": " + json_number(k.slope.private_bytes) +
                ", \"executable_bytes\": " + json_number(k.slope.executable_bytes) +
                ", \"other_bytes\": " + json_number(k.slope.private_bytes - k.slope.executable_bytes) +
                ", \"heap_bytes\": " + json_number(k.slope.heap_bytes) +
                ", \"heap_blocks\": " + json_number(k.slope.heap_blocks) +
                "}, \"fixed_private_bytes\": " + json_number(k.fixed_private) + ", \"total\": {\"private_bytes\": " +
                json_number(k.total.private_bytes) +
                ", \"executable_bytes\": " + json_number(k.total.executable_bytes) +
                ", \"heap_bytes\": " + json_number(k.total.heap_bytes) + "}, \"what\": \"" + k.what + "\"}" +
                (i + 1 < kinds.size() ? ",\n" : "\n");
    }
    json += "  },\n  \"budgets\": {\n";
    json += "    \"core\": {\"bytes\": " + json_number(core_total) + ", \"budget_bytes\": " +
            std::to_string(kCoreBudget) + ", \"within\": " + (core_within ? "true" : "false") +
            ", \"what\": \"private bytes of the start-up + writable sections of version.dll\"},\n";
    for (size_t i = 0; i < kinds.size(); ++i) {
        const Kind& k = kinds[i];
        json += "    \"" + k.name + "\": {\"bytes\": " + json_number(k.slope.private_bytes) + ", \"budget_bytes\": " +
                json_number(change_budget(k.name)) + ", \"within\": " +
                (k.slope.private_bytes <= change_budget(k.name) ? "true" : "false") +
                ", \"what\": \"private bytes per change (stubs included)\"}" + (i + 1 < kinds.size() ? ",\n" : "\n");
    }
    json += "  }\n}\n";
    if (out_path != nullptr) {
        FILE* f = nullptr;
        if (fopen_s(&f, out_path, "wb") != 0 || f == nullptr) {
            std::fprintf(stderr, "anvil_bench: cannot write %s\n", out_path);
            return 1;
        }
        std::fputs(json.c_str(), f);
        std::fclose(f);
    } else {
        std::fputs(json.c_str(), stdout);
    }

    // A short table on stderr for people.
    std::fprintf(stderr, "%-24s %12s %12s %12s %10s\n", "core (bytes)", "private", "heap", "executable", "budget");
    std::fprintf(stderr, "%-24s %12.0f %12.0f %12.0f\n", "start-up, no plugins", core.delta.private_bytes,
                 core.delta.heap_bytes, core.delta.executable_bytes);
    if (image.found) {
        std::fprintf(stderr, "%-24s %12.0f   (image %.0f, not budgeted)\n", "version.dll writable", image.writable_bytes,
                     image.size_of_image);
    } else {
        std::fprintf(stderr, "version.dll not found next to anvil_bench: the core budget is not checked\n");
    }
    std::fprintf(stderr, "%-24s %12.0f %12s %12s %10llu%s\n", "core total", core_total, "", "", kCoreBudget,
                 core_within ? "" : "  OVER");
    std::fprintf(stderr, "%-24s %12.0f   (%zu DLLs loaded by the start-up, not budgeted)\n", "their images",
                 module_images, core.modules.size());
    std::fprintf(stderr, "%-24s %12s %12s %12s %10s\n", "per change (bytes)", "private", "heap", "executable", "budget");
    for (const auto& k : kinds) {
        std::fprintf(stderr, "%-24s %12.1f %12.1f %12.1f %10.0f%s\n", k.name.c_str(), k.slope.private_bytes,
                     k.slope.heap_bytes, k.slope.executable_bytes, change_budget(k.name),
                     k.slope.private_bytes <= change_budget(k.name) ? "" : "  OVER");
    }
    return 0;
}
