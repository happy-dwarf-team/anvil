# SafetyHook mid-hooks and hook chaining: how they work and what they cost

Research for [#14](https://github.com/happy-dwarf-team/anvil/issues/14)
(map [#12](https://github.com/happy-dwarf-team/anvil/issues/12)). Question: how
do SafetyHook 0.7 mid-function hooks and several hooks on one function work,
and what do they cost? The answers decide the hook kinds and the chaining
policy of API 2.

Each statement is labelled:

- **Verified**: read in the primary source (link given) or measured here.
- **Derived**: follows from the cited code by reading it; not executed.
- **Estimate**: reasoning about cost; no measurement behind it.
- **Recommendation**: a proposal for Anvil, not a fact.

Sources, pinned:

- SafetyHook v0.7.0 = commit
  [`1922366`](https://github.com/cursey/safetyhook/tree/19223663fb8a573253ffb2e82da87cc354bf5c16)
  (tag date 2026-06-15), the version Anvil fetches (`CMakeLists.txt`,
  `GIT_TAG v0.7.0`). Links below abbreviate it as `SH/<path>#L<n>`; the
  full form is `https://github.com/cursey/safetyhook/blob/19223663fb8a573253ffb2e82da87cc354bf5c16/<path>#L<n>`.
- MinHook master
  [`8af6b4a`](https://github.com/TsudaKageyu/minhook/tree/8af6b4acae5a9388fd742b56fa79ece89d96f823)
  (abbreviated `MH/<path>#L<n>`).
- Zydis instruction database
  [zydis-db `eaf92a0`](https://github.com/zyantific/zydis-db/tree/eaf92a02dca616b90c12b2ebc338d87ed943c84f)
  (the tables Zydis 4.1.0 is generated from; master, not the 4.1.0 snapshot).
- Microsoft x64 ABI:
  [x64 software conventions](https://learn.microsoft.com/en-us/cpp/build/x64-software-conventions)
  and [x64 calling convention](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention).

## 1. Answer in short

1. A `MidHook` is an ordinary `InlineHook` whose destination is a 391-byte
   per-hook stub. The stub saves 16 GPRs, RFLAGS and XMM0–15 into a
   408-byte `Context64` on the stack, calls `void(Context64&)`, restores
   everything and returns into the trampoline. Every GPR, RFLAGS and XMM0–15
   can be modified; `rsp` cannot (only `trampoline_rsp`); `rip` can be
   changed to resume elsewhere. YMM/ZMM upper halves, x87 and MXCSR are not
   saved. (Verified, §2.)
2. The layout is a plain struct of `uint8_t`/`uintptr_t` fields with no
   padding, but SafetyHook does not promise it as an ABI. It changed in
   v0.1.2 and v0.2.0 and has been unchanged since v0.2.0 (2023-12-16). Anvil
   should own a C struct with the same layout, pinned with `static_assert`s.
   (Verified history; recommendation.)
3. Relocation: whole instructions covering at least 5 bytes (E9 mode) are
   copied. RIP-relative `disp32` and `rel32` operands are re-encoded, and
   short `Jcc`/`JMP` are widened. In the 14-byte fallback mode no
   IP-relative instruction is allowed. `JRCXZ`/`LOOP*` in the stolen bytes
   are mis-encoded. (Verified code; the mis-encoding is derived, §3.)
4. Two hooks on the same address stack: the second hook relocates the
   first hook's `E9` jump, so the newest hook runs first. SafetyHook has no
   registry, so hooks on overlapping but different addresses are not
   detected and produce garbage. Removal must be LIFO: removing an older
   hook first silently unhooks the newer ones, and later removals then
   resurrect a jump to freed memory. (Verified test of LIFO stacking; the
   out-of-order failure is derived, §4.)
5. Thread safety since v0.5.0 is page trapping, not freezing. The pages are
   made non-executable, a vectored exception handler makes faulting threads
   spin or moves them to `trampoline + i`, and then the bytes are written.
   The README still says "Stops all other threads", which no longer
   describes the code. `InlineHook::call()` takes a `recursive_mutex` for
   the whole original call; Anvil already bypasses it by handing out the raw
   trampoline. (Verified, §5.)
6. Cost, measured on an i9-13900KF (indicative, §6) over a 1.6 ns baseline
   call: an inline hook with a raw trampoline call adds about 1 ns, each
   stacked inline hook another 1 ns, `call()` with the mutex about 10 ns, an
   empty mid hook about 21 ns, and each further stacked mid hook about 17 ns.

## 2. MidHook context

### Verified

- `MidHook::setup` copies the stub bytes into memory from the allocator,
  stores the destination pointer and the trampoline address into the last
  16 bytes, and creates an `InlineHook` from the target to the stub
  ([SH/src/mid_hook.cpp#L115-L156](https://github.com/cursey/safetyhook/blob/19223663fb8a573253ffb2e82da87cc354bf5c16/src/mid_hook.cpp#L115-L156)).
  So a mid hook inherits every relocation rule and limit of `InlineHook`
  (§3) and can sit at any instruction boundary, not only at a function
  start.
- Layout of `Context64`
  ([SH/include/safetyhook/context.hpp#L30-L33](https://github.com/cursey/safetyhook/blob/19223663fb8a573253ffb2e82da87cc354bf5c16/include/safetyhook/context.hpp#L30-L33)):
  `Xmm xmm0..xmm15` (16-byte unions u8/u16/u32/u64/f32/f64), then
  `uintptr_t rflags, r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rdx,
  rcx, rbx, rax, rbp, rsp, trampoline_rsp, rip`. That is 256 + 19 × 8 =
  **408 bytes**. `rflags` is at offset 256 and `rsp` at 384, which matches
  the stub's `[rsp+384]` fix-up.
- The Windows x64 stub
  ([SH/src/mid_hook.x86_64-windows.asm](https://github.com/cursey/safetyhook/blob/19223663fb8a573253ffb2e82da87cc354bf5c16/src/mid_hook.x86_64-windows.asm)):
  - It pushes `[rel trampoline]` (this becomes `rip`), then `rsp` twice,
    then the 15 GPRs, `pushfq`, and stores XMM0–15 with `movdqu`. The second
    `rsp` slot is corrected by +16 so that `ctx.rsp` is the value at the
    hook site (L4–L44).
  - It passes `rcx = &ctx`, saves `rsp` in `rbx`, reserves 48 bytes, aligns
    to 16 (32 bytes of shadow space plus alignment), and does
    `call [rel destination]` (L46–L55).
  - It reloads XMM0–15, `popfq`, and pops the GPRs. `lea rsp,[rsp+8]`
    skips `ctx.rsp`, `pop rsp` loads `ctx.trampoline_rsp`, and `ret` jumps
    to the qword stored there, the `rip` slot (L60–L96).
- The header documents `rsp` as read-only and tells you to move
  `trampoline_rsp` instead, keeping the resume `rip` on top
  ([SH/include/safetyhook/context.hpp#L26-L29](https://github.com/cursey/safetyhook/blob/19223663fb8a573253ffb2e82da87cc354bf5c16/include/safetyhook/context.hpp#L26-L29)).
  The header also says the context "only provides access to integer
  registers" (L50), which is outdated: the XMM registers were added later.
- Tests modify `rcx` and `xmm0.f32[0]` in a mid hook and observe the change
  ([SH/test/mid_hook.cpp#L7-L73](https://github.com/cursey/safetyhook/blob/19223663fb8a573253ffb2e82da87cc354bf5c16/test/mid_hook.cpp#L7-L73)).
- History of `context.hpp`: XMM0–15 and `rip` were added in
  [`39dff2c`](https://github.com/cursey/safetyhook/commit/39dff2cdfeb62869b032e23e0dac3820f1365db3)
  (first in v0.1.2, 2023-10-04), and `trampoline_rsp` was inserted before
  `rip` in
  [`56f7886`](https://github.com/cursey/safetyhook/commit/56f7886539450617b19dfc9a849398a6b32f0981)
  (first in v0.2.0, 2023-12-16). Later commits touched only preprocessor and
  module glue, not the fields.
- Stub, trampoline and other allocator memory is committed as
  `PAGE_EXECUTE_READWRITE`
  ([SH/src/allocator.cpp#L189](https://github.com/cursey/safetyhook/blob/19223663fb8a573253ffb2e82da87cc354bf5c16/src/allocator.cpp#L189),
  [L201](https://github.com/cursey/safetyhook/blob/19223663fb8a573253ffb2e82da87cc354bf5c16/src/allocator.cpp#L201)).

### Derived

- **Modifiable:** all 15 GPRs except `rsp`, RFLAGS and XMM0–15 (all are
  reloaded). `ctx.rip` starts as the trampoline address; overwriting it
  makes `ret` resume at another address, which can be used to skip code.
  `ctx.rsp` is ignored on the way out.
- **Not saved:** YMM/ZMM upper halves, x87/MMX state, MXCSR, AVX-512 mask
  registers. The Microsoft ABI makes YMM upper halves volatile across calls.
  A destination that executes AVX code, including CRT routines such as
  `memcpy` that may pick AVX paths, can therefore clobber live YMM upper
  halves at the hook site if the game holds any there. Whether EU4 1.37.5
  keeps live YMM state anywhere is **unknown** (not checked).
- The ABI requires DF = 0 at a call. The stub calls the destination with
  whatever RFLAGS the hook site had, so DF = 1 at a hook site (rare,
  string-op code) would break that rule.
- The context pointer refers to the hooking thread's stack and is valid
  only during the callback.
- Layout stability: the struct is POD with natural alignment and no padding,
  so a C mirror with the same field order is binary-identical under MSVC,
  clang-cl and MinGW. It is tied to the hand-assembled stub, not to a
  promised ABI.

### Recommendation

- Define `AnvilCpuContext` in `anvil_plugin.h` with exactly these 35 fields
  in this order. In Anvil, `static_assert` `sizeof == 408` and every
  `offsetof` against `safetyhook::Context64`, and pass the SafetyHook
  pointer through unchanged (no copy). If a future SafetyHook changes the
  layout, Anvil ships its own copy of the 391-byte stub. The stub is
  self-contained and the C ABI stays fixed.
- Document: `rsp` read-only, `rip` writable (resume address), and do not
  touch `trampoline_rsp` unless you know why (or leave it out of the
  documented fields). Also: no YMM/x87/MXCSR, keep the callback
  non-blocking and SSE-only, and the pointer is valid only during the call.

## 3. Instruction relocation and limits

### Verified (`InlineHook::e9_hook` / `ff_hook`, [SH/src/inline_hook.cpp](https://github.com/cursey/safetyhook/blob/19223663fb8a573253ffb2e82da87cc354bf5c16/src/inline_hook.cpp))

- **E9 mode (preferred):** whole instructions are decoded with Zydis until
  at least 5 bytes are covered (L194–L229). The trampoline is allocated
  within ±2 GB (`0x7FFF'FFFF`) of the target and of every RIP-relative or
  rel32 destination found
  ([SH/include/safetyhook/allocator.hpp#L95-L96](https://github.com/cursey/safetyhook/blob/19223663fb8a573253ffb2e82da87cc354bf5c16/include/safetyhook/allocator.hpp#L95-L96)).
  Relocation rules:
  - any relative instruction with a 32-bit displacement (`lea rax,[rip+x]`,
    `mov`, `cmp`, …) is copied and its `disp32` recomputed;
  - a relative instruction with a 32-bit immediate (`call/jmp/jcc rel32`)
    is copied and the immediate recomputed;
  - a short `Jcc` becomes `0F 8x rel32`;
  - a short `JMP` becomes `E9 rel32`;
  - when a short branch lands inside the stolen bytes, its original
    displacement is kept;
  - anything else that is relative fails with
    `UNSUPPORTED_INSTRUCTION_IN_TRAMPOLINE` (L240–L289).

  The trampoline ends with `E9` back to `target + stolen` and an
  `FF 25 [rip+0]` absolute jump to the destination.
- The target is patched with `E9` to the trampoline's `FF 25` jump, so the
  hook entry is two jumps. Leftover stolen bytes are filled with `NOP`
  (L383–L403, L85–L98).
- **FF mode (fallback when nothing can be allocated within ±2 GB):** it
  steals at least 14 bytes (`FF 25` + 8-byte address written at the target)
  and refuses any IP-relative instruction in them
  (`IP_RELATIVE_INSTRUCTION_OUT_OF_RANGE`, L324–L366).
- Tests cover RIP-relative operands, all 16 short `Jcc`s, short `JMP`, a
  short jump inside the trampoline and "no nearby memory"
  ([SH/test/inline_hook.cpp](https://github.com/cursey/safetyhook/blob/19223663fb8a573253ffb2e82da87cc354bf5c16/test/inline_hook.cpp),
  [SH/test/inline_hook.x86_64.cpp](https://github.com/cursey/safetyhook/blob/19223663fb8a573253ffb2e82da87cc354bf5c16/test/inline_hook.x86_64.cpp)).
- Zydis classifies `jrcxz`/`jecxz` (E3), `loop` (E2), `loope` (E1) and
  `loopne` (E0) as `COND_BR` with rel8 operands (zydis-db
  `datafiles/instructions.json`). Comparison: MinHook rejects these
  explicitly, saying "LOOPNZ/LOOPZ/LOOP/JCXZ/JECXZ to the outside are not
  supported"
  ([MH/src/trampoline.c#L236-L239](https://github.com/TsudaKageyu/minhook/blob/8af6b4acae5a9388fd742b56fa79ece89d96f823/src/trampoline.c#L236-L239)).

### Derived

- SafetyHook widens every short `COND_BR` as `0F, 0x10 + opcode`
  (L270). That is correct for `70–7F`. For `E0–E3` it emits `0F F0..F3`,
  which encodes an unrelated instruction (for example `0F F3` is
  `psllq`). A `JRCXZ`/`LOOP` in the stolen bytes is therefore mis-relocated
  silently instead of refused. MSVC rarely emits these, but Anvil should
  refuse them itself.
- The "target inside the stolen bytes" case keeps the original 8-bit
  displacement. That is correct only if no instruction between the branch
  and its target changed size. Two widened short branches in a row, or a
  backward branch, would break it. The tests cover only the simple forward
  case.
- No analysis outside the stolen bytes: if any other code jumps to an
  address inside bytes 1–4 (or 1–13), it lands in the middle of the new
  jump. Mid hooks are most exposed, because mid-function sites are often
  branch targets or loop heads. The plugin author has to choose a site
  where that cannot happen; Anvil cannot check it cheaply.
- Minimum size: a function shorter than 5 bytes is still "hooked" by
  copying instructions past its end (padding, the next function) into the
  trampoline. That works only if those bytes are executable padding.
  SafetyHook does not check for function end. MinHook stops at `ret` and
  uses the hot-patch area above the function only when there is padding
  ([MH/src/trampoline.c#L290-L309](https://github.com/TsudaKageyu/minhook/blob/8af6b4acae5a9388fd742b56fa79ece89d96f823/src/trampoline.c#L290-L309)).

## 4. Several hooks on one function, overlap, removal order

### Verified

- SafetyHook keeps no global registry of hooks. Each `InlineHook` only
  records its target, trampoline and original bytes (whole source of
  `inline_hook.cpp`).
- Stacking on the **same address** is supported and tested. Four
  `SafetyHookInline`s on one function run newest first ("carol and eve and
  alice and bob"), and the test removes them in **reverse order only**
  ([SH/test/inline_hook.cpp#L12-L84](https://github.com/cursey/safetyhook/blob/19223663fb8a573253ffb2e82da87cc354bf5c16/test/inline_hook.cpp#L12-L84);
  also `example/multiple.cpp`). It works because the second hook's stolen
  bytes are the first hook's `E9 rel32`, which is relocated like any rel32
  jump (§3).
- `disable()` writes the hook's saved `m_original_bytes` back to the target
  (L421–L422), and `destroy()` then frees the trampoline (L429–L439). The
  allocator puts freed blocks on a free list and reuses them for later
  allocations
  ([SH/src/allocator.cpp#L85-L170](https://github.com/cursey/safetyhook/blob/19223663fb8a573253ffb2e82da87cc354bf5c16/src/allocator.cpp#L85-L170)).
- MinHook, for comparison, refuses a second hook on the same target within
  one MinHook instance (`MH_ERROR_ALREADY_CREATED`,
  [MH/src/hook.c#L589](https://github.com/TsudaKageyu/minhook/blob/8af6b4acae5a9388fd742b56fa79ece89d96f823/src/hook.c#L589),
  [L658](https://github.com/TsudaKageyu/minhook/blob/8af6b4acae5a9388fd742b56fa79ece89d96f823/src/hook.c#L658)).
  Separate MinHook copies in different DLLs do not see each other and
  stack the same way as above.
- Anvil 1 refuses overlaps only between **different owners**, using the
  real stolen length (`original_bytes().size()`) for existing hooks, and
  rolls back one owner's entries in reverse order (`src/core/patching.cpp`,
  `overlaps_other_owner`, `rollback`).

### Derived

- **Out-of-order removal breaks the chain.** Take hooks A (older) and B
  (newer) on one address and remove A first. A writes the function's
  original prologue back, so B is silently disconnected while still
  "enabled". A's trampoline goes to the free list, but B's trampoline
  still jumps into it. If B is removed next, it writes back its saved
  bytes, which are A's `E9` to the freed (possibly reused) block, and the
  next call executes stale or foreign code.
- **Overlap at different addresses:** a hook at `X+k` (0 < k < stolen
  length of a hook at `X`) decodes from the middle of the existing `E9`
  and relocates garbage. A hook at `X` placed after a hook at `X+k` copies
  the `E9` of the inner hook as a relative jump. Its own trampoline then
  runs the bytes before `X+k` and jumps into the inner hook, which is
  correct only by luck of instruction boundaries; removal order again
  matters. Neither case is detected by SafetyHook.
- In-flight calls: removal frees the trampoline (and, for a mid hook, the
  stub) right away. A thread inside the destination returns into freed
  memory. Only `call()` callers are protected, because `destroy()` takes
  the same recursive mutex. Mid-hook stubs have no such guard.

### Recommendation (chaining policy)

- Anvil owns exactly **one SafetyHook hook per address** and builds the
  chain itself:
  - **Mid hooks:** one stub per site calls a list of plugin callbacks in a
    fixed order (load order, or a priority), all on the same
    `AnvilCpuContext`. Removing a callback edits the list; no code bytes
    change.
  - **Inline hooks:** each link gets a `next` pointer that Anvil hands out
    as the plugin's `original`. Link *i* calls `next[i]`, and the last link
    calls the SafetyHook trampoline. Removing a link re-points the previous
    `next` with one atomic store, so links can leave in any order.
- Never free a trampoline, stub or chain node while the process lives.
  Plugins are not unloaded (a failed plugin's DLL stays mapped), so leaking
  a few hundred bytes per removed hook is cheaper than proving quiescence.
- Keep refusing different-address overlaps inside the stolen range of any
  hook, also for the same owner. Same-address requests join the chain
  instead of returning `ANVIL_ERROR_CONFLICT`. `expected_prologue` is
  compared against the original bytes (already the registry's rule).
- Refuse `E0–E3` branches and FF-mode relocation of IP-relative code in
  Anvil before calling SafetyHook. Anvil's near-image allocator makes FF
  mode unnecessary.

## 5. Thread safety while the game runs

### Verified

- Since v0.5.0
  ([`b046e12`](https://github.com/cursey/safetyhook/commit/b046e12), "Thread
  trapping") `enable`/`disable` patch inside `trap_threads(from, to, len,
  fn)`
  ([SH/src/os.windows.cpp#L268-L320](https://github.com/cursey/safetyhook/blob/19223663fb8a573253ffb2e82da87cc354bf5c16/src/os.windows.cpp#L268-L320)).
  Before that, up to v0.4.1, SafetyHook enumerated and `SuspendThread`ed
  all threads and fixed their contexts.
  1. The trap is registered in a process-wide `TrapManager`. Its first use
     installs a first-priority vectored exception handler, which is never
     removed until static destruction (L173–L176). The trap list is never
     pruned (`insert_or_assign`, L223).
  2. The pages of `from` and `to` are switched to `PAGE_READWRITE` (not
     executable), or to RWX if they share an allocation with SafetyHook
     itself or with `VirtualProtect` (L281–L300). A global
     `virtual_protect_mutex` serialises this (race fix
     [`b8a0ab7`](https://github.com/cursey/safetyhook/commit/b8a0ab7),
     v0.6.7).
  3. The bytes are written, and protections are restored.
  4. Meanwhile the handler catches access violations. If the faulting
     address is `from + i` within the hooked range, it sets `Rip = to + i`.
     If it is elsewhere on a trapped page, it returns
     `EXCEPTION_CONTINUE_EXECUTION`, so the thread re-faults and spins
     until the page is executable again (L230–L256).
- For `enable`, `from` is the target and `to` the trampoline; for `disable`
  it is the reverse (L383, L421).
- The README still lists "Stops all other threads when creating or deleting
  hooks"
  ([SH/README.md#L5](https://github.com/cursey/safetyhook/blob/19223663fb8a573253ffb2e82da87cc354bf5c16/README.md#L5)),
  which describes the pre-0.5 design.
- `InlineHook::call()` (and `ccall`, `thiscall`, …) holds the hook's
  `std::recursive_mutex` for the whole call of the original.
  `unsafe_call()` does not
  ([SH/include/safetyhook/inline_hook.hpp#L217-L276](https://github.com/cursey/safetyhook/blob/19223663fb8a573253ffb2e82da87cc354bf5c16/include/safetyhook/inline_hook.hpp#L217-L276),
  [L349](https://github.com/cursey/safetyhook/blob/19223663fb8a573253ffb2e82da87cc354bf5c16/include/safetyhook/inline_hook.hpp#L349)).
  Anvil hands plugins `original<void*>()`, the raw trampoline, so plugin
  calls do not take that mutex.
- MinHook freezes instead: a Toolhelp snapshot, then `SuspendThread` on
  every other thread, with the IP of each thread remapped through
  per-instruction `oldIPs`/`newIPs` tables
  ([MH/src/hook.c#L163-L205](https://github.com/TsudaKageyu/minhook/blob/8af6b4acae5a9388fd742b56fa79ece89d96f823/src/hook.c#L163-L205),
  [L267-L360](https://github.com/TsudaKageyu/minhook/blob/8af6b4acae5a9388fd742b56fa79ece89d96f823/src/hook.c#L263-L380)).
  It can enable all queued hooks under one freeze
  (`MH_ApplyQueued`/`MH_ALL_HOOKS`).

### Derived

- **Gap in trapping:** a thread that was preempted with RIP inside the
  stolen bytes (after their first instruction) and is not scheduled during
  the short patch window resumes after protections are restored. It then
  executes the middle of the new jump. Freezing handles this case because
  it fixes RIP in suspended contexts; trapping does not. The window is
  microseconds and the site must be mid-prologue, so the risk is low but
  not zero.
- `fix_ip` maps `from + i → to + i` byte for byte. After a short branch was
  widened earlier in the stolen bytes, later instruction offsets in the
  trampoline differ, and a trapped thread could be sent mid-instruction.
  MinHook's instruction tables avoid this.
- Side effects in a running game:
  - every thread that executes anything on the affected 4 KiB pages spins
    on access-violation exceptions during each patch (two `VirtualProtect`
    round trips);
  - every later access violation in the process (including ones the game
    handles itself with SEH) first passes the SafetyHook VEH, which takes a
    mutex and scans the trap list.
- Each `enable()` traps separately; there is no batch API like
  `MH_ApplyQueued`.

### Recommendation

- Install hooks in the `early` phase (single game thread) whenever
  possible. Treat main-phase install and removal as "safe for most sites,
  not guaranteed", and say so in the API docs.
- Don't free code memory on removal (§4). This also covers threads still
  inside destinations or trampolines.

## 6. Per-call cost on x64

### Measured (indicative)

Setup:

- Machine and build: i9-13900KF, Windows 11, MSVC 14.44 `/O2`, SafetyHook
  v0.7.0 + Zydis 4.1.0.
- Target: 15 bytes of hand-written code in its own page,
  `push rbx; sub rsp,20h; mov eax,ecx; add eax,edx; add rsp,20h; pop rbx;
  ret`. 5 bytes are stolen.
- Loop: called through a volatile function pointer, 50 M calls × 9 runs,
  thread pinned to logical CPU 2.
- Hooks: the inline detours call the original through the raw trampoline,
  as Anvil plugins do, or through `hook.call()`. The mid-hook destination
  is empty.
- **Caveat:** EU4 was running on the same machine (CPUs 1–7) during the
  only run, so the numbers are indicative, not a lab measurement. There
  was a single session and no repeat, and the multi-thread rows are the
  noisiest.

| Case | median ns/call | over baseline |
|---|---:|---:|
| Baseline, unhooked | 1.58 | — |
| Inline hook, raw trampoline call | 2.51 | +0.9 |
| 2 stacked inline hooks, raw calls | 3.58 | +2.0 |
| Inline hook, `hook.call()` (recursive mutex) | 11.93 | +10.4 |
| Mid hook, empty destination | 22.44 | +20.9 |
| 2 stacked mid hooks, empty | 39.78 | +38.2 |
| Baseline after removing all hooks | 1.98 | — |

Four threads calling the same hooked function, wall time per call per
thread (5 M calls each, threads on CPUs 2/4/6/8):

| Case | ns/call |
|---|---:|
| Inline, raw | 2.9 |
| Inline, `call()`, 1 thread | 36.3 |
| Inline, `call()`, 4 threads | 107.4 |
| Mid, empty | 42.9 |

The `call()` row grows with threads, consistent with the mutex being held
across the original (§5).

### Estimate (reasoning over the generated code)

- **Inline hook entry:** `E9` + `FF 25 [rip]`, two well-predicted jumps
  (about 1 ns). The way back to the original is the copied prologue plus
  one `E9`.
- **Mid hook:** about 80 instructions. These are 19 pushes plus `pushfq`,
  16 `movdqu` stores and 16 loads (512 bytes of stack traffic), an
  indirect call, `popfq` (microcoded, slow), and a final `ret` whose target
  was pushed rather than called. That `ret` almost surely mispredicts the
  return stack buffer on every hit, which is likely the largest single item.
- The stub uses legacy-SSE `movdqu` without `vzeroupper`. If the game
  leaves dirty YMM upper state, there is an SSE/AVX transition cost (a false
  dependency on recent Intel cores).
- Stack use is about 460 extra bytes per mid hook.
- **Order of magnitude for EU4:** +1 ns per inline link and about 20 ns per
  mid-hook site are negligible for per-frame or per-day calls. In inner
  loops called millions of times per in-game day, a mid hook can be
  measurable. For example, 10 M hits per second cost about 0.2 s of CPU per
  second, which is significant. Prefer function-entry inline hooks, or
  `redirect_call`, for hot paths.

### Recommendation (hook kinds)

- API 2 should offer two kinds:
  - **function hook** (inline, chainable, `original` per link), the cheap
    default;
  - **mid hook** (callback with `AnvilCpuContext*`), documented as about
    20 ns plus the callback per hit.
- Several mid callbacks at one site share **one** stub and one context save
  (Anvil dispatches the list). This avoids paying about 17 ns per extra
  SafetyHook `MidHook`, as in the stacked row.

## 7. Open questions

- Does EU4 1.37.5 keep live YMM state or DF = 1 at any candidate mid-hook
  site? This needs a scan of `eu4.exe` for VEX-encoded instructions.
- Repeat the benchmark in the lab with EU4 closed and pinned per the lab
  rules, to get numbers that can be cited.
- Whether to vendor the mid-hook stub from day one, so the context ABI does
  not depend on SafetyHook at all, or only when SafetyHook changes it.
