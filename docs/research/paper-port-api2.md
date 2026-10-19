# Paper port of the reference plugins onto the API 2 draft

Result of [Paper-port the reference plugins onto the API 2 draft](https://github.com/happy-dwarf-team/anvil/issues/27), part of [Map: Anvil 2 specification](https://github.com/happy-dwarf-team/anvil/issues/12). Static reading (2026-10-19); nothing was built or run.

The draft is what the map has decided so far:
- `symbols.toml` format v1 ([#18](https://github.com/happy-dwarf-team/anvil/issues/18), extended in [#21](https://github.com/happy-dwarf-team/anvil/issues/21));
- the six kinds of change, chains and staging ([#19](https://github.com/happy-dwarf-team/anvil/issues/19));
- the core and the standard plugins `anvil-game` / `anvil-overlay` ([#17](https://github.com/happy-dwarf-team/anvil/issues/17));
- the header and wrapper prototype on branch [`prototype/hello-plugin`](https://github.com/happy-dwarf-team/anvil/tree/prototype/hello-plugin/prototypes/hello-plugin).

The inputs are the counts in the [inventory](https://github.com/happy-dwarf-team/anvil/blob/research/reference-plugin-inventory/docs/research/reference-plugin-inventory.md), plus a second reading of the hook stubs.

Legend: **fits**: one API 2 call or declaration expresses it. **awkward**: expressible, but the author must hand-write something Anvil could know. **gap**: not expressible in the draft.

## New fact from the second reading

**91 of Unicode Patch's 92 mid hooks do not return to the original code.** Each MASM stub saves context, calls C++, writes result registers, and ends with `jmp qword ptr [g_*_return]`, a fixed continuation further down the function. The overwritten instructions are replaced, not re-run. Example `person_name_hook`: it replaces the code at `0xA4B48C`, writes `rax`, and resumes at `0xA4B4A6`. Menu Patch's 2 stubs do the same (call, then `jmp` to the stock epilogue). Bellows `verifypath_listing` builds its own 0x28-byte stub, reached by `patch_bytes` writing `call rel32; nop` over a 6-byte indirect call, that also performs the following `mov ebp,eax`.

## Unicode Patch

| Today | API 2 | Fit |
| --- | --- | --- |
| Own `VERSION.dll` proxy loading `plugins/*.dll` | Anvil | fits |
| 91 MinHook jumps to MASM stubs that replace a block and resume at a fixed address | `mid_hook` with a callback that sets `cpu.rip = address_of(sym::continuation)`; the continuation declared as a site | **awkward**: every callback sets `rip` by hand, and Anvil does not know the skipped range, so another plugin's change inside it would silently never run (see G1) |
| 1 mid hook that returns normally | `mid_hook` | fits |
| ≈ 49 MinHook function hooks with trampolines | `hook_function` with typed `sym::` and `next` | fits |
| 6 IDirect3DDevice9 methods hooked via the device vtable | `hook_function_at` (outside `eu4.exe`); chains with `anvil-overlay` on the same methods | fits |
| 25 NOP patches over 5-byte calls | `patch_bytes` on 25 site symbols | fits |
| Calls into game functions (name append/assign, text input, keyboard, editor) | `address_of` / typed `host.call` with `signature` | fits |
| Profile: 348 sites (RVA + bytes) and 14 section ranges | `symbols.toml` with `memory` per symbol; section ranges become per-symbol `memory` | fits (most sites are evidence for struct offsets used in code; see G2) |
| Atomic `MH_EnableHook(MH_ALL_HOOKS)` | staging during `anvil_plugin_load` | fits |
| Refuses when EU4DLL's `plugin64.dll` is loaded | out of scope for Anvil; for two Anvil text plugins see G5 | gap |
| Fonts and pinyin dictionary next to the DLL; `config.ini` | needs `plugin_dir` and `data_dir` (the prototype header omitted `plugin_dir`) | fits once `plugin_dir` is kept |
| Installs in `DllMain` under the loader lock | phase `early` or `main` | fits |
| 92 mid hooks inside text layout and rendering, partly per glyph; the MASM context saves only volatile GPRs and XMM0–5 | SafetyHook mid hook ≈ 21 ns per call (indicative) | **awkward**: cost in hot loops is unknown (G6) |

## Menu Patch

| Today | API 2 | Fit |
| --- | --- | --- |
| Needs an EU4DLL-style loader | Anvil | fits |
| 2 hand-assembled stubs: `mov rcx, r13/rbx`; call; `jmp` to the stock epilogue | `mid_hook` reading `cpu.r13`, calling the function, setting `rip` to a declared continuation | **awkward** (G1) |
| Front-end function-pointer slot in `.rdata`, checked against its stock target, swapped atomically | `swap_pointer` on a slot symbol with `memory = "read"` and `points_to` | fits |
| 25-site profile, a copy of Unicode Patch's code | `symbols.toml` | fits |
| Installs from a thread started in `DllMain` | phase `main` | fits |
| Steam lobby clean-up through game functions | `address_of` with `signature` | fits |

## Mare Liberum

| Today | API 2 | Fit |
| --- | --- | --- |
| Own `d3dx9_43.dll` proxy | Anvil | fits |
| 7 mid hooks via own `detour.h` (all GPRs, flags, volatile XMM; resumes into a trampoline) | `mid_hook` (`AnvilCpuContext` is a superset) | fits |
| 20 call-site redirects, incl. both game-setup calls on the loading screen (install runs inside them) | `redirect_call` with `next` | fits |
| 8 byte patches with expected bytes | `patch_bytes` | fits |
| Thread suspension and range check while patching | SafetyHook page trapping in Anvil | fits |
| Own `alloc_near` (after a rel32 truncation crash) | not needed for these kinds | fits |
| Dormant unless its mod is enabled in `dlc_load.json` | shipped inside the mod's `anvil/` folder (stage 3), the plugin is found only when the mod is enabled | fits once Workshop discovery exists; until then a locally installed plugin has no way to say so (G7) |
| Own vectored-exception crash log that survives stack overflow | core crash attribution | requirement for [#24](https://github.com/happy-dwarf-team/anvil/issues/24): capture on stack overflow, file opened in advance |
| Frame-poll first tick | `anvil-game` frame event | fits |
| Reads of trade-node structures | `Data<T>` symbols plus offsets in code | awkward (G2) |
| Marker files as switches | configuration convention (map fog) | fits later |

## Bellows

| Today | API 2 | Fit |
| --- | --- | --- |
| 18 RVAs in `build.hpp`, bytes in each module | `symbols.toml` | fits |
| `redirect_call` ×≥6, `patch_bytes` ×≥5, `replace_import` ×3 (KERNEL32), `hook_function` ×2 | same kinds, symbol arguments | fits |
| `alloc_near_image` + hand-written stub + `patch_bytes` over a 6-byte `call [rip+disp32]`, also replacing the next `mov ebp,eax` | a mid hook replacing the block (G1); `redirect_call` covers only `E8 rel32` | **awkward** today, fits with G1 |
| `GetProcAddress` on `eu4.exe` for `PHYSFS_setErrorCode`, `PHYSFS_getLastErrorCode`, `PHYSFS_getWriteDir` | no way to declare a symbol by export name | **gap** (G3) |
| Checks that a DIR archiver vtable entry points to `dir_stat` | slot symbol with `points_to` | fits |
| Reads the game's ready marker itself | `anvil-game` readiness | fits |
| Phase `early` (mimalloc, warm-up threads) | phase `early` | fits |
| `config.toml` in the data folder, `EU4SB_*` variables | `data_dir` | fits |

## Tongs

| Today | API 2 | Fit |
| --- | --- | --- |
| 93 `[[expect]]` apart from ≈ 53 RVA uses in code | `symbols.toml` | fits for addresses |
| 66 of those 93 exist only to prove a global address or a struct offset that the code then writes again by hand (e.g. "root global 0x233FE78, current date at root+0x1DD0", decoded from `48 8B 05 75 DC F0 00 8B 98 D0 1D 00 00`) | the offsets and the global stay hand-written in code | **gap**: the same number is written twice again (G2) |
| Frame hook and main-thread dispatch | `anvil-game` | fits |
| Cursor hook | `hook_function` | fits |
| Pipe server and notification threads, config switches | unchanged | fits |
| `affects_gameplay = true` | [#29](https://github.com/happy-dwarf-team/anvil/issues/29) | open |

## Gaps

- **G1 Block replacement.** A mid hook that replaces a declared range and resumes at a declared continuation. Anvil sets `rip` and treats the skipped range as owned, so conflicts are detected. Used 91× in Unicode Patch, 2× in Menu Patch and 1× in Bellows. The draft only lets the callback write `rip`.
- **G2 Values taken from declared bytes.** Constants a plugin needs that are already inside the bytes it declares: a RIP-relative global's target, struct offsets, immediates. `anvil gen` could emit them, e.g. `sym::root_and_date.root` and `.date_offset`. This affects 66 of Tongs's 93 checks, most of Unicode Patch's 348 sites, and Mare Liberum's structure reads.
- **G3 Symbols by export name.** `eu4.exe` exports PhysFS functions; Bellows resolves 3 with `GetProcAddress`. A symbol could be declared by `export = "PHYSFS_getWriteDir"` instead of an RVA.
- **G4 `plugin_dir` in API 2.** The prototype header dropped it; Unicode Patch (fonts, dictionary) and Bellows (profiles) need it. This fixes the draft and needs no decision.
- **G5 Declared incompatibility.** Two Anvil plugins that may not run together although their bytes do not overlap (two text renderers). A manifest field such as `conflicts = ["id"]`.
- **G6 Mid-hook cost in hot paths.** Unicode Patch puts mid hooks in per-glyph text code. Whether ≈ 21 ns per call matters, and whether a lighter context is worth offering, belongs in [#20](https://github.com/happy-dwarf-team/anvil/issues/20).
- **G7 Gating on an enabled mod.** A locally installed plugin that should run only with a given mod enabled. This is likely covered by stage-3 discovery ([#25](https://github.com/happy-dwarf-team/anvil/issues/25)); otherwise a manifest condition.
