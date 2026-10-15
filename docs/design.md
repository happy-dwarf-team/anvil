# Anvil — design

Status: draft 2 (2026-10-15), stage 1 implemented. Decisions marked **D** are
agreed; **Q** are open.

## 1. Goals

- Load native plugins into EU4 (Windows, 64-bit, Steam) **without modifying
  `eu4.exe` or any game file**.
- Removing Anvil (or all plugins) returns the game to vanilla behaviour without
  restoring files.
- Runtime cost of loading a plugin ≈ cost of patching the same change into the
  executable: plugins run in-process, as early as the game's own code.
- Safe failure: an unknown game build, a broken manifest or a crashing plugin
  initializer must never prevent the game from starting.
- Clear status for the player: a log of what was loaded and what was skipped.
- Distributable to the community via GitHub Releases (later also Steam
  Workshop for plugins).

Non-goals (for now): Linux/macOS builds of EU4, 32-bit, other Paradox games,
an in-game overlay (stage 4), scripting languages inside plugins.

## 2. Injection mechanism — proxy `version.dll` (D)

`eu4.exe` 1.37.5 statically imports `VERSION.dll` and `WINMM.dll`; neither is
in `KnownDLLs`, so the Windows loader searches the application directory
first. Anvil ships as `version.dll` next to `eu4.exe`:

- It loads the real `%SystemRoot%\System32\version.dll` (explicit full path)
  and forwards all 17 exports (`GetFileVersionInfoA/W`, `…ExA/ExW`,
  `GetFileVersionInfoSizeA/W/ExA/ExW`, `VerFindFileA/W`, `VerInstallFileA/W`,
  `VerLanguageNameA/W`, `VerQueryValueA/W`, `GetFileVersionInfoByHandle`).
- The real DLL is loaded lazily, by full path, on the first forwarded call
  (never from `DllMain`). A bare `"version.dll"` would resolve to the proxy
  itself, which is already loaded.
- **Entry-point hook (D, changed from draft 1):** `DllMain` only checks that the
  process is `eu4.exe` and overwrites the first 14 bytes of the executable's
  entry point with an absolute jump to Anvil. When the main thread reaches the
  entry point, every DLL is initialized and the loader lock is released, but
  the game's C runtime has not started. Anvil restores the 14 bytes first (so
  plugins and `[[expect]]` checks see the unmodified image), loads `early`
  plugins there and continues into the game. Plugins can therefore
  load DLLs and start threads (forbidden in `DllMain`) and still run before
  the game allocates its first heap block. Any other process, or any failure,
  leaves the DLL a pure proxy. `ANVIL_ANY_EXE=1` allows other executable
  names (lab/testing only).
- `version.dll` was chosen over `winmm.dll` because its export surface is tiny
  and stable.

Alternatives considered: adding an import to `eu4.exe` (modifies the game;
overwritten by Steam), a launcher that injects into a suspended process
(changes how the game is started through Steam/Paradox launcher), AppInit_DLLs
(system-wide, disabled by Secure Boot).

## 3. Folder layout (D)

```
<EU4>/eu4.exe
<EU4>/version.dll                 Anvil (proxy + loader)
<EU4>/anvil/
    anvil.toml                    loader settings (optional; defaults apply)
    anvil.log                     this start; previous start → anvil.prev.log
    plugins/<plugin-id>/
        plugin.toml               manifest
        <entry>.dll               plugin binary (+ any read-only files)
    data/<plugin-id>/             writable per-plugin data (caches, learned lists, config)
    trust.toml                    stage 3: approvals of Workshop plugin versions
```

Plugin folders are treated as read-only (a Workshop folder can be replaced by
Steam at any time); everything a plugin writes goes to `data/<plugin-id>/`.

## 4. Plugin manifest `plugin.toml` (D, fields may grow)

```toml
id = "eu4-startup-boost"          # [a-z0-9-]+, max 64, unique
name = "EU4 Startup Boost"
version = "0.1.0"                 # SemVer
authors = ["Happy Dwarf Team"]
description = "Faster game start-up."
api = 1                           # Anvil plugin API major version
anvil = ">=0.2.0"                 # optional: Anvil versions the plugin needs (SemVer range)
entry = "eu4_startup_boost.dll"   # file name inside the plugin folder
phase = "early"                   # "early" | "main"
affects_gameplay = false          # true = may change simulation (multiplayer/ironman warning)
load_after = []                   # ordering only, no dependency
game_builds = ["eu4-1.37.5-steam-win64"]   # or ["*"] when no game addresses are used

[[dependencies]]                  # zero or more
id = "anvil-ui"
version = ">=1.2.0, <2"           # SemVer range: comparators joined by ',', ^ ~ = > >= < <= *
optional = false                  # optional: loaded first if present, never required

[[expect]]                        # zero or more: game bytes the plugin depends on
build = "eu4-1.37.5-steam-win64"  # the build these RVAs belong to ("*" = same bytes in every build)
rva = 0x210000                    # TOML integer; hex allowed
bytes = "48 89 5C 24 08 ?? ?? 57" # hex pairs, whitespace ignored, "??" = any byte (e.g. relocated addresses)
name = "CEU4App::ResetGame"       # optional, for the log
```

Parsing: toml++ v3.4.0 (header-only, MIT). Unknown keys are logged and ignored.

### Dependency bytes `[[expect]]` (D)

Patch calls already verify the bytes they overwrite. A plugin usually also
*relies* on game code it does not patch: functions it calls by RVA, vtables,
globals, struct layouts read through them. `[[expect]]` declares those ranges.

- Entries whose `build` is the running build (plus all `"*"` entries) are
  compared during resolution, before any plugin DLL is loaded and before any
  plugin has patched anything; with an unknown build only `"*"` entries apply.
  A mismatch, or a range outside the image or not readable, skips the plugin
  with the RVA and the first differing byte in the log, and skips everything
  that depends on it.
- `build` must be listed in `game_builds` (unless `game_builds` contains
  `"*"`); `rva + size` must fit in 32 bits; one entry compares at most
  `ANVIL_MAX_EXPECT_BYTES` (64 KiB) and must contain at least one non-wildcard
  byte.
- Dependencies only known at run time use the host call `expect_bytes` (§6)
  with the same semantics and an arbitrary bit mask.
- Rationale: the EU4 Menu Patch (YozoraTempest/EU4-Menu-Patch) refuses to
  install unless every native function it calls still has its known bytes; a
  file hash would reject harmless edits of unrelated code, and checking only
  the patch sites would miss a changed callee.

## 5. Game build identity (D)

Plugins patch code at fixed addresses, so they are tied to exact builds.

- At start Anvil reads the PE header of the running image (TimeDateStamp,
  SizeOfImage, CheckSum, entry point) and maps it to a build id from a table
  compiled into Anvil (`eu4-1.37.5-steam-win64` → SHA-256 `9AD3EFE1…771A`).
  The version resource of `eu4.exe` only says "1.0", so it is not used.
- The decision is made from the header alone (no hashing on the critical
  path). The full SHA-256 is checked afterwards on the main-phase thread and
  cached in `anvil/data/anvil/build-cache.txt` (key: size + mtime). A mismatch
  is logged; every patch is still verified against expected bytes, so a wrong
  build fails safely.
- Unknown build → plugins that list specific builds are skipped with a clear
  log line ("game build unknown (EU4 updated?) not supported; plugin supports
  …").

## 6. Plugin API (C ABI, header-only SDK) (D)

The SDK is the single header `include/anvil/anvil_plugin.h` (API 1); plugin
repositories vendor a copy. Compiler-independent C ABI. Summary:

- `AnvilHost` (function table, `size` + `api_version` for compatibility):
  `log`, `plugin_dir`, `data_dir`; patching — `patch_bytes`, `redirect_call`,
  `hook_function`, `replace_import`, `alloc_near_image`; services —
  `register_service`, `get_service`; events — `subscribe`, `publish`;
  appended after API 1.0 — `expect_bytes` (check with `ANVIL_HOST_HAS`).
  `game` points to `AnvilGameInfo` (build id or NULL, image base/size, PE
  identity, `ready_signal_known`).
- `expect_bytes(self, rva, expected, mask, size)` is a read-only dependency
  check: `(actual & mask) == (expected & mask)`, mask NULL = every bit. Bytes
  patched by other plugins are compared in their original form. Once the
  calling plugin has patches of its own the call returns
  `ANVIL_ERROR_WRONG_PHASE`: verify everything first, then patch, so that a
  main-phase plugin never leaves the running game half-patched.
- Plugins export `anvil_plugin_load(const AnvilHost*, AnvilPlugin*)`.
- All patch addresses are RVAs inside `eu4.exe`. The host verifies expected
  bytes / call target / import before writing, refuses overlaps with patches of
  other plugins (registry; both ids are logged), records originals, and rolls
  back all patches of a plugin whose load fails (error result or structured
  exception, caught by an SEH guard). The DLL of a failed plugin stays mapped
  (it may have started threads) but inactive.
- Function hooks: SafetyHook v0.7.0 (BSL-1.0; fetches Zydis 4.1.0) inside
  Anvil, hidden behind the C API so it can be replaced (D). Requires C++23 for
  Anvil itself only. Call redirection and byte patches are Anvil's own code;
  stubs come from an allocator of executable memory within ±2 GB of the image
  (no reuse of `int3` padding inside `eu4.exe`).
- Phases:
  - `early`: called at the executable entry point on the main thread, outside
    the loader lock, before the game's C runtime starts (see §2). DLL loading
    and threads are allowed; no UI.
  - `main`: called on an Anvil thread right after the early phase, concurrently
    with the game start-up.

### Services and events between plugins (D)

Following the patterns of mature plugin systems (Fabric's dependency
metadata, SKSE's messaging, VS Code extension APIs):

1. **Dependencies** in the manifest (above) define availability and order:
   topological sort per phase, ties by id; missing/mismatched required
   dependency, dependency cycle, or an early plugin depending on a main-phase
   plugin → the plugin is skipped with a reason, and everything depending on
   it is skipped too.
2. **Service registry**: `register_service(self, "<own-id>.<name>", major,
   ptr)` / `get_service(self, name, major)`. A service is a C struct of
   function pointers whose first field is `uint32_t size`; minor additions are
   appended (check `size`), breaking changes get a new major (both majors may be
   registered at the same time). The providing plugin publishes the struct in
   its own header. Services live for the process lifetime; a plugin can only
   register under its own id prefix.
3. **Events**: `subscribe(self, event, callback, context)` / `publish(self,
   event, payload)`, synchronous on the publishing thread, in subscription
   order. Built-in: `anvil.plugins_loaded` (after the main phase) and
   `anvil.game_ready` (known builds only; Anvil polls the game's
   initialization marker). The `anvil.` prefix is reserved for the host.

## 7. Load order and failure policy (D)

1. Proxy setup (always). Entry-point hook in `eu4.exe` only.
2. At the entry point: if `anvil/` does not exist → nothing (no files written).
   `ANVIL_DISABLE=1` or `enabled = false` in `anvil/anvil.toml` → log only.
3. Identify the game build (header).
4. Discover plugins: `anvil/plugins/*/plugin.toml` (stage 3: plus enabled
   mods' `anvil/` folders); broken manifests and missing entry DLLs are skipped
   with the error.
5. Resolve (API version, `anvil` version range, build, duplicates — first
   folder in name order wins, disabled ids, `[[expect]]` bytes, dependencies,
   cycles) and order.
6. Restore the entry point; load `early` plugins; start the main-phase thread
   (main plugins → `anvil.plugins_loaded` → status summary → SHA-256 check) and
   the ready watcher.

Settings (`anvil.toml`): `enabled = true`, `disabled_plugins = []`,
`log_level`. Environment variable `ANVIL_DISABLE=1` disables all plugins for a
single start (diagnostics).

## 8. Logging and status (D)

- `anvil/anvil.log` (UTF-8), rotated to `anvil.prev.log`. Contents: Anvil
  version, game build id (or "unknown" + header identity), per plugin
  `loaded` / `skipped (reason)` / `failed (reason)`, patch list per plugin,
  timings of discovery and loading.
- Lines logged before the log file is opened (e.g. from `DllMain`) are kept
  in memory and written when it opens at the entry point. Plugin messages are
  tagged with the plugin id. `log_level` in `anvil.toml` filters.
- Stage 4: in-game overlay with the same information.

## 9. Stage 3 — plugins from enabled mods / Steam Workshop (D, later)

- Source: mods enabled in the current playset (`dlc_load.json` in the EU4 user
  folder → `.mod` descriptors → `path=`), each mod's `anvil/` subfolder with
  the same layout as `anvil/plugins/<id>/`. Not merely subscribed items.
- Trust: a Workshop plugin version runs only after approval, recorded in
  `anvil/trust.toml` as (plugin id, mod id, SHA-256 of entry DLL and manifest).
  A new version needs a new approval; until then the previously approved
  version stays (copied to `data/anvil/approved/`) or nothing runs.
- Early plugins load before the game window exists, and a modal question at
  that moment would block the start: the first start records the plugin as
  pending and asks once the game is running; it loads from the next start
  (stage 3a). A separate "Anvil Manager"
  for approving before launch is stage 4.
- To verify before publishing: whether a mod containing only `anvil/` changes
  the game checksum (ironman/achievements), and Workshop/Paradox rules for
  DLLs in mods.

## 10. Security

Plugins are native code with the game's permissions. Anvil never downloads
anything. Local plugins are trusted by being placed in the folder; Workshop
plugins need per-version approval. Releases are built by GitHub Actions with
published SHA-256 hashes; code signing is planned (proxy DLLs are a pattern
antivirus heuristics look at).

## 11. Build, test, release

- CMake + Ninja or Visual Studio generator, MSVC (Visual Studio 2022 Build
  Tools, x64) as the reference toolchain; C++23 inside Anvil (SafetyHook), C
  ABI at the boundary; static CRT so `version.dll` needs no redistributable.
- Tests (`tests/test_main.cpp`): SemVer and ranges, manifest parsing and
  errors, resolver (dependencies, optional, cycles, phases, builds, API,
  duplicates, disabled, verification hook), patch registry (bytes, conflicts,
  rollback, call redirection, SafetyHook hook, import replacement,
  `expect_bytes` with masks, bounds, unreadable pages, other owners' patches
  and the wrong-phase rule), `[[expect]]` patterns and per-build selection,
  services/events, and the
  built proxy forwarding all 17 exports. Game-level check (lab): vanilla copy,
  proxy without `anvil/`, empty `plugins`, example plugin.
- GitHub Actions on `windows-2022` for every pull request and push to `main`:
  build, tests, package zip (`version.dll`, `anvil/` skeleton, README,
  LICENSE, SHA256SUMS). Actions are pinned to commit SHAs (Dependabot).
- Releases (D): Conventional Commits in squash-merged pull request titles;
  release-please maintains a release pull request that bumps `version.txt`
  (the single version source) and `CHANGELOG.md`; merging it tags `vX.Y.Z`,
  after which the release workflow builds the tag, attaches the zip and
  checksums with a build provenance attestation, and triggers the website.
  Details in `CONTRIBUTING.md`.
- Versioning: SemVer for Anvil; the plugin API has its own major version.
  Within API 1, functions and fields are only appended (`ANVIL_HOST_HAS`) and
  manifest keys only added; a plugin that needs something added later states
  `anvil = ">=x.y.z"`, and an older Anvil skips it with the reason instead of
  loading it without the feature (an unknown manifest key, such as
  `[[expect]]` on an Anvil that predates it, would otherwise be ignored).

## 12. Stages

1. Core: proxy, discovery of local plugins, manifest, build identity, log,
   patch API with registry, services/events, sample plugin. **Done** (0.1.0).
2. First real plugin: `eu4-startup-boost` (separate repository).
3. Plugins from enabled mods + per-version approval.
4. In-game overlay, Anvil Manager, code signing.

## 13. Open questions (Q)

- Decided: SafetyHook (§6); services, dependencies and events in API 1 (§6).
- Whether `version.dll` alone is enough for all launch paths (Steam, Paradox
  launcher, direct start) — direct start verified in the lab; Steam/launcher
  to be tested with a real install.
- Mid-function hooks (SafetyHook `MidHook`) in a later API minor: needs a
  stable C layout for the register context.
