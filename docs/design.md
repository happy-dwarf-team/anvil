# Anvil: design

Status: **draft 3, accepted 2026-10-19.** It specifies Anvil 1.0 with plugin API 2 and collects the decisions of [Map: Anvil 2 specification](https://github.com/happy-dwarf-team/anvil/issues/12). Each section links the ticket that holds the reasoning. How it is delivered is in §22. This document describes the target; the code reaches it through the pull requests of §22.3. Stage 1 (Anvil 0.2, API 1) is described by draft 2 in the git history.

Words in **bold** at first use are defined in [GLOSSARY.md](../GLOSSARY.md).

## 1. Goals

For players:
- Native plugins load into EU4 1.37.5 (Windows, 64-bit) **without modifying `eu4.exe` or any game file**. Removing Anvil returns the game to vanilla.
- An unknown game build, a broken plugin or a crash never prevents the game from starting, and the player is told what happened and why.
- Plugins that change the simulation never act in ironman or multiplayer unless both the author and the player allow it.

For plugin authors:
- A modder who already writes hooks ports one hook in an evening. Every game address is declared once, verified before load and typed in code. The author writes no register-save assembly, no byte profiles and no own proxy DLL.
- Mistakes surface early: a signature mismatch fails to compile, and `anvil check` finds byte, memory and conflict problems without starting the game.

For both:
- Anvil's own cost stays within published budgets (§17), and everything Anvil does is visible in its log.

Non-goals: Linux/macOS, 32-bit, other Paradox games, game builds other than 1.37.5, loading EU4DLL-style `plugins/*.dll` (EU4DLL is treated as legacy), a scripting layer for modders without C/C++.

## 2. Injection: proxy `version.dll` (unchanged)

`eu4.exe` imports `VERSION.dll`, which is not in `KnownDLLs`. Anvil ships as `version.dll` next to `eu4.exe`:
- it forwards the 17 exports to `%SystemRoot%\System32\version.dll`;
- `DllMain` checks that the process is `eu4.exe` and overwrites the executable's entry point with a jump to Anvil;
- at the entry point Anvil restores those bytes, runs the `early` phase and continues into the game.

This is verified for direct starts and through the Paradox launcher (EU4 engine lab, `docs/anvil-real-folder.md`).

## 3. Folder layout

```
<EU4>/eu4.exe
<EU4>/version.dll                      Anvil (proxy + core)
<EU4>/AnvilManager.exe                 player tool, also the installer (§15)
<EU4>/anvil/
    anvil.toml                         enabled, disabled_plugins, profile, log_level, opt-ins (§13)
    anvil.log, anvil.prev.log
    trust.toml                         approvals of Workshop plugins (§14)
    crashes/<time>.json|.txt           last 10 crash reports (§12)
    plugins/<plugin-id>/               local plugins: trusted by placement
        plugin.toml  symbols.toml  settings.toml  <entry>.dll  (+ read-only files)
    data/<plugin-id>/                  writable per plugin; settings.toml holds the player's values
    data/anvil/approved/<id>/<hash>/   the previously approved copy of a Workshop plugin
<mod folder of an enabled mod>/anvil/<plugin-id>/   Workshop plugins (§14)
```

## 4. Core and standard plugins

Decided in [What belongs in the Anvil core and what in library plugins](https://github.com/happy-dwarf-team/anvil/issues/17).

**Rule:** a feature belongs to the **core** only if it must work before any plugin loads or needs a view across all plugins at once.

Core:
- discovery, resolution and load order;
- build identity (the core's only game knowledge);
- symbol verification;
- the registry of changes and the chains;
- services and events;
- the approval decision for Workshop plugins;
- crash capture and attribution;
- suspending gameplay plugins (§13);
- plugin status (`anvil.status`) and the log.

The core never shows a blocking dialog.

**Standard plugins** are library plugins maintained by the project and released with Anvil. They load, fail and can be disabled or replaced like any other plugin; dependents of a missing one are skipped with the reason.
- `anvil-game`: game readiness, a frame event, running work on the main thread, and a session event (ironman, multiplayer).
- `anvil-overlay`: notifications, the panel and the settings UI (§15).

Knowledge that only some plugins need (GUI tree, console, text) stays in those plugins and their services.

## 5. A plugin package

### 5.1 `plugin.toml` (manifest v2)

```toml
id = "hello"                        # [a-z0-9-]+, max 64, unique
name = "Hello"
version = "0.1.0"                   # SemVer
authors = ["Your Name"]
description = "..."
api = 2
anvil = ">=1.0.0"                   # optional SemVer range of Anvil
entry = "hello.dll"
phase = "main"                      # "early" | "main"
symbols = "symbols.toml"            # optional; its `build` replaces game_builds
settings = "settings.toml"          # optional
affects_gameplay = false            # REQUIRED (§13)
allow_ironman = false               # false | "opt-in"
allow_multiplayer = false           # false | "opt-in"
load_after = []
issues = "https://github.com/you/hello/issues"   # optional; shown with crash reports
repository = "https://github.com/you/hello"      # optional; shown when approving

[[dependencies]]
id = "anvil-game"
version = "^1"
optional = false

[[conflicts]]                       # this plugin is skipped while a listed one is enabled
id = "other-text-renderer"
version = "<2"
```

Unknown keys are errors ([Offline anvil check CLI and plugin CI](https://github.com/happy-dwarf-team/anvil/issues/23)). `game_builds` and `[[expect]]` are gone: a plugin without `symbols` runs on every build; with `symbols` it runs only on that file's `build`.

### 5.2 `symbols.toml`

Decided in [One source of truth for game addresses](https://github.com/happy-dwarf-team/anvil/issues/18), extended by [Plugin authoring experience](https://github.com/happy-dwarf-team/anvil/issues/21) and [API 2 gaps from the paper port](https://github.com/happy-dwarf-team/anvil/issues/32).

```toml
build = "eu4-1.37.5-steam-win64"

[symbol.speed_object]                         # a function
rva = 0x8024D0
bytes = "48 8B 81 18 04 00 00 C3"             # hex pairs, ?? = any byte
memory = "code"                               # code | read | write
signature = "void* (void* session)"           # typed symbol in C/C++/Rust
note = "session vtable slot 0x100: returns the game-speed object"

[symbol.person_name_block]                    # a block for replace_block (Unicode Patch's person_name_hook)
rva = 0xA4B48C
bytes = "48 8D 55 B8 48 83 7D D0 10 48 0F 43 55 B8 4C 8B 45 C8 48 8B CB E8 4A 7E 64 FF"
memory = "code"
# resume = "<symbol>"                         # optional; default: the end of the block (0xA4B4A6 here)

[symbol.frontend_tick_slot]                   # a pointer slot
rva = 0x1D5A8E8
memory = "read"
points_to = 0x10D2E80

[symbol.root_and_date]                        # values taken from verified bytes
rva = 0x14321FC
bytes = "48 8B 05 75 DC F0 00 8B 98 D0 1D 00 00"
memory = "code"
values.root        = { at = 3, rip_relative = 7, type = "Root" }   # -> sym::root, Data<Root> at 0x233FE78
values.date_offset = { at = 9, size = 4 }                          # -> sym::root_and_date.date_offset = 0x1DD0

[symbol.physfs_get_write_dir]                 # by export name instead of RVA
export = "PHYSFS_getWriteDir"
signature = "const char* ()"
```

- **Kinds:** a symbol is a function (`signature`), a pointer slot (`points_to`), data (`type`) or a site (anything else).
- **Values:** they must lie on fixed bytes.
- **Exports:** an `export` symbol is resolved through the export table; `bytes`, if given, check the prologue.
- **Imports:** the format accepts symbols imported from a shared address library (the library itself is fog, §21).
- **Schema:** unknown keys are errors. Names match `[a-z][a-z0-9_]*` (they become identifiers in generated headers), and a symbol's id is its position in the file. A symbol has exactly one of `rva` and `export`; one at an `rva` needs `memory` and `bytes` or `points_to` (a data symbol may have neither). `signature`, `points_to` and `type` exclude each other. A value has `at` and either `size` (1, 2, 4 or 8, little-endian) or `rip_relative`, the offset of the instruction's end, at least `at + 4`: it yields the RVA `rva + rip_relative + disp32`. `resume` names another symbol and needs `bytes`, the block it ends.

### 5.3 `settings.toml`

Decided in [Plugin configuration convention and the overlay settings panel](https://github.com/happy-dwarf-team/anvil/issues/35).

```toml
[setting.ram_prefetch_budget_mb]
type = "int"                  # bool | int | float | enum | string | key
default = 1024
min = 64
max = 8192
label = "RAM prefetch budget (MB)"
description = "RAM for unpacked archive members the game has not taken yet."
group = "RAM prefetch"
restart = true                # true: applies at the next start
```

## 6. Game build identity

Anvil reads the PE header of the running image (TimeDateStamp, SizeOfImage, CheckSum, entry point) and maps it to a build id from a compiled-in table. The full SHA-256 is checked off the critical path and cached. With an unknown build, plugins with `symbols` are skipped with "game build unknown (EU4 updated?)", and the overlay says so: it runs on every build. Other storefront builds are not supported yet ([Other distributions of EU4 1.37.5](https://github.com/happy-dwarf-team/anvil/issues/36), §21).

## 7. Verification before loading

For each plugin, before its DLL is loaded and before any plugin has changed anything, Anvil:
1. parses `plugin.toml`, `symbols.toml` and `settings.toml`; an unknown key or a schema error skips the plugin;
2. resolves `export` symbols;
3. compares every symbol with the image: `(actual & mask) == (expected & mask)`, the memory kind against the section flags, and `points_to` against the stored pointer;
4. computes `values`.

Bytes already changed by other plugins are compared in their original form, so two plugins can depend on the same function. A mismatch skips the plugin, logging the symbol, the RVA and the first differing byte. `anvil check` runs the same code offline (§16).

## 8. Changes

Decided in [Hook API 2: hook kinds, chaining and conflict policy](https://github.com/happy-dwarf-team/anvil/issues/19) and [API 2 gaps from the paper port](https://github.com/happy-dwarf-team/anvil/issues/32). Facts: [SafetyHook research](https://github.com/happy-dwarf-team/anvil/issues/14).

### 8.1 Seven kinds of change

| Kind | Call | Notes |
| --- | --- | --- |
| Function hook | `hook_function(sym, detour, &next)` | detour with its own `next` |
| Mid hook | `mid_hook(sym, callback, user)` | callback gets `AnvilCpuContext`; the original code runs afterwards |
| Block replacement | `replace_block(sym, callback, user)` | the declared block never runs; resume at `resume` or at the block's end |
| Call redirect | `redirect_call(sym, target, &next)` | `call rel32` sites |
| Byte patch | `patch_bytes(sym, bytes, size)` | expected bytes come from the declaration |
| Pointer swap | `swap_pointer(sym, value, &previous)` | atomic; also in read-only memory |
| Import replacement | `replace_import(module, function, replacement, &next)` | `eu4.exe` imports |

In addition, `address_of(sym)` gives a verified address to call or read.

Every call that touches `eu4.exe` takes a declared symbol, never a raw RVA. Raw pointers are accepted only **outside** the `eu4.exe` image (`hook_function_at` and friends, e.g. Direct3D 9 vtables). Those still go through the registry and chains, with an optional prologue check.

### 8.2 Chains

- Anvil owns one SafetyHook hook per address and chains the plugins itself.
- Order follows the load order (dependencies, `load_after`): the earlier plugin is outermost. There are no priorities.
- Every link has its own `next`. Redirects and pointer swaps chain with `next` being the previous target or value.
- Links can be removed in any order. Executable memory (stubs, trampolines) is never freed.
- At one address, mid hooks run first, then a block replacement or the function-hook chain or the redirected call.
- A mid hook that changes `rip` ends the chain; the first occurrence per address and session is logged.

### 8.3 Conflicts and unsafe sites

- A change whose bytes overlap another plugin's change at a different address is refused, and both owners are logged. A block replacement owns its whole block.
- The one exception: identical byte patches share ownership.
- A hook whose stolen bytes contain `JRCXZ` or `LOOP*` is refused, because SafetyHook mis-encodes them.

### 8.4 Staging, handles, suspension

- Changes requested during `anvil_plugin_load` are staged and enabled together after it returns `ANVIL_OK`; on failure none are enabled.
- Later changes are atomic one by one or as an explicit group.
- Every change returns a handle and can be removed. A failed load rolls back everything the plugin had.
- The core can **suspend and resume all changes of a plugin**: it bypasses its links, reverts its patches and restores its pointers (§13).
- All kinds are allowed in every phase; SafetyHook's page trapping covers threads.

### 8.5 `AnvilCpuContext`

- The layout is identical to SafetyHook's `Context64` (408 bytes): XMM0–15, RFLAGS, the GPRs, `rsp` (read-only), a reserved slot (`trampoline_rsp`) and `rip` (writable). It is pinned with `static_assert`s.
- YMM upper halves, x87 and MXCSR are not saved.
- If SafetyHook's layout ever changes, Anvil ships its own stub and keeps the ABI.
- A lighter, volatile-only context is not offered in 1.0 (§17).

## 9. Plugin API 2

The C ABI is in `include/anvil/anvil_plugin.h`; the draft is [`docs/api2/anvil_plugin.h`](api2/anvil_plugin.h) until it replaces API 1. Plugins vendor it.
- **Host table:** log, `plugin_dir`, `data_dir`, game info, the change calls of §8, `address_of`, services, events, and settings (§11).
- **Versioning:** the `size` and `api_version` fields keep it extensible; within API 2 functions are only appended (`ANVIL_HOST_HAS`).
- **Generated headers:** `anvil gen` produces `symbols.gen.h/.hpp` (symbol ids, typed function symbols, values) and `settings.gen.h/.hpp` (typed accessors).
- **C++ wrapper** (`anvil.hpp`, header-only):
  - `ANVIL_PLUGIN(Type)`; a failing call in `load()` throws, the macro turns it into a result, and Anvil rolls back;
  - hook callbacks must not throw: the adapter catches, logs once and continues;
  - `anvil::HookOf<sym::x>`; mid hooks and block replacements take any callable;
  - a signature mismatch fails with a `static_assert` naming the symbol;
  - `Host` is a stateless value (`anvil::Host{}` works anywhere);
  - `Change` is a move-only handle.
- **Rust** ([Rust SDK tier and timing](https://github.com/happy-dwarf-team/anvil/issues/22)): `anvil-sys` (committed bindgen bindings, checked against the header in CI, published to crates.io), `anvil gen --lang rust`, and a Rust hello in CI. The safe `anvil` crate comes later, with recorded principles: `#[anvil::plugin]` returning `Result`, `catch_unwind` adapters, typed symbols, `Drop` handles, a `#[repr(C)]` context.

## 10. Load order and failure policy

1. The proxy is set up (always); the entry-point hook is placed in `eu4.exe` only.
2. At the entry point: no `anvil/` folder means nothing happens. `ANVIL_DISABLE=1` or `enabled = false` means the log only.
3. **Safe mode:** two consecutive crashes before `game_ready` (§12) mean this start loads no plugins except the standard ones the overlay needs to explain it.
4. The build is identified.
5. Plugins are discovered: local `anvil/plugins/*`, and Workshop plugins in enabled mods (approved versions only, §14).
6. Resolve and verify:
   - API version and the `anvil` range;
   - the build of `symbols`;
   - duplicates (the local plugin wins over a Workshop one with the same id);
   - disabled ids;
   - the symbols (§7);
   - `conflicts` (the declaring plugin is skipped, see below);
   - dependencies and cycles.

   **Conflicts.** A plugin that declares a `conflicts` entry is skipped when a plugin with that id and a version in the range is among the candidates that passed the checks above; the other plugin loads normally. The log says `conflicts with <id> <version>; disable one of them`. Whatever depends on a skipped plugin is skipped by the dependency check, as usual. Edge cases are resolved the same way on every start, whatever the discovery order:

   - A plugin that is itself skipped for a conflict does not count as a target. Plugins that conflict with nothing alive are kept first, and whoever declares a conflict with one of them is skipped. So with `a` conflicting with `b` and `b` with `c`, `b` is skipped and `a` and `c` load.
   - When only cycles are left, the smallest id (byte order) is skipped and the rest is resolved again. Two plugins that conflict with each other leave the one with the larger id; in `a -> b -> c -> a`, `a` is skipped, then `b` because of `c`, and `c` loads.
   - A conflict with a plugin that is absent, disabled, rejected by the API, `anvil` or symbols checks, or outside the version range is ignored.
   - A candidate that is skipped later, because its own dependency is missing or in a cycle, still counts as a conflict target: conflicts are resolved before dependencies.
7. `early` plugins are loaded at the entry point, then `main` plugins on an Anvil thread. Each plugin's staged changes are enabled when its load returns.
8. `anvil.plugins_loaded` is published and the status is recorded.

## 11. Settings

- Defaults live in `settings.toml` in the plugin folder, read-only.
- The player's values live in `anvil/data/<id>/settings.toml`, only the values that differ from the default; the file can be edited by hand.
- An invalid value falls back to the default, with a warning in the log and the overlay.
- `ANVIL_<ID>_<NAME>` environment variables override values for testing, and Anvil logs it.
- Plugins read validated values through generated accessors. For `restart = false` settings they can subscribe to changes.
- The overlay and the Manager edit the values. Settings are global per plugin; profiles are fog.

## 12. Crash capture and attribution

Decided in [Crash attribution to plugins](https://github.com/happy-dwarf-team/anvil/issues/24).

- **Capture**
  - The core registers a VEH at the entry point. It records fatal-looking exceptions in memory only: no IO, at most 8.
  - An unhandled-exception filter chained ahead of EU4's writes the report, then hands over to EU4's own crash reporter.
  - On stack overflow, a helper thread prepared at start-up writes the report and a small minidump.
- **Report:** `anvil/crashes/<time>.json` and `.txt`, keeping the last 10. It contains:
  - the code and the address as module + offset;
  - for `eu4.exe` addresses, the nearest declared symbol and whether a plugin's change covers it;
  - for stubs and trampolines, the change, plugin and symbol;
  - the unwound stack with every frame attributed;
  - the registers, the plugins with versions, the build, and the tail of the log.
- **Levels:** "in plugin X's code", "in plugin X's change", "plugin X on the stack", or "no plugin involved". Never "X caused it".
- **Next start:** a notice in the overlay with "disable X at the next start" (only on click), and safe mode after two crashes before `game_ready`. Nothing is uploaded; "copy report" plus the `issues` link.

## 13. Gameplay-affecting plugins

Decided in [Gameplay-affecting plugins in ironman, achievements and multiplayer](https://github.com/happy-dwarf-team/anvil/issues/29).

- **`affects_gameplay`** is required: `true` if the plugin can change the simulation's state or results.
- **Suspension:** `anvil-game` publishes a session event. In ironman or multiplayer the core suspends every change of gameplay plugins (§8.4), the plugin is told so it can stop its threads, and the changes come back after the session.
- **Opt-in:** `allow_ironman` / `allow_multiplayer = "opt-in"` let the player allow a plugin in that mode; the choice is stored in `anvil.toml`.
- **Achievements:** with an ironman opt-in active, the overlay shows a permanent notice that achievements stay enabled but may break Paradox's rules.
- **Multiplayer:** the overlay shows a hash of the active gameplay plugin set for players to compare by hand.

## 14. Workshop distribution and trust

Decided in [Workshop distribution and trust model](https://github.com/happy-dwarf-team/anvil/issues/25). Facts: [Workshop research](https://github.com/happy-dwarf-team/anvil/issues/16).

[Run the Workshop experiments E1 and E2](https://github.com/happy-dwarf-team/anvil/issues/34) found:
- **E1:** a mod with only `descriptor.mod` + `anvil/**` keeps the 1.37.5 checksum and allows ironman achievements.
- **E2:** a private upload through the Paradox launcher delivered the DLL byte-identical, with no warnings; the launcher only adds `remote_file_id` to `descriptor.mod`.

- **Discovery:** only mods enabled in the current playset (`dlc_load.json` → `.mod` → `path`), in `anvil/<plugin-id>/` folders, several per mod. This also gates a plugin on its mod being enabled.
- **Trust:**
  - an approval covers a SHA-256 over the whole plugin folder;
  - `trust.toml` records the id, version, Workshop id, hash and date;
  - the Manager can revoke an approval.
- **Updates:** a new hash is pending; the previously approved copy (`data/anvil/approved/`) keeps running until the new one is approved.
- **First approval:**
  - the overlay asks once the game runs, and the plugin loads from the next start;
  - the Manager can approve before launch;
  - the prompt shows the name, authors, version, mod, `affects_gameplay`, symbol count, hash, `repository` and a native-code warning.
- **Anvil itself** cannot be installed by the Workshop (items never land next to `eu4.exe`); the Manager installs it (§15).

## 15. Player UI

Decided in [Player UI: in-game overlay and Anvil Manager](https://github.com/happy-dwarf-team/anvil/issues/26).

- **`anvil-overlay`**
  - Drawing: Dear ImGui on D3D9 (`EndScene`/`Reset` via `hook_function_at`, chained with other D3D9 hooks); input by subclassing the game window's WndProc. It turns itself off on the OpenGL backend (fog).
  - Notifications: non-blocking, for skipped plugins, the previous crash, pending approvals/updates and safe mode.
  - Panel on `Ctrl+Shift+F12`:
    - status and reasons;
    - approvals;
    - "disable at the next start";
    - the crash report;
    - the log tail;
    - the settings editor.
  - It never pauses the game. Plugins get no ImGui context.
- **`AnvilManager.exe`** (C++ and Dear ImGui, static):
  - finds the game (Steam `libraryfolders.vdf`);
  - installs, repairs and uninstalls Anvil from its own package;
  - enables plugins, approves and revokes;
  - shows what will load and why (`anvil check --game --format json`);
  - shows crashes and logs, and launches through `steam://rungameid/236850`;
  - has an opt-in GitHub release check that only shows a link. Nothing is downloaded.

## 16. Tooling for authors

- **`anvil.exe`** (in the SDK archive) is built from the core's own library:
  - `anvil gen [--lang c|cpp|rust]`;
  - `anvil check` with strict schemas, which also verifies, with `--exe`, the build, bytes, memory, pointers, exports, values and instruction boundaries; with `--game <folder>`, every installed plugin and the conflicts between them;
  - `--format text|json|github`; exit codes 0 OK, 1 problems, 2 cannot check.
- **`anvil gen [--lang c|cpp] <symbols.toml> <out-dir>`** writes both headers without `--lang` and leaves a file alone when its content would not change:
  - `symbols.gen.h`: `SYM_<NAME>` ids, `sym_<name>_fn` pointer types from `signature`, `SYMVAL_<SYMBOL>_<VALUE>` constants;
  - `symbols.gen.hpp`: namespace `sym` with an `anvil::Function<signature>`, `Site`, `Slot` or `Data<type>` per symbol ([`anvil_symbols.hpp`](../include/anvil/anvil_symbols.hpp)), values as `static constexpr` members (`sym::root_and_date.date_offset`), and an `anvil::Data<type>` for each typed `rip_relative` value (`sym::root`);
  - a value is the integer in the declared bytes, or for `rip_relative` an RVA, which gen cannot know for an `export` symbol. The plugin declares the types named in `signature` and `type` before including the header. Names the C++ header cannot hold (keywords, a value called `id` or `value`, two `sym::` names alike) are errors.
- **Problems carry lines:** the parsers report the line of the key (one error per file, as the loader stops at it); `--exe` reports every failing symbol at its `[symbol.<name>]` line. `--format github` prints `::error file=<path>,line=<n>,title=anvil check::<message>` with the path as passed, so CI runs it from the repository root.
- **Instruction boundaries** (`--exe`): `symbols.toml` does not say how a symbol is used, so the rule covers every function or site in code with at least 5 declared bytes (a `jmp rel32`):
  - decoded from the first byte (Zydis, x86-64; `??` bytes take the image's values), the declared bytes are valid instructions and the last one ends exactly at their end, so a block replacement owns whole instructions and a hook's stolen bytes lie in verified bytes;
  - the instructions that start in the first 5 bytes, which a hook relocates, include no `JRCXZ`, `JECXZ`, `LOOP`, `LOOPE` or `LOOPNE` (§8.3);
  - a block's `resume` symbol is in code.

  Shorter sites (byte patches, which may start inside an instruction), data and pointer slots are not decoded. Hooking a symbol with fewer than 5 declared bytes would steal unverified bytes; the change registry decides whether to refuse it.
- **`anvil check --game <folder> [--format text|json|github]`** is the start-up analysis (§10) without starting the game, for a folder with `eu4.exe` and `anvil/`:
  - It reads `anvil/anvil.toml` (`enabled`, `disabled_plugins`; a file that does not parse is a problem, as the loader ignores it) and every folder of `anvil/plugins`, in the order the loader discovers them. Each plugin is checked as `anvil check <folder> --exe <folder>/eu4.exe` does, except that nothing is written: no `symbols.lock`.
  - The plugins that pass discovery (a readable `plugin.toml`, the entry DLL present) go to the loader's own resolver (`ResolveInput`, shared code in `anvil_core`): API and `anvil` range, build and symbols, duplicates, disabled ids, `conflicts`, dependencies and cycles. The result is what loads, in which order, and what is skipped and why, in the words of the loader's log. Only what the loader acts on skips a plugin: a failing instruction-boundary check is a problem of the plugin, which still loads.
  - **Overlapping ranges (warning).** `symbols.toml` does not say which symbols a plugin will change, so every verified symbol with `memory = "code"` and declared `bytes` counts as a range `[rva, rva + length)`. For each pair of plugins that load together, ranges that overlap are reported as one warning, at the symbol of the plugin that loads first (`file:line: warning: …`; `::warning` with `github`), naming both symbols, and counting further overlaps of the pair. An identical range (same address and length) is the normal case of a chain (§8.2) and is not reported. Whether two plugins really change the same bytes is decided at load by the change registry, which refuses the second change (§8.3). A warning never changes the exit code, and plugins that are skipped overlap nothing. Symbols of other memory kinds and symbols without `bytes` are not compared.
  - **Exit codes** as in `check`. 0: nothing is wrong. 1: a plugin has problems, a plugin that is not disabled by the player is skipped, or `anvil.toml` does not parse. What the player disabled (`disabled_plugins`, `enabled = false`) is listed but does not count, and neither do its problems (text and `github` print them as warnings). 2: no folder, no `eu4.exe` (or not a readable game image), no `anvil` folder, or a plugin whose files cannot be read. With 2 for the whole folder nothing else is printed.
  - **JSON** is one object, which the Manager (§15) reads. `plugins[]` has one entry per folder of `anvil/plugins`, each the result `anvil check` prints for one plugin (`result`, `exit_code`, `plugin`, `build`, `symbols`, `exe`, `problems`) plus `folder` and `status`; the other sections are always present, empty when there is nothing:
    ```json
    {
      "tool": "anvil 1.0.0",
      "result": "problems",              // "ok" | "problems" | "cannot-check"
      "exit_code": 1,
      "cannot_check": "…",               // only with "cannot-check"
      "game": {"folder": "C:/…/Europa Universalis IV",
               "exe": {"name": "eu4.exe", "sha256": "9AD3EFE1…", "build": "eu4-1.37.5-steam-win64"}},   // build: null when unknown
      "anvil_toml": {"enabled": true, "disabled_plugins": ["old-ui"]},
      "problems": [{"file": "…/anvil.toml", "line": 2, "message": "…"}],   // of anvil.toml
      "plugins": [
        {"folder": "hello", "status": "loaded",   // "loaded" | "skipped" | "disabled"
         "result": "ok", "exit_code": 0, "plugin": "hello", "build": "eu4-1.37.5-steam-win64",
         "symbols": 3, "exe": {"sha256": "…", "build": "…"}, "problems": []}
      ],
      "load_order": [{"id": "hello", "version": "0.1.0", "phase": "early", "folder": "hello"}],   // early plugins first
      "skipped": [{"id": "other", "folder": "other-1.2", "reason": "conflicts with hello 0.1.0; disable one of them", "disabled": false}],
      "warnings": [{"file": "…/hello/symbols.toml", "line": 7, "message": "…", "plugins": ["hello", "tweaks"]}]
    }
    ```
    `id` in `skipped` is the folder name when `plugin.toml` is broken; `disabled` marks what the player turned off. `file` and `line` are as in `check` (the path starts with the folder as passed; `line` 0 is the whole file). Fields may be added in minor versions; consumers ignore the ones they do not know.
  - Text prints each plugin's problems and summary, then the load order, the skipped plugins and the warnings. `github` prints annotations for the problems, for a plugin that is skipped without problems of its own, and for the warnings.
- **`symbols.lock`:** `anvil check --exe` writes the `eu4.exe` SHA-256, a digest of `symbols.toml`, the tool version and the result. CI, which cannot have the game, fails when the lock is stale.
  ```toml
  format = 1
  tool = "anvil 1.0.0"                # informational
  symbols_sha256 = "…"                # symbols.toml without a UTF-8 BOM, CRLF read as LF (git autocrlf)
  exe_sha256 = "9AD3EFE1…771A"
  build = "eu4-1.37.5-steam-win64"    # the exe's build, or "unknown"
  result = "ok"                       # or "problems"
  ```

  It lies next to `plugin.toml` and is committed. `--exe` writes it also when it finds problems, so CI fails until they are fixed, but not when a file does not parse or the exe cannot be read. Without `--exe`, a plugin with `symbols` fails on a missing lock, a different digest, `result = "problems"`, an unknown key or a newer `format`.
- **SDK:**
  - an archive on every release, consumed with `FetchContent`;
  - `anvil_add_plugin(<target> SOURCES …)` runs `anvil gen`, builds with the static CRT and lays out `plugin/<id>/`;
  - `<target>-check` (with `EU4_EXE`) and `<target>-deploy` (with `EU4_DIR`).
- **Template:** `happy-dwarf-team/anvil-plugin-template`. Its CI runs `anvil check --format github` and the lock check; its release workflow zips the plugin folder, or a mod folder for the Workshop. The README covers a first plugin in 15 minutes.

## 17. Performance budgets

Decided in [Performance budgets and how the lab measures them](https://github.com/happy-dwarf-team/anvil/issues/20).

| Quantity | Budget |
| --- | --- |
| Core start-up, no plugins (entry point → end of loading) | ≤ 20 ms; no difference in `Running application` detectable within the spread |
| Same with `anvil-game` and `anvil-overlay` | ≤ 50 ms |
| Function hook, call redirect: first / each further link | ≤ 2 ns / ≤ 2 ns |
| Mid hook, block replacement: first / each further link | ≤ 25 ns / ≤ 20 ns |
| Pointer swap, byte patch, import replacement | 0 |
| All changes of the standard plugins in a SIMULATION year | ≤ 0.5% |
| Memory: core / per change | ≤ 2 MB / ≤ 1 KB |

**Method**
- In-game impact is per-call cost × counted calls; the optional counters are enabled with `profile = true`.
- Microbenchmarks run in CI as a ±20% regression check. Binding numbers come from the lab: i9-13900KF, CPU4, median of ≥ 30.
- Start-up follows the lab procedure, with ≥ 5 runs per variant.
- SIMULATION A/B through Tongs, with ≥ 5 valid trials, guards only against ≥ 2% regressions.

Exceeding a budget blocks the release and any change to stubs, chains or core start-up.

## 18. Logging and status

`anvil/anvil.log` is UTF-8 and rotated. It records the Anvil version, the build, each plugin's status and reason, its changes, verification failures with the symbol and byte, timings and the call counters. The same status is published as the `anvil.status` service and event for the overlay and the Manager.

## 19. Security

Plugins are native code with the game's permissions:
- local plugins are trusted by placement, and Workshop plugins need per-version approval;
- Anvil and its standard plugins never download anything; the Manager's release check is opt-in and only links;
- releases are built by GitHub Actions with SHA-256 sums and provenance attestation. Code signing is fog.

## 20. Build, test, release

- CMake, with MSVC (VS 2022) as the reference toolchain. C++23 inside Anvil, a C ABI at the boundary, and the static CRT.
- **Tests:** unit tests for manifests, symbols, settings, the resolver, chains (orders, removal in any order, conflicts, staging, suspension) and the CLI, plus microbenchmarks and a Rust hello build.
- **Lab checks:** the vanilla copy, the proxy without `anvil/`, each standard plugin, the budgets, and E1/E2 for the Workshop.
- **Releases:** release-please and Conventional Commits, squash-merged PRs; the release workflow attaches the Anvil zip, the SDK archive, `anvil.exe`, the Manager and `anvil-sys`.

## 21. Open items and fog

- **Other storefront builds** ([#36](https://github.com/happy-dwarf-team/anvil/issues/36)): GOG ships two different `eu4.exe` builds (1.37.5.0 and 1.37.5.2), and the Microsoft Store exe is encrypted on disk. Unknown builds are skipped safely; GOG support needs a GOG binary (fog).
- **Workshop rules:** the Paradox forum's mod rules were not readable (E3).
- **Fog** (from the map):
  - hot reload;
  - code signing;
  - the ABI stability policy for API 2;
  - localization of player messages;
  - the shared address library;
  - a vcpkg port;
  - call counts of text hooks (lighter context);
  - OpenGL in the overlay;
  - setting profiles;
  - automatic multiplayer plugin-set exchange;
  - the safe Rust crate.

## 22. Delivery

Decided in [Rewrite or evolve, and the pull request sequence to Anvil 2](https://github.com/happy-dwarf-team/anvil/issues/28).

### 22.1 Evolve, not rewrite

- **Kept:** the proxy and entry-point hook, log, semver, resolver, services and events, build identity, CI and release-please.
- **Rewritten:** the change layer (`PatchRegistry` → a change registry with chains) and the public header.
- **Replaced:** `[[expect]]`, by `symbols.toml` verification.

### 22.2 Versions, repositories, releases

- **Versions:** the first release with plugin API 2 is **Anvil 1.0.0**. The Anvil version and the plugin API version are separate numbers.
- **Repositories:** `plugins/anvil-game`, `plugins/anvil-overlay` and `tools/manager` live in this repository (one CI, one release archive). `anvil-plugin-template`, Bellows and Tongs have their own repositories.
- **Releases:**
  - **1.0:** core with API 2, `anvil.exe`, SDK, template, `anvil-game`, settings, crash capture with safe mode, `anvil-sys`, and the migrated Bellows and Tongs.
  - **1.1:** `anvil-overlay` with the settings panel.
  - **1.2:** the Manager.
  - **1.3:** Workshop discovery and trust.
- **API 1:** no compatibility layer. Anvil 1.0 skips `api = 1` plugins with "plugin API 1: update the plugin". Bellows and Tongs migrate before 1.0 and release with it.

### 22.3 Pull-request sequence

One PR per line, Conventional Commits, squash-merged. Implementation issues and milestones track them.

**1.0**
- **A. Foundations**
  1. Split the core into a library shared by the loader and a CLI target.
  2. Manifest v2.
  3. `symbols.toml` parsing and verification.
  4. `anvil.exe gen` and `check` (`symbols.lock`, `--format json|github`).
- **B. Changes** (after A)
  5. Change registry with chains, staging and handles: function hook, call redirect, byte patch, import replacement, `*_at`.
  6. Mid hooks and block replacement.
  7. Pointer swap.
  8. Suspend and resume of a plugin's changes.
  9. The API 2 header, the C++ wrapper, and hello examples in C and C++.
  10. Microbenchmarks with a CI regression gate.
- **C. Around the plugins** (after B; independent of each other)
  11. Settings.
  12. Crash capture and attribution with safe mode.
  13. `conflicts` in the resolver.
  14. `anvil check --game`.
  15. Call counters.
  16. SDK archive and CMake package.
- **D**
  17. `plugins/anvil-game` with the gameplay policy.
  18. `anvil-sys` with `anvil gen --lang rust`.
- **E. Elsewhere:** the `anvil-plugin-template` repository; the Bellows migration; the Tongs migration.
- **F:** lab measurements against §17, then the release-please PR.

**1.1:** `plugins/anvil-overlay`. **1.2:** `tools/manager`. **1.3:** Workshop discovery, `trust.toml`, approved copies, and the approval UI in the overlay and the Manager.
