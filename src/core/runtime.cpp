#include "runtime.hpp"

#include "discovery.hpp"
#include "game_build.hpp"
#include "log.hpp"
#include "manifest.hpp"
#include "changes.hpp"
#include "plugin_symbols.hpp"
#include "resolver.hpp"
#include "seh_guard.hpp"
#include "services.hpp"
#include "symbol_check.hpp"

#include <anvil/anvil_plugin.h>
#include <windows.h>

#include <algorithm>
#include <map>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// Opaque handle given to plugins.
struct AnvilPlugin {
    anvil::Manifest manifest;
    anvil::OwnerId owner = 0;      // in the change registry; ids follow the load order
    anvil::PluginSymbols symbols;  // verified symbols.toml, by AnvilSymbol
    std::wstring plugin_dir;
    std::wstring data_dir;
    HMODULE module = nullptr;
    std::string state = "pending";  // pending | loaded | failed
};

namespace anvil {
namespace {

struct Runtime {
    std::filesystem::path game_dir;
    std::filesystem::path anvil_dir;
    std::filesystem::path exe_path;
    PeIdentity identity;
    const KnownBuild* build = nullptr;
    AnvilGameInfo game{};
    std::unique_ptr<ChangeRegistry> changes;
    ServiceBus bus;
    std::vector<std::unique_ptr<AnvilPlugin>> plugins;  // resolved, in load order
    std::vector<Skipped> skipped;
    LARGE_INTEGER start{};
    LARGE_INTEGER frequency{};
};

Runtime* g_runtime = nullptr;

double elapsed_ms() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - g_runtime->start.QuadPart) * 1000.0 /
           static_cast<double>(g_runtime->frequency.QuadPart ? g_runtime->frequency.QuadPart : 1);
}

std::string fmt_ms(double ms) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f ms", ms);
    return buffer;
}

const char* result_name(AnvilResult r) {
    switch (r) {
    case ANVIL_OK: return "ok";
    case ANVIL_ERROR_INVALID_ARGUMENT: return "invalid argument";
    case ANVIL_ERROR_BYTES_MISMATCH: return "bytes mismatch";
    case ANVIL_ERROR_CONFLICT: return "conflict with another plugin";
    case ANVIL_ERROR_OUT_OF_MEMORY: return "out of memory";
    case ANVIL_ERROR_NOT_FOUND: return "not found";
    case ANVIL_ERROR_UNSUPPORTED: return "unsupported";
    case ANVIL_ERROR_WRONG_PHASE: return "wrong phase";
    case ANVIL_ERROR_ALREADY_EXISTS: return "already exists";
    case ANVIL_ERROR_INTERNAL: return "internal error";
    }
    return "unknown";
}

// API 1 has no codes for some refusals of the change registry; the closest
// API 1 meaning is used (WrongKind: the bytes at the site are not what the call
// needs). API 2 (#46) maps every status one to one.
AnvilResult api1_result(ChangeStatus status) {
    switch (status) {
    case ChangeStatus::Ok: return ANVIL_OK;
    case ChangeStatus::InvalidArgument:
    case ChangeStatus::NotDeclared: return ANVIL_ERROR_INVALID_ARGUMENT;
    case ChangeStatus::WrongKind:
    case ChangeStatus::BytesMismatch: return ANVIL_ERROR_BYTES_MISMATCH;
    case ChangeStatus::Conflict: return ANVIL_ERROR_CONFLICT;
    case ChangeStatus::UnsafeSite: return ANVIL_ERROR_UNSUPPORTED;
    case ChangeStatus::OutOfMemory: return ANVIL_ERROR_OUT_OF_MEMORY;
    case ChangeStatus::WrongPhase: return ANVIL_ERROR_WRONG_PHASE;
    case ChangeStatus::NotFound: return ANVIL_ERROR_NOT_FOUND;
    case ChangeStatus::Internal: return ANVIL_ERROR_INTERNAL;
    }
    return ANVIL_ERROR_INTERNAL;
}

// ---- AnvilHost implementation -------------------------------------------

bool valid(AnvilPlugin* self) { return self != nullptr && g_runtime != nullptr; }

void host_log(AnvilPlugin* self, AnvilLogLevel level, const char* message) {
    if (message == nullptr) return;
    Log::instance().write(level, valid(self) ? self->manifest.id : "?", message);
}

const wchar_t* host_plugin_dir(AnvilPlugin* self) { return valid(self) ? self->plugin_dir.c_str() : nullptr; }

const wchar_t* host_data_dir(AnvilPlugin* self) {
    if (!valid(self)) return nullptr;
    std::error_code ignored;
    std::filesystem::create_directories(self->data_dir, ignored);
    return self->data_dir.c_str();
}

uintptr_t rva_to_address(uint32_t rva) { return g_runtime->identity.image_base + rva; }

AnvilResult note(AnvilPlugin* self, const char* what, AnvilResult r) {
    if (r != ANVIL_OK) {
        Log::instance().write(ANVIL_LOG_WARNING, self->manifest.id, std::string(what) + " refused: " + result_name(r));
    }
    return r;
}

// API 1 change calls, bridged to the change registry until the API 2 header
// (#46) replaces them with symbol calls (plugin_symbols.hpp). An RVA is a
// target inside eu4.exe whose expected bytes the plugin passes itself; they are
// compared with the original bytes, so a plugin can join another plugin's chain.
ChangeTarget rva_target(uint32_t rva, const void* expected, size_t size) {
    ChangeTarget t;
    t.address = rva_to_address(rva);
    t.inside_image = true;
    t.expected = static_cast<const uint8_t*>(expected);
    t.expected_size = expected != nullptr ? size : 0;
    return t;
}

AnvilResult host_patch_bytes(AnvilPlugin* self, uint32_t rva, const void* expected, const void* replacement, size_t size) {
    if (!valid(self) || expected == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    return note(self, "patch_bytes",
                api1_result(g_runtime->changes->patch_bytes(self->owner, rva_target(rva, expected, size), replacement,
                                                            size)));
}

AnvilResult host_redirect_call(AnvilPlugin* self, uint32_t call_rva, uint32_t expected_target_rva, void* new_target) {
    if (!valid(self)) return ANVIL_ERROR_INVALID_ARGUMENT;
    void* next = nullptr;  // API 1 has no next: the plugin calls the old target itself
    return note(self, "redirect_call",
                api1_result(g_runtime->changes->redirect_call(self->owner, rva_target(call_rva, nullptr, 0),
                                                              rva_to_address(expected_target_rva), new_target, &next)));
}

AnvilResult host_hook_function(AnvilPlugin* self, uint32_t rva, const void* expected, size_t size, void* detour,
                               void** original) {
    if (!valid(self) || expected == nullptr || size == 0 || size > 64 || original == nullptr) {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    return note(self, "hook_function",
                api1_result(g_runtime->changes->hook_function(self->owner, rva_target(rva, expected, size), detour,
                                                              original)));
}

AnvilResult host_replace_import(AnvilPlugin* self, const char* module, const char* function, void* replacement,
                                void** original) {
    if (!valid(self)) return ANVIL_ERROR_INVALID_ARGUMENT;
    void* next = nullptr;
    const AnvilResult r = api1_result(g_runtime->changes->replace_import(self->owner, GetModuleHandleW(nullptr), module,
                                                                         function, replacement, &next));
    if (r == ANVIL_OK && original != nullptr) *original = next;
    return note(self, "replace_import", r);
}

AnvilResult host_expect_bytes(AnvilPlugin* self, uint32_t rva, const void* expected, const void* mask, size_t size) {
    if (!valid(self)) return ANVIL_ERROR_INVALID_ARGUMENT;
    std::string detail;
    const AnvilResult r = api1_result(
        g_runtime->changes->expect_bytes(self->owner, rva_to_address(rva), expected, mask, size, &detail));
    if (r == ANVIL_ERROR_BYTES_MISMATCH) {
        char where[48];
        std::snprintf(where, sizeof(where), "rva 0x%X, %zu B", rva, size);
        Log::instance().write(ANVIL_LOG_WARNING, self->manifest.id,
                              std::string("expect_bytes mismatch at ") + where + ", " + detail);
        return r;
    }
    return note(self, "expect_bytes", r);
}

void* host_alloc_near_image(AnvilPlugin* self, size_t size) {
    return valid(self) ? g_runtime->changes->alloc_near(size) : nullptr;
}

AnvilResult host_register_service(AnvilPlugin* self, const char* name, uint32_t version, const void* service) {
    if (!valid(self) || name == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    return note(self, "register_service", g_runtime->bus.register_service(self->manifest.id, name, version, service));
}

const void* host_get_service(AnvilPlugin* self, const char* name, uint32_t version) {
    if (!valid(self) || name == nullptr) return nullptr;
    return g_runtime->bus.get_service(name, version);
}

AnvilResult host_subscribe(AnvilPlugin* self, const char* event, AnvilEventCallback callback, void* context) {
    if (!valid(self) || event == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    return g_runtime->bus.subscribe(self->manifest.id, event, callback, context);
}

AnvilResult host_publish(AnvilPlugin* self, const char* event, const void* payload) {
    if (!valid(self) || event == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (std::string_view(event).starts_with("anvil.")) return ANVIL_ERROR_INVALID_ARGUMENT;  // reserved
    g_runtime->bus.publish(event, payload);
    return ANVIL_OK;
}

const AnvilHost kHost = {
    sizeof(AnvilHost),
    ANVIL_API_VERSION,
    kAnvilVersion,
    nullptr,  // game: patched in startup()
    host_log,
    host_plugin_dir,
    host_data_dir,
    host_patch_bytes,
    host_redirect_call,
    host_hook_function,
    host_replace_import,
    host_alloc_near_image,
    host_register_service,
    host_get_service,
    host_subscribe,
    host_publish,
    host_expect_bytes,
};
AnvilHost g_host = kHost;

// ---- loading ---------------------------------------------------------------

using LoadFn = PluginLoadFn;

void load_plugin(AnvilPlugin& plugin) {
    const double begin = elapsed_ms();
    const std::filesystem::path dll = std::filesystem::path(plugin.plugin_dir) / plugin.manifest.entry;
    const std::string label = plugin.manifest.id + " " + plugin.manifest.version.to_string();
    plugin.module = LoadLibraryExW(dll.c_str(), nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (plugin.module == nullptr) {
        plugin.state = "failed";
        log_error(label + ": cannot load " + narrow(dll.wstring()) + " (error " + std::to_string(GetLastError()) + ")");
        return;
    }
    const auto fn = reinterpret_cast<LoadFn>(GetProcAddress(plugin.module, "anvil_plugin_load"));
    if (fn == nullptr) {
        plugin.state = "failed";
        log_error(label + ": entry DLL does not export anvil_plugin_load");
        FreeLibrary(plugin.module);
        plugin.module = nullptr;
        return;
    }
    // Changes requested during anvil_plugin_load are staged and enabled together
    // after it returns ANVIL_OK; on failure none are (design §8.4).
    ChangeRegistry& changes = *g_runtime->changes;
    changes.begin_group(plugin.owner);
    unsigned long exception = 0;
    AnvilResult result = call_plugin_load(fn, &g_host, &plugin, &exception);
    const size_t staged = changes.staged_count(plugin.owner);
    if (result == ANVIL_OK && changes.commit_group(plugin.owner) != ChangeStatus::Ok) {
        log_error(label + ": its " + std::to_string(staged) + " change(s) could not be enabled");
        result = ANVIL_ERROR_INTERNAL;
    }
    if (result != ANVIL_OK) {
        plugin.state = "failed";
        changes.rollback(plugin.owner);
        g_runtime->bus.remove_owner(plugin.manifest.id);
        char code[32] = {};
        if (exception != 0) std::snprintf(code, sizeof(code), "0x%08lX", exception);
        log_error(label + ": anvil_plugin_load " +
                  (exception != 0 ? std::string("crashed with exception ") + code : std::string("returned ") + result_name(result)) +
                  "; " + std::to_string(staged) + " change(s) rolled back; plugin stays inactive");
        // The DLL is not unloaded: threads it may have started could still run.
        return;
    }
    plugin.state = "loaded";
    std::string summary = label + ": loaded (" + (plugin.manifest.phase == Phase::Early ? "early" : "main") + ", " +
                          fmt_ms(elapsed_ms() - begin) + ", " + std::to_string(changes.count(plugin.owner)) +
                          " change(s))";
    if (plugin.manifest.affects_gameplay) summary += " — declares gameplay changes";
    log_info(summary);
    for (const auto& line : changes.describe(plugin.owner)) {
        Log::instance().write(ANVIL_LOG_DEBUG, plugin.manifest.id, line);
    }
}

void write_summary() {
    size_t loaded = 0, failed = 0;
    for (const auto& p : g_runtime->plugins) {
        if (p->state == "loaded") ++loaded;
        else if (p->state == "failed") ++failed;
    }
    log_info("status: " + std::to_string(loaded) + " loaded, " + std::to_string(failed) + " failed, " +
             std::to_string(g_runtime->skipped.size()) + " skipped");
    for (const auto& s : g_runtime->bus.describe()) log_info("service " + s);
}

DWORD WINAPI ready_watcher(LPVOID) {
    const auto* marker = reinterpret_cast<const volatile uint8_t*>(rva_to_address(g_runtime->build->ready_marker_rva));
    for (int i = 0; i < 20 * 60 * 30; ++i) {  // up to 30 minutes
        if (*marker == 1) {
            log_info("game ready at " + fmt_ms(elapsed_ms()));
            g_runtime->bus.publish(ANVIL_EVENT_GAME_READY, nullptr);
            return 0;
        }
        Sleep(50);
    }
    log_warning("game ready signal not observed within 30 minutes");
    return 0;
}

DWORD WINAPI main_phase(LPVOID) {
    for (auto& p : g_runtime->plugins) {
        if (p->manifest.phase == Phase::Main) load_plugin(*p);
    }
    g_runtime->bus.publish(ANVIL_EVENT_PLUGINS_LOADED, nullptr);
    write_summary();
    if (g_runtime->build != nullptr) {
        std::string actual;
        const bool ok = verify_build_file(*g_runtime->build, g_runtime->exe_path,
                                          g_runtime->anvil_dir / "data" / "anvil" / "build-cache.txt", actual);
        if (ok) {
            log_info("eu4.exe SHA-256 verified");
        } else {
            log_warning("eu4.exe SHA-256 " + (actual.empty() ? std::string("could not be computed") : actual) +
                        " differs from the known build; every change is still checked against expected bytes");
        }
    }
    return 0;
}

void startup_impl(const std::filesystem::path& game_dir) {
    auto runtime = std::make_unique<Runtime>();
    QueryPerformanceFrequency(&runtime->frequency);
    QueryPerformanceCounter(&runtime->start);
    runtime->game_dir = game_dir;
    runtime->anvil_dir = game_dir / "anvil";
    g_runtime = runtime.release();  // lives for the process

    if (!std::filesystem::is_directory(g_runtime->anvil_dir)) return;  // not installed: pure proxy, no files
    Log::instance().open(g_runtime->anvil_dir / "anvil.log");
    log_info(std::string("Anvil ") + kAnvilVersion + " (plugin API " + std::to_string(kManifestApi) + ")");

    wchar_t disable[8] = {};
    if (GetEnvironmentVariableW(L"ANVIL_DISABLE", disable, 8) == 1 && disable[0] == L'1') {
        log_info("ANVIL_DISABLE=1: no plugins loaded");
        return;
    }
    const AnvilSettings settings = read_anvil_settings(g_runtime->anvil_dir / "anvil.toml");
    if (!settings.error.empty()) log_warning("anvil.toml ignored: " + settings.error);
    Log::instance().set_min_level(settings.log_level);
    if (!settings.enabled) {
        log_info("disabled in anvil.toml: no plugins loaded");
        return;
    }

    HMODULE exe = GetModuleHandleW(nullptr);
    wchar_t exePath[MAX_PATH * 2] = {};
    GetModuleFileNameW(exe, exePath, static_cast<DWORD>(std::size(exePath)));
    g_runtime->exe_path = exePath;
    const auto identity = read_pe_identity(exe);
    if (!identity) {
        log_error("cannot read the game executable header; no plugins loaded");
        return;
    }
    g_runtime->identity = *identity;
    g_runtime->build = find_known_build(*identity);
    g_runtime->changes = std::make_unique<ChangeRegistry>(identity->image_base, identity->size_of_image);
    g_runtime->changes->set_refusal_sink([](OwnerId owner, const std::string& message) {
        // Names both owners of a conflict; the host call adds its own line.
        // Owner ids are the indices of g_runtime->plugins (both in load order).
        const auto& plugins = g_runtime->plugins;
        Log::instance().write(ANVIL_LOG_WARNING, owner < plugins.size() ? plugins[owner]->manifest.id : "?", message);
    });
    AnvilGameInfo& game = g_runtime->game;
    game.size = sizeof(AnvilGameInfo);
    game.build_id = g_runtime->build ? g_runtime->build->id : nullptr;
    game.image_base = identity->image_base;
    game.image_size = identity->size_of_image;
    game.pe_timestamp = identity->pe_timestamp;
    game.pe_checksum = identity->pe_checksum;
    game.entry_point_rva = identity->entry_point_rva;
    game.file_version = nullptr;
    game.ready_signal_known = g_runtime->build != nullptr && g_runtime->build->ready_marker_rva != 0;
    g_host.game = &game;
    char header[160];
    std::snprintf(header, sizeof(header), "timestamp 0x%08X, image 0x%X, checksum 0x%08X, entry 0x%X",
                  identity->pe_timestamp, identity->size_of_image, identity->pe_checksum, identity->entry_point_rva);
    log_info(std::string("game build: ") + (game.build_id ? game.build_id : "unknown") + " (" + header + ")");

    // Discovery: anvil/plugins/<folder>/plugin.toml
    ResolveInput input;
    input.host_api = kManifestApi;
    input.host_version = Version::parse(kAnvilVersion);
    input.game_build = game.build_id;
    input.disabled = settings.disabled_plugins;
    // symbols.toml: its build and every symbol are checked here, before any
    // plugin is loaded. The image is read through the registry, so bytes a
    // plugin has already changed read in their original form (design §7).
    // The verified symbols are kept per plugin folder for address_of and the
    // symbol change calls.
    std::map<std::wstring, PluginSymbols> verified_symbols;
    input.verify = [&](const Manifest& m) {
        if (m.symbols.empty()) return std::string();
        const auto file = m.folder / m.symbols;
        SymbolVerification verified;
        SymbolFile declared;
        std::string reason = check_plugin_symbols(file, game.build_id, *g_runtime->changes, identity->image_base,
                                                  &verified, &declared);
        if (!reason.empty()) return reason;
        verified_symbols[m.folder.wstring()] = PluginSymbols(declared, verified, identity->image_base);
        log_info(m.id + ": " + std::to_string(verified.symbols.size()) + " symbol(s) verified");
        return std::string();
    };
    const auto plugins_dir = g_runtime->anvil_dir / "plugins";
    const std::vector<std::filesystem::path> folders = plugin_folders(plugins_dir);
    for (const auto& folder : folders) {
        const auto parsed = load_manifest(folder);
        if (!parsed.manifest) {
            g_runtime->skipped.push_back({narrow(folder.filename().wstring()), parsed.error, folder});
            continue;
        }
        if (std::string reason = missing_entry_reason(*parsed.manifest); !reason.empty()) {
            g_runtime->skipped.push_back({parsed.manifest->id, std::move(reason), folder});
            continue;
        }
        input.manifests.push_back(*parsed.manifest);
    }
    log_info("discovered " + std::to_string(folders.size()) + " plugin folder(s) in " + narrow(plugins_dir.wstring()));
    ResolveResult resolved = resolve(input);
    g_runtime->skipped.insert(g_runtime->skipped.end(), resolved.skipped.begin(), resolved.skipped.end());
    for (const auto& s : g_runtime->skipped) log_warning(s.id + ": skipped — " + s.reason);
    for (auto& m : resolved.order) {
        auto plugin = std::make_unique<AnvilPlugin>();
        // Owners in load order: the earlier plugin is outermost in every chain.
        plugin->owner = g_runtime->changes->add_owner(m.id);
        if (const auto it = verified_symbols.find(m.folder.wstring()); it != verified_symbols.end()) {
            plugin->symbols = std::move(it->second);
        }
        plugin->plugin_dir = m.folder.wstring();
        plugin->data_dir = (g_runtime->anvil_dir / "data" / m.id).wstring();
        plugin->manifest = std::move(m);
        g_runtime->plugins.push_back(std::move(plugin));
    }

    // Early plugins: here, before the game's runtime starts.
    for (auto& p : g_runtime->plugins) {
        if (p->manifest.phase == Phase::Early) load_plugin(*p);
    }
    log_info("early phase done at " + fmt_ms(elapsed_ms()));

    if (game.ready_signal_known) {
        if (HANDLE t = CreateThread(nullptr, 0, ready_watcher, nullptr, 0, nullptr)) CloseHandle(t);
    }
    if (HANDLE t = CreateThread(nullptr, 0, main_phase, nullptr, 0, nullptr)) {
        CloseHandle(t);
    } else {
        log_error("cannot start the main-phase thread; main plugins not loaded");
    }
}

}  // namespace

void startup(const std::filesystem::path& game_dir) {
    try {
        startup_impl(game_dir);
    } catch (const std::exception& e) {
        log_error(std::string("startup aborted: ") + e.what());
    } catch (...) {
        log_error("startup aborted by an unknown exception");
    }
}

}  // namespace anvil
