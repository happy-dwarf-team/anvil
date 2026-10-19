/*
 * The CPU context of mid hooks and block replacements, word for word the
 * block of docs/api2/anvil_plugin.h. It is internal until the API 2 header
 * replaces include/anvil/anvil_plugin.h (#46); then this block moves there
 * unchanged and this file goes away. changes.cpp pins the layout against
 * SafetyHook's Context64 with static_asserts.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ANVIL_CPU_CONTEXT_H
#define ANVIL_CPU_CONTEXT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- mid hooks and block replacements --------------------------------------
 * Exactly SafetyHook's Context64 (408 bytes), pinned by static_asserts in Anvil.
 * Not saved: YMM upper halves, x87, MXCSR. Valid only during the callback. */
typedef union AnvilXmm {
    uint8_t u8[16];
    uint16_t u16[8];
    uint32_t u32[4];
    uint64_t u64[2];
    float f32[4];
    double f64[2];
} AnvilXmm;

typedef struct AnvilCpuContext {
    AnvilXmm xmm[16];
    uintptr_t rflags, r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rdx, rcx, rbx, rax, rbp;
    uintptr_t rsp;      /* read-only: the stack pointer at the site */
    uintptr_t reserved; /* do not touch */
    uintptr_t rip;      /* writable; in a mid hook a change ends the chain at this address */
} AnvilCpuContext;

typedef void (*AnvilCpuCallback)(AnvilCpuContext *cpu, void *user);

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_CPU_CONTEXT_H */
