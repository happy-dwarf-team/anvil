# How the reference plugins hook, patch and verify EU4 1.37.5

Inventory for [Inventory how reference plugins hook and verify the game](https://github.com/happy-dwarf-team/anvil/issues/13), part of [Map: Anvil 2 specification](https://github.com/happy-dwarf-team/anvil/issues/12). Static reading of the sources (2026-10-19); nothing was built or run.

| Plugin | Repository @ commit | Loads through |
| --- | --- | --- |
| EU4 Unicode Patch | YozoraTempest/EU4-Unicode-Patch @ `2ce9154` | its own `VERSION.dll` proxy (EU4DLL layout, `plugins/*.dll`) |
| EU4 Menu Patch | YozoraTempest/EU4-Menu-Patch @ `7752efd` | any EU4DLL-style `VERSION.dll` loader |
| Mare Liberum | rdavislee/eu4-per-good-trade @ `b54d922` | its own `d3dx9_43.dll` proxy |
| Bellows | happy-dwarf-team/bellows @ `8b842c4` | Anvil API 1, phase `early` |
| Tongs | happy-dwarf-team/tongs @ `51a8b6c` | Anvil API 1, phase `main` |

Counts are of code sites found by searching the sources. A call inside a loop over a table is counted per table entry where the table is visible; otherwise the count is textual and marked "≥". Everything else is read from the code; inferences are marked as such.

## Code changes per plugin

| Kind of change | Unicode Patch | Menu Patch | Mare Liberum | Bellows | Tongs |
| --- | ---: | ---: | ---: | ---: | ---: |
| Mid-function hook with register save | **92** (MinHook jump to MASM stubs, 92 `PROC`s in 7 `.asm` files, 1,638 lines; `SAVE_CONTEXT` saves volatile GPRs, flags, XMM0–5) | **2** (hand-assembled stubs: call a C++ function, then jump to a different continuation; i.e. `rip` is changed) | **7** (own `detour::install`: 14-byte `jmp [rip]`, stub saves all GPRs + flags + volatile XMM into `Regs`, writes them back) | 0 | 0 |
| Function hook with trampoline | ≈ 49 (MinHook) | 0 | 0 | ≥ 2 (`hook_function`) | ≥ 2 (`hook_function`: frame and cursor) |
| Hook on code outside `eu4.exe` | **6** (IDirect3DDevice9 vtable methods: Reset, DrawIndexedPrimitive, DrawPrimitive, CreateVertexBuffer, SetStreamSource, CreateTexture, hooked in `d3d9.dll` via MinHook) | 0 | 0 | 0 | 0 |
| Call-site redirect | 0 | 0 | **20** (incl. both game-setup paths on the loading screen) | ≥ 6 (`redirect_call`) | 0 |
| Byte patch | 25 (5-byte calls replaced with NOPs) | 0 | 8 (`patch_bytes` with expected bytes) | ≥ 5 (`patch_bytes`) | 0 |
| Data pointer swap | 0 | **1** (front-end function-pointer slot at `0x1D5A8C8+0x20`, checked against its expected value, swapped with `InterlockedExchangePointer`) | not counted (sources mention vtables 54 times; mostly reads) | 0 | 0 |
| Import replacement | 0 | 0 | 0 | 3 (`KERNEL32` `CreateFileW`, `GetFileAttributesExW`, `GetFileAttributesA`) | 0 |
| Calls into game functions by RVA | yes (name append/assign, text input, keyboard, editor) | yes (menu transition) | yes (trade driver, reachability rebuild, console) | no | **yes, central** (≈ 53 RVA references: console, UI tree, events, input; ADR 0001) |

## Addresses and verification

| | Unicode Patch | Menu Patch | Mare Liberum | Bellows | Tongs |
| --- | --- | --- | --- | --- | --- |
| How addresses are found | fixed RVAs in code | fixed RVAs in code | fixed RVAs in code, chosen offline with Capstone (`impl/tools/disasm.py`, `OFFSETS.md`) | fixed RVAs in a per-build table (`build.hpp`, 18 RVAs) | fixed RVAs in code |
| Runtime pattern scanning | no | no | no | no | no |
| What is verified before patching | hand-written C++ profile: **348** sites (RVA + expected bytes) and **14** section ranges with required access (code / read / write) | the same profile design: **25** sites, 9 ranges | expected bytes at each hook and patch site; refuses on mismatch | expected bytes per Anvil patch call | **93** `[[expect]]` entries in `plugin.toml`, apart from the RVAs in code |
| Where the same address is written | twice: profile and install code | twice: profile and install code | once per site (expected bytes next to the install call) | twice: `build.hpp` and expected bytes in each module | twice: manifest and code |
| Whole-file identity | SHA-256 logged, not enforced | SHA-256 logged, not enforced | build checked at attach ("checks the binary") | build id from Anvil | build id from Anvil |
| Shared code | `executable_compatibility.cpp` (169 lines) | a separate copy of the same file (169 lines, own namespace) | own `detour.h` (247 lines) with own `alloc_near`, thread suspension and range check | Anvil | Anvil |

## Start-up, gating and threads

| | Unicode Patch | Menu Patch | Mare Liberum | Bellows | Tongs |
| --- | --- | --- | --- | --- | --- |
| When it installs | in its `DllMain`, loaded by its proxy's `DllMain` (loader lock held) | worker thread started from `DllMain`, concurrent with game start-up | worker thread from the proxy's `DllMain`; part of the install runs inside two redirected game-setup calls on the loading screen | Anvil `early` phase (entry point, before the CRT) | Anvil `main` phase; game calls are dispatched on the main thread through its frame hook |
| Atomic install | one MinHook transaction (`MH_EnableHook(MH_ALL_HOOKS)`) after all hooks are created; a failure enables none | all page permissions acquired before any write; a failure writes nothing | per site; other threads suspended and checked to be outside the patched range | per patch through Anvil, rolled back on load failure | per patch through Anvil |
| Gate | refuses when EU4DLL's `plugin64.dll` is loaded | none | active only when its mod is enabled in `dlc_load.json`; otherwise dormant | none | `enabled`, `input`, `game_calls` switches in its config |
| Own threads | — | installer thread | worker thread | warm-up and prefetch workers | pipe server and notification threads |

## Configuration, logging and what the player sees

| | Unicode Patch | Menu Patch | Mare Liberum | Bellows | Tongs |
| --- | --- | --- | --- | --- | --- |
| Configuration | `config.ini` (search options); an `.ini` for experimental input in research builds | none | marker files next to the DLL (e.g. `DEFDUMP`) | `config.toml` in Anvil's data folder, plus `EU4SB_*` environment variables | its config file (switches above) |
| Log | own log file next to the DLL | own log file next to the DLL | `per-good-trade.log` next to `eu4.exe` | through Anvil's log | through Anvil's log |
| Crash data | none of its own | none of its own | **own vectored exception handler** writing `pgt_crash.log` (survives stack overflow; the engine writes no dump then) | none of its own | none of its own |
| Shown to the player | nothing (log only) | nothing (log only) | nothing (log only) | nothing (log only) | nothing (log only) |
| Tests | many unit tests plus recorded in-game evidence | a guard host test | a detour self-test (`detourtest.cpp`) and acceptance scripts | unit tests | unit tests |

## What this means for API 2 (inferences)

- **Mid-function hooks are the main tool of the non-Anvil plugins:** 101 sites in three plugins, built three different ways (MASM macros, a stub generator, hand-assembled bytes). One saves only volatile registers, two save all; Menu Patch needs to change `rip`. A mid hook with a full, writable register context covers all three.
- **Hooks are needed outside `eu4.exe`.** Unicode Patch hooks six Direct3D 9 device methods in `d3d9.dll`; Anvil API 1 only accepts RVAs inside `eu4.exe`.
- **Nobody scans for patterns at run time.** All five use fixed RVAs for 1.37.5 and verify expected bytes. Pattern search matters for finding addresses offline, not for loading.
- **Every plugin writes each address at least twice** (code plus profile or manifest), except Mare Liberum, which keeps expected bytes next to each call but has no list of dependencies it only reads or calls. One declaration that feeds both code and verification would remove the duplication in four of the five.
- **Section access checks** (code / read-only / writable ranges) are part of both YozoraTempest profiles and absent from Anvil's `[[expect]]`.
- **Data pointer swaps with an expected value** (Menu Patch) are not a separate Anvil call today; `patch_bytes` can do it but without atomic pointer exchange.
- **All-or-nothing install** is expected by every plugin (MinHook transaction, permissions first, rollback).
- **Installing from a game call on the loading thread** (Mare Liberum) and **running work on the main thread** (Tongs) are two ways of reaching a safe point in the game; neither exists in the core.
- **A plugin gated on an enabled mod** (Mare Liberum) is close to the planned Workshop discovery: the plugin is active only with its mod in the playset.
- **No plugin shows anything to the player**; one built its own crash logger because nothing else records stack overflows.
