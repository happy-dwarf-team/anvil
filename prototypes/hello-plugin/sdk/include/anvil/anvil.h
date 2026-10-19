/*
 * PROTOTYPE of the Anvil plugin API 2 C header. Throwaway: it shows the shape
 * decided in "Hook API 2" and "One source of truth for game addresses", not a
 * finished contract. Only the parts the hello plugin uses are here.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ANVIL_H
#define ANVIL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANVIL_API_VERSION 2
#define ANVIL_PLUGIN_EXPORT __declspec(dllexport)

typedef struct AnvilPlugin AnvilPlugin; /* the calling plugin */
typedef struct AnvilChange AnvilChange; /* one change; remove it with host->remove */
typedef uint32_t AnvilSymbol;           /* index of a declaration in the plugin's symbols.toml */

typedef enum AnvilResult {
    ANVIL_OK = 0,
    ANVIL_ERROR_INVALID_ARGUMENT,
    ANVIL_ERROR_NOT_DECLARED,   /* symbol not in symbols.toml, or a raw pointer into eu4.exe */
    ANVIL_ERROR_WRONG_KIND,     /* e.g. mid_hook on a symbol declared as a pointer slot */
    ANVIL_ERROR_CONFLICT,       /* overlaps another plugin's change at a different address */
    ANVIL_ERROR_UNSAFE_SITE,    /* stolen bytes contain JRCXZ / LOOP* */
    ANVIL_ERROR_UNSUPPORTED,
    ANVIL_ERROR_INTERNAL
} AnvilResult;

typedef enum AnvilLogLevel { ANVIL_LOG_DEBUG, ANVIL_LOG_INFO, ANVIL_LOG_WARNING, ANVIL_LOG_ERROR } AnvilLogLevel;

typedef struct AnvilGameInfo {
    uint32_t size;
    const char *build_id; /* "eu4-1.37.5-steam-win64" */
    uintptr_t image_base;
} AnvilGameInfo;

/* Mid-hook register context: the exact layout of SafetyHook's Context64 (408 bytes). */
typedef union AnvilXmm {
    uint8_t u8[16];
    uint32_t u32[4];
    uint64_t u64[2];
    float f32[4];
    double f64[2];
} AnvilXmm;

typedef struct AnvilCpuContext {
    AnvilXmm xmm[16];
    uintptr_t rflags, r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rdx, rcx, rbx, rax, rbp;
    uintptr_t rsp;      /* read-only: the stack pointer at the hook site */
    uintptr_t reserved; /* SafetyHook's trampoline_rsp; do not touch */
    uintptr_t rip;      /* writable: where execution resumes; changing it ends the chain */
} AnvilCpuContext;

typedef void (*AnvilMidHookFn)(AnvilCpuContext *cpu, void *user);

typedef struct AnvilHost {
    uint32_t size;
    uint32_t api_version;
    const char *anvil_version;
    const AnvilGameInfo *game;

    void (*log)(AnvilPlugin *self, AnvilLogLevel level, const char *message);
    const wchar_t *(*data_dir)(AnvilPlugin *self);

    /* Changes. Symbols come from the generated header; expected bytes come from
     * symbols.toml, so code never repeats them. During anvil_plugin_load every
     * change is staged and all are enabled together after it returns ANVIL_OK. */
    AnvilResult (*hook_function)(AnvilPlugin *self, AnvilSymbol symbol, void *detour, void **next, AnvilChange **out);
    AnvilResult (*mid_hook)(AnvilPlugin *self, AnvilSymbol symbol, AnvilMidHookFn callback, void *user, AnvilChange **out);
    AnvilResult (*redirect_call)(AnvilPlugin *self, AnvilSymbol symbol, void *target, void **next, AnvilChange **out);
    AnvilResult (*patch_bytes)(AnvilPlugin *self, AnvilSymbol symbol, const void *bytes, size_t size, AnvilChange **out);
    AnvilResult (*swap_pointer)(AnvilPlugin *self, AnvilSymbol symbol, void *value, void **previous, AnvilChange **out);
    AnvilResult (*replace_import)(AnvilPlugin *self, const char *module, const char *function, void *replacement,
                                  void **next, AnvilChange **out);
    /* Raw addresses: only outside the eu4.exe image (e.g. Direct3D 9 vtables). */
    AnvilResult (*hook_function_at)(AnvilPlugin *self, void *address, void *detour, void **next, AnvilChange **out);
    AnvilResult (*remove)(AnvilPlugin *self, AnvilChange *change);

    /* The verified address of a symbol, to call or read. */
    void *(*address_of)(AnvilPlugin *self, AnvilSymbol symbol);
} AnvilHost;

/* Exported by every plugin; called once in the phase its plugin.toml names. */
ANVIL_PLUGIN_EXPORT AnvilResult anvil_plugin_load(const AnvilHost *host, AnvilPlugin *self);

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_H */
