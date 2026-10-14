# Anvil — design

Status: draft 1 (2026-10-15). Decisions marked **D** are agreed; **Q** are open.

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
- Its `DllMain(DLL_PROCESS_ATTACH)` runs before the game's CRT initializes and
  before `main` — the same moment as an import added to the executable, which
  is what the startup plugin needs (e.g. replacing the CRT allocator).
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
id = "eu4-startup-boost"          # [a-z0-9-]+, unique
name = "EU4 Startup Boost"
version = "0.1.0"                 # SemVer
authors = ["Happy Dwarf Team"]
description = "Faster game start-up."
api = 1                           # Anvil plugin API major version
entry = "eu4_startup_boost.dll"
phase = "early"                   # "early" | "main"
affects_gameplay = false          # true = may change simulation (multiplayer/ironman warning)
load_after = []                   # plugin ids
game_builds = ["eu4-1.37.5-steam-win64"]
```

Parsing: toml++ (header-only, MIT). Unknown keys are ignored with a log note.

## 5. Game build identity (D)

Plugins patch code at fixed addresses, so they are tied to exact builds.

- At start Anvil reads the PE header of the running image (TimeDateStamp,
  SizeOfImage, CheckSum, entry point) and the file version resource, and maps
  them to a build id from a table compiled into Anvil (e.g.
  `eu4-1.37.5-steam-win64` → SHA-256 `9AD3EFE1…771A`).
- The full SHA-256 is computed only when the header identity is new and is
  cached in `anvil/data/anvil/build-cache.toml` keyed by file size + mtime, so
  normal starts do not hash 38 MB.
- Unknown build → plugins that list specific builds are skipped with a clear
  log line ("EU4 was updated; plugin X supports …"). A plugin may declare
  `game_builds = ["*"]` if it does not depend on addresses.

## 6. Plugin API (C ABI, header-only SDK) (D)

The SDK is a single versioned header `include/anvil/anvil_plugin.h`; plugin
repositories vendor a copy. Compiler-independent C ABI; plugins may be built
with MSVC or MinGW.

```c
#define ANVIL_API_VERSION 1

typedef struct AnvilHost {
    uint32_t size;                 /* sizeof(AnvilHost) as known by the host */
    uint32_t api_version;
    const AnvilGameInfo *game;     /* build id, image base/size, ready marker (if known) */
    const wchar_t *plugin_dir;     /* read-only */
    const wchar_t *data_dir;       /* writable, created on demand */
    void (*log)(AnvilPluginHandle, AnvilLogLevel, const char *fmt, ...);
    /* code patching — all changes to game memory go through these */
    AnvilResult (*patch_bytes)(AnvilPluginHandle, uintptr_t rva, const void *expected,
                               const void *replacement, size_t size);
    AnvilResult (*redirect_call)(AnvilPluginHandle, uintptr_t call_rva,
                                 uintptr_t expected_target_rva, void *new_target);
    AnvilResult (*hook_function)(AnvilPluginHandle, uintptr_t rva, const void *expected_prologue,
                                 size_t prologue_size, void *detour, void **original);
    AnvilResult (*replace_import)(AnvilPluginHandle, const char *module, const char *function,
                                  void *replacement, void **original);
    void (*on_game_ready)(AnvilPluginHandle, void (*callback)(void *), void *context);
} AnvilHost;

/* exported by every plugin */
ANVIL_EXPORT AnvilResult anvil_plugin_load(const AnvilHost *host, AnvilPluginHandle self);
ANVIL_EXPORT void anvil_plugin_unload(void);   /* optional; process exit */
```

Rules:

- `phase = "early"`: `anvil_plugin_load` runs inside Anvil's `DllMain` (loader
  lock held). Allowed: memory patches, IAT replacement, `VirtualAlloc`,
  `CreateThread` (thread starts after the lock is released), reading small
  files. Not allowed: loading DLLs that are not yet loaded, COM/shell calls,
  waiting on other threads, UI.
- `phase = "main"`: runs on an Anvil thread right after the loader lock is
  released, before or concurrently with game initialization; no UI either.
- `on_game_ready`: called once when the game reports initialization finished
  (known builds only).
- The host checks `expected` bytes/targets before every patch and refuses
  patches that overlap another plugin's patch (hook registry), logging both
  plugin ids. Patches are applied atomically per call.
- Trampolines and stubs are allocated by the host in executable memory within
  ±2 GB of the image (no reuse of `int3` padding inside `eu4.exe`). Function
  hooks use a vetted library (MinHook or SafetyHook — **Q**, decide when
  implementing).
- A plugin returning an error, or an exception/SEH fault inside
  `anvil_plugin_load`, marks the plugin failed; patches it already applied are
  rolled back when possible, otherwise the plugin is marked "partially applied"
  in the log.

## 7. Load order and failure policy (D)

1. Proxy setup (always). If anything below fails, Anvil stays a pure proxy.
2. Read `anvil/anvil.toml` (optional), identify the game build.
3. Discover plugins: `anvil/plugins/*/plugin.toml` (stage 3: plus enabled
   mods' `anvil/` folders).
4. Validate manifests (API version, build, duplicate ids, entry file present),
   resolve `load_after` (cycles → skip the involved plugins).
5. Load `early` plugins in order inside `DllMain`; schedule `main` plugins.
6. Write the status summary to the log.

Settings (`anvil.toml`): `enabled = true`, `disabled_plugins = []`,
`log_level`. Environment variable `ANVIL_DISABLE=1` disables all plugins for a
single start (diagnostics).

## 8. Logging and status (D)

- `anvil/anvil.log` (UTF-8), rotated to `anvil.prev.log`. Contents: Anvil
  version, game build id (or "unknown" + header identity), per plugin
  `loaded` / `skipped (reason)` / `failed (reason)`, patch list per plugin,
  timings of discovery and loading.
- Logging in `DllMain` uses a preallocated buffer flushed by the main-phase
  thread.
- Stage 4: in-game overlay with the same information.

## 9. Stage 3 — plugins from enabled mods / Steam Workshop (D, later)

- Source: mods enabled in the current playset (`dlc_load.json` in the EU4 user
  folder → `.mod` descriptors → `path=`), each mod's `anvil/` subfolder with
  the same layout as `anvil/plugins/<id>/`. Not merely subscribed items.
- Trust: a Workshop plugin version runs only after approval, recorded in
  `anvil/trust.toml` as (plugin id, mod id, SHA-256 of entry DLL and manifest).
  A new version needs a new approval; until then the previously approved
  version stays (copied to `data/anvil/approved/`) or nothing runs.
- Because early plugins run in `DllMain`, approval cannot be asked at that
  moment: first start records the plugin as pending and asks once the game is
  running; it loads from the next start (stage 3a). A separate "Anvil Manager"
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

- CMake, MSVC (Visual Studio 2022, x64) as the reference toolchain; C++20
  inside Anvil, C ABI at the boundary.
- Tests: manifest parsing, build identity, load-order resolution, hook
  registry overlap rules, export forwarding (a test executable calling the
  proxy). Game-level check: the game starts with an empty `plugins` folder and
  with a sample plugin (manual + lab harness).
- GitHub Actions on `windows-latest`: build, tests, release zip
  (`version.dll`, `anvil/` skeleton, README, LICENSE, SHA256SUMS).
- Versioning: SemVer for Anvil; the plugin API has its own major version.

## 12. Stages

1. Core: proxy, discovery of local plugins, manifest, build identity, log,
   patch API with registry, sample plugin.
2. First real plugin: `eu4-startup-boost` (separate repository).
3. Plugins from enabled mods + per-version approval.
4. In-game overlay, Anvil Manager, code signing.

## 13. Open questions (Q)

- MinHook vs SafetyHook for function hooks.
- Whether `version.dll` alone is enough for all launch paths (Steam,
  Paradox launcher, direct start) — to be tested.
- Should a plugin be able to expose services to other plugins (registry of
  named interfaces) in API v1, or later?
