/*
 * Anvil plugin SDK — the only header a plugin needs.
 *
 * Plugins are DLLs with a C ABI; any compiler that produces x64 Windows DLLs
 * works. Copy this file into the plugin repository; the host checks
 * ANVIL_API_VERSION (major) at load time and `size` fields for additions.
 *
 * Compatibility rules:
 *   - New functions/fields are only ever appended to the end of a struct;
 *     check `host->size` before using a field added after API 1.0.
 *   - A breaking change increments ANVIL_API_VERSION.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ANVIL_PLUGIN_H
#define ANVIL_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANVIL_API_VERSION 1

#if defined(_WIN32)
#define ANVIL_PLUGIN_EXPORT __declspec(dllexport)
#else
#define ANVIL_PLUGIN_EXPORT
#endif

typedef struct AnvilPlugin AnvilPlugin; /* opaque handle identifying the calling plugin */

typedef enum AnvilResult {
    ANVIL_OK = 0,
    ANVIL_ERROR_INVALID_ARGUMENT = 1,
    ANVIL_ERROR_BYTES_MISMATCH = 2, /* expected bytes/target at the address differ */
    ANVIL_ERROR_CONFLICT = 3,       /* overlaps a patch of another plugin */
    ANVIL_ERROR_OUT_OF_MEMORY = 4,
    ANVIL_ERROR_NOT_FOUND = 5,
    ANVIL_ERROR_UNSUPPORTED = 6,
    ANVIL_ERROR_WRONG_PHASE = 7,    /* call not allowed at this point of start-up */
    ANVIL_ERROR_ALREADY_EXISTS = 8,
    ANVIL_ERROR_INTERNAL = 9
} AnvilResult;

typedef enum AnvilLogLevel {
    ANVIL_LOG_DEBUG = 0,
    ANVIL_LOG_INFO = 1,
    ANVIL_LOG_WARNING = 2,
    ANVIL_LOG_ERROR = 3
} AnvilLogLevel;

/* Identity of the running game executable. */
typedef struct AnvilGameInfo {
    uint32_t size;               /* sizeof(AnvilGameInfo) */
    const char *build_id;        /* e.g. "eu4-1.37.5-steam-win64"; NULL when unknown */
    uintptr_t image_base;        /* address of eu4.exe in memory */
    uint32_t image_size;
    uint32_t pe_timestamp;
    uint32_t pe_checksum;
    uint32_t entry_point_rva;
    const char *file_version;    /* from the version resource, e.g. "1.37.5.0"; may be NULL */
    int ready_signal_known;      /* 1 when Anvil can publish ANVIL_EVENT_GAME_READY for this build */
} AnvilGameInfo;

/* Built-in events (publish/subscribe below). */
#define ANVIL_EVENT_PLUGINS_LOADED "anvil.plugins_loaded" /* all plugins processed; payload NULL */
#define ANVIL_EVENT_GAME_READY "anvil.game_ready"         /* game finished initialization; payload NULL */

typedef void (*AnvilEventCallback)(const char *event, const void *payload, void *context);

typedef struct AnvilHost {
    uint32_t size;               /* sizeof(AnvilHost) as known by the host */
    uint32_t api_version;        /* ANVIL_API_VERSION of the host */
    const char *anvil_version;   /* host version, SemVer */
    const AnvilGameInfo *game;

    /* --- environment --- */
    void (*log)(AnvilPlugin *self, AnvilLogLevel level, const char *message);
    const wchar_t *(*plugin_dir)(AnvilPlugin *self); /* read-only plugin folder */
    const wchar_t *(*data_dir)(AnvilPlugin *self);   /* writable folder, created on first call */

    /* --- code patching (all addresses are RVAs inside eu4.exe) ---
     * Every call verifies the expected bytes against the game's original
     * bytes, refuses overlaps with other changes at a different address (also
     * the plugin's own) and records the original bytes so the change can be
     * rolled back. Hooks, redirects and import replacements at the same
     * address chain in load order (the earlier plugin is called first);
     * identical byte patches are shared. Changes requested during
     * anvil_plugin_load are enabled together after it returns ANVIL_OK. */
    AnvilResult (*patch_bytes)(AnvilPlugin *self, uint32_t rva, const void *expected,
                               const void *replacement, size_t size);
    /* Retarget a 5-byte `call rel32` at call_rva (originally calling
     * expected_target_rva) to new_target, through a stub near the image. API 1
     * gives no next: a plugin that calls the original target itself skips
     * later plugins' redirects of the same call. */
    AnvilResult (*redirect_call)(AnvilPlugin *self, uint32_t call_rva, uint32_t expected_target_rva,
                                 void *new_target);
    /* Inline hook at the start of a function (SafetyHook inside Anvil).
     * expected_prologue: the first bytes of the function as the plugin knows them. */
    AnvilResult (*hook_function)(AnvilPlugin *self, uint32_t rva, const void *expected_prologue,
                                 size_t prologue_size, void *detour, void **original);
    /* Replace an import of eu4.exe (module name case-insensitive, e.g. "KERNEL32.dll"). */
    AnvilResult (*replace_import)(AnvilPlugin *self, const char *module, const char *function,
                                  void *replacement, void **original);
    /* Executable memory within +-2 GB of the image (for plugin-written stubs). */
    void *(*alloc_near_image)(AnvilPlugin *self, size_t size);

    /* --- services between plugins ---
     * A service is a struct of function pointers whose first field is
     * `uint32_t size`. Names are "<plugin-id>.<service>"; version is the
     * service's major version. A service lives as long as the process. */
    AnvilResult (*register_service)(AnvilPlugin *self, const char *name, uint32_t version,
                                    const void *service);
    const void *(*get_service)(AnvilPlugin *self, const char *name, uint32_t version);

    /* --- events --- */
    AnvilResult (*subscribe)(AnvilPlugin *self, const char *event, AnvilEventCallback callback,
                             void *context);
    AnvilResult (*publish)(AnvilPlugin *self, const char *event, const void *payload);

    /* --- added after API 1.0: check ANVIL_HOST_HAS(host, expect_bytes) --- */

    /* Read-only check that game code or data the plugin depends on (functions
     * it calls, vtables, globals) is what the plugin was written for. Compares
     * (actual & mask) with (expected & mask); mask NULL compares every bit, a
     * 0x00 mask byte skips that byte (e.g. relocated absolute addresses).
     * Bytes already patched by other plugins are compared in their original
     * form. Verify every dependency before the first patch: once the plugin
     * has patches of its own, the call returns ANVIL_ERROR_WRONG_PHASE.
     * Dependencies known in advance are better declared in symbols.toml:
     * Anvil checks those before the plugin DLL is loaded.
     * size: 1 .. ANVIL_MAX_EXPECT_BYTES. */
    AnvilResult (*expect_bytes)(AnvilPlugin *self, uint32_t rva, const void *expected, const void *mask,
                                size_t size);
} AnvilHost;

/* Largest range one expect_bytes call may compare. */
#define ANVIL_MAX_EXPECT_BYTES 65536

/* Nonzero when the host provides `field` (appended after API 1.0). */
#define ANVIL_HOST_HAS(host, field) \
    ((host)->size >= offsetof(AnvilHost, field) + sizeof((host)->field))

/*
 * Exported by every plugin. Called once:
 *   phase "early": at the game's entry point, before the C runtime of
 *                  eu4.exe initializes (only the main thread runs game code);
 *   phase "main":  on an Anvil thread, concurrently with game start-up.
 * Changes requested here are staged and enabled together after it returns.
 * Return ANVIL_OK to stay loaded; any other value unloads the plugin and
 * rolls back its patches (staged ones are never enabled).
 */
ANVIL_PLUGIN_EXPORT AnvilResult anvil_plugin_load(const AnvilHost *host, AnvilPlugin *self);

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_PLUGIN_H */
