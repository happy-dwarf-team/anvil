# How other game plugin loaders solve addresses, hot reload and managers

Research for [issue #15](https://github.com/happy-dwarf-team/anvil/issues/15)
(map [#12](https://github.com/happy-dwarf-team/anvil/issues/12)). Sources were
read on 2026-10-19. Links to code are pinned to the commit that was read, so
line numbers stay valid.

**How to read this file.** Sections marked *Verified* only state what the
linked primary source (official repository, source code or official docs)
says. Sections marked *Inference* are my reading of those facts and are not
claims made by the projects. The last section, *Ideas for Anvil 2*, is a list
of suggestions, not decisions.

**Gaps.** Nexus Mods pages (Address Library, Crash Logger SSE mod pages)
returned HTTP 403 to the fetcher, so nothing here quotes them; Address Library
behaviour is taken from the code that consumes it (SKSE64, CommonLibSSE-NG,
Crash Logger SSE) and from the Address Library author's GitHub repository.
Community opinion is only cited where it appears in an issue or in the
projects' own text; forum and Discord sentiment was not used.

## Summary

| Loader | Address declaration | One declaration feeds code *and* checks? | Hot reload | Crash attribution | Manager / in-game UI |
|---|---|---|---|---|---|
| SKSE64 / F4SE + Address Library + CommonLib | Stable numeric ID per function/global, resolved through a per-game-version database file; per-plugin flags say *how* the plugin finds addresses | No. ID resolves the address; a byte pattern is a separate, optional declaration | No | SKSE wraps plugin load calls in SEH and names the crashing plugin; Crash Logger SSE (separate plugin) prints frames as module+offset and game frames as Address Library ID+offset | No first-party manager; SKSE shows a message box listing failed plugins with reasons |
| Reloaded-II | Signature scanning at start-up (recommended), or hard-coded addresses | No | Yes, opt-in per mod (`CanUnload`/`CanSuspend`), from the launcher or a remote API | Crash handler writes a minidump plus `info.json` with enabled/running mods and the faulting module | Full launcher: install, update, configure (live), mod sets, process list with unload buttons |
| BepInEx | Managed: patch by type/method name (Harmony); no addresses | n/a | Dev tool ScriptEngine reloads `BepInEx\scripts` on F6 | Exceptions logged with the plugin's log source; no native crash report in core | ConfigurationManager (F1) renders every plugin's config automatically |
| MelonLoader | Managed: patch by name (Harmony); no addresses | n/a | No mod reload; preferences file is watched and reloaded | Event exceptions logged under the owning mod's logger; each assembly's SHA-256 logged | Console + `Loader.cfg`; installer app |
| Ultimate ASI Loader | None in the loader; plugins use byte patterns (`Hooking.Patterns`) | No | Yes for `.cxx` source snippets (compile, unload, restore bytes) | `CrashDumps` folder: `.log` names the plugin, module+offset, IDA address, unique byte pattern; `.dmp`; `.zip` | None (ini file) |

## 1. SKSE64 / F4SE, Address Library and CommonLibSSE(-NG)

### Verified

**Plugin declares how it finds addresses.** An SKSE64 plugin exports a
`SKSEPluginVersionData` structure with name, author, support e-mail, and two
bit fields: `versionIndependence` (`kVersionIndependent_AddressLibraryPostAE`,
`kVersionIndependent_Signatures` — "NO HARDCODED ADDRESSES",
`kVersionIndependent_StructsPost629`) and `versionIndependenceEx`
(`kVersionIndependentEx_NoStructUse`, `kVersionIndependentEx_AddressLibraryV5`),
plus a zero-terminated `compatibleVersions[16]` list of exact runtime versions
([PluginAPI.h L296–L340](https://github.com/ianpatt/skse64/blob/25b72352adb6543fa6d0bd3795780672b2e238e0/skse64/PluginAPI.h#L296-L340)).
F4SE has the same idea split into `addressIndependence` and
`structureIndependence`, with one flag per Address Library generation
(`kAddressIndependence_AddressLibrary_1_10_980`, `…_1_11_137`)
([F4SE PluginAPI.h L297–L327](https://github.com/ianpatt/f4se/blob/6f6a7caa9aeaebc957a70d934b6ab6b80eb768b2/f4se/PluginAPI.h#L297-L327)).
Address independence and structure-layout independence are declared
separately.

**SKSE turns the declaration into a precise reason.** The loader checks the
flags against the running game and the Address Library file and produces
statuses such as `"disabled, address library needs to be updated"`,
`"disabled, incompatible with current version of the game"`,
`"must be recompiled for new address library"` and
`"disabled, only compatible with versions earlier than 1.6.629"`
([PluginManager.cpp L444–L626](https://github.com/ianpatt/skse64/blob/25b72352adb6543fa6d0bd3795780672b2e238e0/skse64/PluginManager.cpp#L444-L626)).
After loading, errored plugins are collected into one message box that starts
"A DLL plugin has failed to load correctly…" and lists each DLL with its reason
and error code; a hard-coded table maps unclear DLL names to user-facing names
because "some plugins have non-descriptive names resulting in bad bug reports"
([L658–L720](https://github.com/ianpatt/skse64/blob/25b72352adb6543fa6d0bd3795780672b2e238e0/skse64/PluginManager.cpp#L658-L720)).
Every load call goes through `SafeCallLoadPlugin`, an SEH `__try` that turns a
crash into `"disabled, fatal error occurred while loading plugin"`, and a crash
while dispatching a post-load message is attributed with
`"crashed during postload"`
([L456–L470](https://github.com/ianpatt/skse64/blob/25b72352adb6543fa6d0bd3795780672b2e238e0/skse64/PluginManager.cpp#L456-L470),
[L770–L790](https://github.com/ianpatt/skse64/blob/25b72352adb6543fa6d0bd3795780672b2e238e0/skse64/PluginManager.cpp#L770-L790)).

**Address Library = ID → offset database per game version.** CommonLibSSE-NG
opens `Data/SKSE/Plugins/version-<ver>.bin` / `versionlib-<ver>.bin` (or a CSV
for VR) for the running version
([ID.h L300–L320](https://github.com/CharmedBaryon/CommonLibSSE-NG/blob/b93280e832f263dbef44e44cbe2936622a02f91a/include/REL/ID.h#L300-L320)).
A missing file fails with "you are missing the address library for this
specific version of the game"
([ID.cpp L121–L128](https://github.com/CharmedBaryon/CommonLibSSE-NG/blob/b93280e832f263dbef44e44cbe2936622a02f91a/src/REL/ID.cpp#L121-L128));
a missing ID fails with "this script extender plugin is incompatible with the
address library for this version of the game"
([ID.h L186–L197](https://github.com/CharmedBaryon/CommonLibSSE-NG/blob/b93280e832f263dbef44e44cbe2936622a02f91a/include/REL/ID.h#L186-L197)).
The ID space itself changed between SE (1.5.x) and AE (1.6.x), and the file
encoding changed again for 1.7.99 (SKSE 2.3.0/2.3.1 "better compatibility
reporting for plugins that need to be rebuilt due to the address library
change", [whatsnew](https://github.com/ianpatt/skse64/blob/25b72352adb6543fa6d0bd3795780672b2e238e0/skse64_whatsnew.txt#L1-L10)).
SKSE detects old plugins that lack the V5 flag by their PE build timestamp
([L563–L575](https://github.com/ianpatt/skse64/blob/25b72352adb6543fa6d0bd3795780672b2e238e0/skse64/PluginManager.cpp#L563-L575)).

**Address Library tooling.** The author's
[AddressLibraryDatabase](https://github.com/meh321/AddressLibraryDatabase/blob/0379cb6fdaa0d68e69ef9ad1a2d39caa6611ac91/README.md)
holds a `relib` file (ID ↔ offset pairs for each game version) and a `rename`
file (ID ↔ name). The Address Library Manager loads it, dumps ID/offset pairs
for a version, builds the Address Library binaries, and imports/exports
function names from/to IDA, so reverse engineers share names keyed by ID.

**How one declaration reaches code (CommonLibSSE-NG).** A function is declared
once as an ID (or ID per runtime) and wrapped in a typed relocation:

```cpp
constexpr auto PopulateHitDataID = RELOCATION_ID(42832, 44001); // SE/VR id, AE id
REL::Relocation<int32_t*(Actor*, char*)> PopulateHitData(PopulateHitDataID, 0x42);
```

`REL::RelocationID`, `REL::VariantID` (SE id, AE id, raw VR offset),
`REL::VariantOffset`, `REL::Relocate(se, ae[, vr])` and
`REL::RelocateMember<T>(this, seAeOffset, vrOffset)` choose per runtime at
run time, so one DLL serves SE, AE and VR
([wiki: Runtime Targeting](https://github.com/CharmedBaryon/CommonLibSSE-NG/wiki/Runtime-Targeting)).
`REL::Relocation` also writes the hooks (`write_branch`, `write_call`,
`write_vfunc`) through the SKSE trampoline
([Relocation.h L358–L405](https://github.com/CharmedBaryon/CommonLibSSE-NG/blob/b93280e832f263dbef44e44cbe2936622a02f91a/include/REL/Relocation.h#L358-L405)).
Byte verification is a *separate* compile-time pattern,
`REL::make_pattern<"40 10 F2 ??">().match_or_fail(addr)`, whose failure text
says the plugin "is incompatible with the current version of the game"
([Pattern.h L123–L135](https://github.com/CharmedBaryon/CommonLibSSE-NG/blob/b93280e832f263dbef44e44cbe2936622a02f91a/include/REL/Pattern.h#L123-L135)).
Struct field offsets are not Address Library IDs: they are written in the
library's headers (per runtime through `REL::RelocateMember`), and SKSE asks
for a separate struct-layout flag.

**Metadata is generated from the build file.** In CommonLibSSE-NG,
`add_commonlibsse_plugin` in CMake (or the `SKSEPluginInfo(...)` macro)
generates `SKSEPlugin_Version` and `SKSEPlugin_Query`; Address Library use is
the default ([README](https://github.com/CharmedBaryon/CommonLibSSE-NG/blob/b93280e832f263dbef44e44cbe2936622a02f91a/README.md),
[wiki](https://github.com/CharmedBaryon/CommonLibSSE-NG/wiki/Runtime-Targeting)).
The "declarative" mode generates `SKSEPlugin_Load` and lets any source file
register prioritized `OnSKSEPluginLoad(n)` / `OnSKSEMessage` handlers
([wiki: Declarative Plugins](https://github.com/CharmedBaryon/CommonLibSSE-NG/wiki/Declarative-Plugins)).
The libxse template does the same with an xmake rule:
`add_rules("commonlibsse.plugin", { name = …, author = …, description = … })`
([xmake.lua](https://github.com/libxse/commonlibsse-template/blob/ba4bbe5f877f752aa6fc9e9a665d6544079ca2d0/xmake.lua)),
and deploys to a mod-manager folder or the game through `XSE_TES5_MODS_PATH`
/ `XSE_TES5_GAME_PATH`.

**Template and tests.** The
[CommonLibSSE sample plugin](https://gitlab.com/colorglass/commonlibsse-sample-plugin)
(CMake + vcpkg) generates `version.rc` and `PluginInfo.h` from the CMake
project, deploys DLL+PDB to `SkyrimPluginTargets` (a list, one per runtime) or a
FOMOD folder, and ships Catch2 unit tests run by CTest. CommonLibSSE-NG can run
plugin code outside the game: `REL::Module::inject(path)` loads `SkyrimSE.exe`
into the test process "but not run!", and `REL::IDDatabase::load()` loads a
chosen Address Library file; tests are tagged `[integration]` (needs the image)
or `[e2e]` (needs an initialised engine)
([wiki: Unit Testing](https://github.com/CharmedBaryon/CommonLibSSE-NG/wiki/Unit-Testing)).
The same page warns that namespace-level `REL::Relocation` objects force the
game module to load at static initialisation.

**Crash Logger SSE** (a separate SKSE plugin, built on CommonLibSSE-NG) installs
both `SetUnhandledExceptionFilter` and a vectored handler, the latter because
`__fastfail` and `/GS` failures bypass the filter
([CrashHandler.cpp L1786–L1831](https://github.com/alandtse/CrashLoggerSSE/blob/b4f4bc5fc30cc41c373553f4571617ddcbad51bc/src/Crash/CrashHandler.cpp#L1786-L1831)).
The log has PROBABLE CALL STACK, REGISTERS, STACK, MODULES, SKSE PLUGINS (with
file versions), game PLUGINS and POSSIBLE RELEVANT OBJECTS sections
([L285](https://github.com/alandtse/CrashLoggerSSE/blob/b4f4bc5fc30cc41c373553f4571617ddcbad51bc/src/Crash/CrashHandler.cpp#L285),
[L849](https://github.com/alandtse/CrashLoggerSSE/blob/b4f4bc5fc30cc41c373553f4571617ddcbad51bc/src/Crash/CrashHandler.cpp#L849),
[L978–L1023](https://github.com/alandtse/CrashLoggerSSE/blob/b4f4bc5fc30cc41c373553f4571617ddcbad51bc/src/Crash/CrashHandler.cpp#L978-L1023)).
Frames inside the game are printed as `module+offset -> <AddressLibraryID>+0x<delta>`
using a reverse `REL::Offset2ID` table
([ModuleHandler.cpp L170–L183](https://github.com/alandtse/CrashLoggerSSE/blob/b4f4bc5fc30cc41c373553f4571617ddcbad51bc/src/Crash/Modules/ModuleHandler.cpp#L170-L183)),
and PDBs are read through `msdia140.dll` when present
([README](https://github.com/alandtse/CrashLoggerSSE/blob/b4f4bc5fc30cc41c373553f4571617ddcbad51bc/README.md)).

**Hot reload and managers.** Nothing in the SKSE64 readme or plugin API
supports unloading a plugin. SKSE ships no manager; the official site says
"SKSE will support the latest version of Skyrim available on Steam, and only
this version" and, after a game patch, "Mods using plugins probably need to be
updated" ([skse.silverlock.org](https://skse.silverlock.org/)).

**Ecosystem state.** CommonLib exists as several forks with different build
systems and activity: Ryan-rsm-McKenzie/CommonLibSSE (last push 2023-07),
CharmedBaryon/CommonLibSSE-NG (last push 2024-09), alandtse/CommonLibVR (NG
branch default, active 2026-10), powerof3/CommonLibSSE and libxse/commonlibsse
(xmake, active 2026-10). powerof3's README announces a move from MIT to
GPL-3.0-or-later with a modding exception, calling it "a breaking change"
([README](https://github.com/powerof3/CommonLibSSE/blob/970373e47e163fac51c93b48691bdd19d5e15d1f/README.md)).

**Author complaints found in primary sources.**
- [CommonLibSSE-NG #106](https://github.com/CharmedBaryon/CommonLibSSE-NG/issues/106):
  the 3.7.0 release stopped working after "Address Library Team has changed
  the format"; the reporter cannot move to the C++23 forks.
- [CommonLibSSE-NG #111](https://github.com/CharmedBaryon/CommonLibSSE-NG/issues/111):
  a struct field documented at `0x118` is really at `0x120` on AE, found by
  byte-diffing live memory; the header has one unconditional `static_assert`.
- [CommonLibSSE-NG #112](https://github.com/CharmedBaryon/CommonLibSSE-NG/issues/112):
  a data race in `REL::Module::get()` initialisation.

### Inference

- The ID scheme moves the "address changed" problem from every plugin to one
  shared database, but the database itself changed generation twice (SE→AE IDs,
  then V5 encoding), and each change forced recompiles and new flags. Address
  IDs only solve addresses; struct layouts and calling conventions still break
  separately (see F4SE's separate `structureIndependence` field and NG #111).
- Nobody in this ecosystem derives byte checks from the address declaration:
  IDs and patterns are written separately, and most plugins only rely on the
  ID lookup succeeding.
- The most reusable parts for Anvil are the *declaration of how a plugin finds
  addresses* (turned into a specific user-facing reason), the aggregated error
  dialog, and the crash log that translates game frames into shared symbolic
  names.

## 2. Reloaded-II

### Verified

- **Addresses.** For games that keep updating, Reloaded recommends that shared
  libraries rediscover addresses with signature scans "during startup of the
  application", because many mods "break instantly every update"; the goal is
  that "as long as the shared library works, all mods work"
  ([Reloaded.SharedLib.Hooks](https://github.com/Sewer56/Reloaded.SharedLib.Hooks/blob/86bc82948aa6f2670cf8a2aee19f0e752ffc4c65/README.md#supporting-actively-developed-applications)).
  The cheat sheet uses `Reloaded.Memory.SigScan` and throws when
  `!result.Found`
  ([Signature Scanning](https://reloaded-project.github.io/Reloaded-II/CheatSheet/SignatureScanning/));
  functions are declared as delegates with a `[Function(CallingConventions…)]`
  attribute and hooked with `hooks.CreateHook<T>(impl, address).Activate()`
  ([Calling & Hooking](https://reloaded-project.github.io/Reloaded-II/CheatSheet/CallingHookingGameFunctions/)).
- **Dependency injection.** A mod lists `ModDependencies` in `ModConfig.json`;
  Reloaded orders loading so dependencies start first; the consumer references
  the publisher's `<ModId>.Interfaces` NuGet package and calls
  `IModLoader.GetController<T>()`, keeping only a `WeakReference` and checking
  `TryGetTarget` before every use, because "If the mod we got IController from
  is unloaded, `TryGetTarget` will fail"
  ([How to Consume](https://reloaded-project.github.io/Reloaded-II/DependencyInjection_Consumer/)).
- **Hot reload.** Mods "can be loaded and unloaded at runtime, including
  remotely through an API" ([home](https://reloaded-project.github.io/Reloaded-II/));
  it is opt-in: the loader never calls `Unload`/`Suspend` unless `CanUnload()` /
  `CanSuspend()` return true
  ([IModV1.cs L40–L60](https://github.com/Reloaded-Project/Reloaded-II/blob/458987f26bc8d78fdc0f86b73fc0d6fd4b673d01/source/Reloaded.Mod.Interfaces/Internal/IModV1.cs#L40-L60)).
  ReadyToRun builds are incompatible with unloading
  ([Mod Template](https://reloaded-project.github.io/Reloaded-II/ModTemplate/)).
  The launcher's process list greys out the unload button for mods that cannot
  unload ([How to Consume](https://reloaded-project.github.io/Reloaded-II/DependencyInjection_Consumer/)).
  The server mod allows load/unload "in real time", remote connections only
  after the user enables them
  ([Server](https://reloaded-project.github.io/Reloaded-II/InternalLibrariesServer/)).
- **Native mods.** A C/C++ DLL is loaded through `ModConfig.json`
  (`ModNativeDll32`/`ModNativeDll64`); the loader tries the exports
  `ReloadedStartEx`, `ReloadedStart`, `InitializeASI`, `Init`; unload support
  needs `ReloadedCanUnload`/`ReloadedUnload` etc., and the docs strongly advise
  against it unless hooks can be stacked and deactivated safely; native support
  is described as limited ([Native Mods](https://reloaded-project.github.io/Reloaded-II/NativeMods/)).
- **Template and packaging.** The .NET template generates `Mod.cs`,
  `Config.cs`, `ModConfig.json`, `Publish.ps1` (with delta updates for GitHub
  or NuGet), `BuildLinked.ps1` (trimmed build to test), and a GitHub workflow
  that builds for GameBanana, GitHub and NuGet on a tag
  ([Mod Template](https://reloaded-project.github.io/Reloaded-II/ModTemplate/)).
- **Crashes.** The loader's crash hook writes `dump.dmp`, a copy of the log and
  `info.json` into `CrashDumps/<log name>/`, then shows a message box with the
  crash address and faulting module and opens the folder
  ([EntryPoint.cs L233–L287](https://github.com/Reloaded-Project/Reloaded-II/blob/458987f26bc8d78fdc0f86b73fc0d6fd4b673d01/source/Reloaded.Mod.Loader/EntryPoint.cs#L233-L287)).
  `info.json` contains `EnabledMods`, `RunningMods`, `CrashAddress` and
  `FaultingModulePath`
  ([CrashDumpInfo.cs](https://github.com/Reloaded-Project/Reloaded-II/blob/458987f26bc8d78fdc0f86b73fc0d6fd4b673d01/source/Reloaded.Mod.Loader/Logging/Structs/CrashDumpInfo.cs));
  old dumps are deleted after `CrashDumpDeleteHours = 24`
  ([LoaderConfig.cs L114–L116](https://github.com/Reloaded-Project/Reloaded-II/blob/458987f26bc8d78fdc0f86b73fc0d6fd4b673d01/source/Reloaded.Mod.Loader.IO/Config/LoaderConfig.cs#L114-L116)).
- **Manager.** The launcher installs runtimes, auto-updates loader and mods
  (delta updates), edits mod configuration with changes applied in real time,
  keeps mod sets, supports several games, never modifies game files, and logs
  to a console and a file in real time
  ([home](https://reloaded-project.github.io/Reloaded-II/)).

### Inference

- Reloaded's model is the closest to Anvil's services: a typed interface,
  declared dependency, loader-ordered start. Its weak-reference rule is the
  price of supporting unload; Anvil would need an equivalent rule (or forbid
  unloading service publishers) before offering hot reload.
- The crash report names the faulting *module*, not the plugin that patched the
  code that crashed; a crash in game code after a hook is attributed to the game.

## 3. BepInEx

### Verified

- **Declaration.** A plugin class carries
  `[BepInPlugin(GUID, Name, Version)]`; the GUID "absolutely must be unique" and
  is used for duplicates and dependency sorting. The template
  (`dotnet new bepinex5plugin -n … -T <TFM> -U <Unity>`, from the BepInEx NuGet
  feed) generates a `PluginInfo` class from the `.csproj`, so the version lives
  in one place ([Plugin tutorial](https://docs.bepinex.dev/articles/dev_guide/plugin_tutorial/2_plugin_start.html)).
  Game code is patched by type and method name through Harmony, so there are
  no addresses to declare (managed games).
- **Load errors.** The chainloader skips older duplicates ("a newer version
  exists"), process-filtered plugins, incompatible plugins, and plugins with
  missing dependencies (with minimum versions), and keeps every message in a
  public `Chainloader.DependencyErrors` list
  ([Chainloader.cs L89, L320–L430](https://github.com/BepInEx/BepInEx/blob/f4c1b1103a32884b7440d9681c60cc5d619f284f/BepInEx/Bootstrap/Chainloader.cs#L320-L430)).
- **Logging.** Each plugin's `Logger` tags lines with level and source, e.g.
  `[Warning: Logger Test]`, and goes to the console, `BepInEx/LogOutput.log` and
  Unity's log ([Logging](https://docs.bepinex.dev/articles/dev_guide/plugin_tutorial/3_logging.html)).
- **Configuration.** `Config.Bind(section, key, default, description)` creates
  `BepInEx\config\<GUID>.cfg` on first run
  ([Configuration](https://docs.bepinex.dev/articles/dev_guide/plugin_tutorial/4_configuration.html)).
  ConfigurationManager (F1 in game) "will automatically display all settings
  from your plugin's `Config`", using metadata such as `AcceptableValueRange`
  (slider) or enums (drop-down) and `KeyboardShortcut` settings
  ([README](https://github.com/BepInEx/BepInEx.ConfigurationManager/blob/7b25a861e602d756c99ba1818b19d9fd4691b08a/README.md)).
- **Hot reload.** The debug tool ScriptEngine loads and reloads plugins from
  `BepInEx\scripts` on F6; the README warns that Harmony patches and loose
  GameObjects remain after the old version is destroyed unless the plugin
  cleans up in `OnDestroy`
  ([BepInEx.Debug](https://github.com/BepInEx/BepInEx.Debug/blob/8f3c71b3cc4c0d179ec128f20fddaae0b187b065/README.md)).
  The same repo has DemystifyExceptions (readable stack traces) and a simple
  Mono profiler.

### Inference

- The single most transferable idea is *settings as data*: because every
  plugin declares settings with type, range and description through one API,
  a generic UI can configure all plugins without plugin code. That maps
  directly onto an Anvil overlay or Manager.

## 4. MelonLoader

### Verified

- Uses a `version.dll` proxy, like Anvil (install step "Extract version.dll &
  dobby.dll"); folders `Mods`, `Plugins`, `UserData`, `MelonLoader/Logs`; a
  TOML `UserData/Loader.cfg` whose every key mirrors a `--melonloader.*` launch
  option (console, debug, log count, debugger wait)
  ([README](https://github.com/LavaGang/MelonLoader/blob/5aff4d9d7c16ae153aad2f1035114dbc2713bf99/README.md)).
- Metadata attributes include `MelonInfo`, `MelonGame`, `MelonGameVersion`,
  `MelonProcess`, `MelonPlatform`, `MelonIncompatibleAssemblies`,
  `MelonOptionalDependencies`, `VerifyLoaderVersion`, `VerifyLoaderBuild`
  ([Attributes](https://github.com/LavaGang/MelonLoader/tree/5aff4d9d7c16ae153aad2f1035114dbc2713bf99/MelonLoader/Attributes)).
  An incompatible mod gets a block listing the accepted games, game versions,
  processes, platforms and loader versions
  ([MelonBase.cs L325–L376](https://github.com/LavaGang/MelonLoader/blob/5aff4d9d7c16ae153aad2f1035114dbc2713bf99/MelonLoader/Melons/MelonBase.cs#L325-L376)).
- Each loaded mod is printed with name, author, version and assembly file, and
  each assembly's SHA-256
  ([MelonBase.cs L594–L598](https://github.com/LavaGang/MelonLoader/blob/5aff4d9d7c16ae153aad2f1035114dbc2713bf99/MelonLoader/Melons/MelonBase.cs#L594-L598),
  [MelonAssembly.cs L157](https://github.com/LavaGang/MelonLoader/blob/5aff4d9d7c16ae153aad2f1035114dbc2713bf99/MelonLoader/Melons/MelonAssembly.cs#L157)).
- An exception in an event callback is caught and logged through the owning
  mod's logger, and the other subscribers still run
  ([MelonEvent.cs L134–L153](https://github.com/LavaGang/MelonLoader/blob/5aff4d9d7c16ae153aad2f1035114dbc2713bf99/MelonLoader/Melons/Events/MelonEvent.cs#L134-L153)).
- No mod hot reload was found in the source; the preferences file is watched
  and re-read when edited
  ([Watcher.cs L68–L84](https://github.com/LavaGang/MelonLoader/blob/5aff4d9d7c16ae153aad2f1035114dbc2713bf99/MelonLoader/Preferences/IO/Watcher.cs#L68-L84)).

### Inference

- Logging a hash per loaded module is a cheap way to make bug reports
  reproducible ("which exact DLL was this?"); Anvil already hashes the game.

## 5. Ultimate ASI Loader (baseline)

### Verified (readme as of 2026-10-15)

- A proxy DLL under many system names (`version`, `dinput8`, `winmm`, …) loads
  `.asi` files from the game folder and `scripts`, `plugins`, `update`; options
  live in an ini; `DebugLog=1` writes `<loader>.log`. The only plugin contract
  is an optional export `InitializeASI`, called after `LoadLibrary` outside the
  loader lock
  ([readme L100–L147](https://github.com/ThirteenAG/Ultimate-ASI-Loader/blob/f9d13268f456076c9ff77201ed5a552e77ec559d/readme.md#L100-L147)).
  The `PluginTemplate` demo shows ini settings, a log file, an inline hook
  (safetyhook) and a byte-pattern search (`Hooking.Patterns`).
- **`.cxx` snippets.** Plain C++ files are compiled by the loader at start (no
  SDK); with `CxxHotReload=1` a saved file is recompiled, and only if it
  compiles is the old version unloaded: `Shutdown()` runs, its hooks are
  removed and "every byte it changed" through the injector helpers is restored.
  A crash in `Init` unloads that snippet and undoes its writes; a later crash is
  logged with snippet file, line and function
  ([readme L149–L173](https://github.com/ThirteenAG/Ultimate-ASI-Loader/blob/f9d13268f456076c9ff77201ed5a552e77ec559d/readme.md#L149-L173)).
- **Crash reports.** If a `CrashDumps` folder exists, each crash writes a
  readable `.log` that "names the plugin if the crash happened inside one",
  gives every address as module+offset (with function/line when symbols exist)
  and as an IDA address, adds a unique wildcard byte pattern for the crash site
  and each stack frame, and lists exception, registers, memory, threads, loaded
  plugins and modules; plus a `.dmp` and a `.zip` of both "to attach". The
  handler runs on a separate thread (stack overflows are captured) and keeps
  the last `CrashDumpMaxReports=10`
  ([readme L291–L308](https://github.com/ThirteenAG/Ultimate-ASI-Loader/blob/f9d13268f456076c9ff77201ed5a552e77ec559d/readme.md#L291-L308)).

### Inference

- Even the "minimal" loader now ships the two features Anvil's map lists as
  later work (crash attribution and hot reload). Its hot reload works because
  the *loader* owns every memory write and can undo it — which is already true
  of Anvil's host-mediated patches.
- A byte pattern for each frame is a useful bridge for EU4: it lets someone with
  a different `eu4.exe` (or a disassembler) find the same place without an
  address-ID database.

## 6. Cross-cutting comparison

**Address declaration vs. verification.** No surveyed loader derives the
verification bytes from the address declaration. SKSE/CommonLib: ID (address)
+ optional `make_pattern` (bytes) + headers (struct offsets), three places.
Reloaded and UASI: the signature *is* the address, so "verification" is simply
"the scan found exactly one match". Managed loaders bind by name. Anvil today
writes RVA in code and bytes in `plugin.toml`, two places (design §4).

**Compatibility reasons.** SKSE (flags → specific status string, aggregated
dialog) and MelonLoader (attribute → list of accepted games/versions) give the
best user-facing explanations; BepInEx keeps a programmatic list of load
errors that a UI can show.

**Offline checking.** CommonLibSSE-NG is the only one with a documented way to
load the game image into a test process and resolve addresses without running
the game. None ships a stand-alone "check my plugins against this exe" CLI.

**Hot reload.** Exists where the host can undo the plugin's effects (UASI
snippets, Reloaded opt-in unload, BepInEx ScriptEngine with manual cleanup).
All three make it opt-in or dev-only and warn about leftovers.

**Crash attribution.** Levels seen: (1) SEH around the loader's own calls into
plugins (SKSE); (2) faulting module + enabled mod list (Reloaded);
(3) module+offset, symbol, plugin name, pattern, minidump zip (UASI, Crash
Logger). Only Crash Logger maps game frames to shared symbolic IDs.

**Managers / in-game UI.** Reloaded's launcher is the full-featured example
(install, update, configure live, sets, process list with unload). BepInEx's
ConfigurationManager is the in-game example (auto-generated settings UI).
SKSE has neither.

## Ideas for Anvil 2 (suggestions, not decisions)

1. **One symbol declaration, two outputs.** Let a plugin (or a shared table)
   declare each game dependency once — name, per-build RVA, expected bytes or
   pattern — and generate both the C constants the code uses and the
   `[[expect]]` entries the host checks (cf. CommonLib generating
   `SKSEPlugin_Version` from the build file). This removes the hand-kept pairing
   behind the 93-entry manifest.
2. **Shared, named EU4 symbol table.** An Anvil-owned table (name → RVA +
   bytes per build), like Address Library plus its names database, so several
   plugins reuse the same verified entry and crash logs can print symbol names.
3. **Declare how addresses are found.** A manifest field in the spirit of
   SKSE's `versionIndependence` (exact builds / patterns / symbol table) and a
   separate one for struct-layout dependence, turned into precise skip reasons.
4. **`anvil check` as an image loader.** Map `eu4.exe` as an image without
   running it (as CommonLibSSE-NG's `REL::Module::inject` does) and evaluate
   every manifest's expects and patterns offline; usable in plugin CI.
5. **Crash reports that use what Anvil already knows.** Top-level + vectored
   handler; report module+RVA, owning plugin for frames in plugin DLLs *and*
   for addresses inside patch ranges the host recorded per plugin, symbol name
   from the table, a byte pattern per frame, enabled plugins with hashes, and a
   zipped minidump; keep the last N.
6. **SEH around every host → plugin call** (entry, events, services), turning a
   crash into "plugin X failed in phase Y" and disabling it, as SKSE does for
   load and post-load messages.
7. **Opt-in hot reload for development.** A capability flag like Reloaded's
   `CanUnload`; the host reverts all patches it applied for that plugin (as UASI
   does) and refuses unload while other plugins hold its services.
8. **Settings as data.** Typed, ranged, described settings declared through the
   API or manifest, so the overlay and Manager render them without plugin UI
   (BepInEx ConfigurationManager), with file watching for live edits
   (MelonLoader).
9. **Aggregated, user-readable failure summary** (log, overlay, Manager): plugin
   display name, reason, what to do — SKSE's message box and MelonLoader's
   incompatibility block are the models.
10. **Template that keeps metadata in one place.** CMake template generating
    `plugin.toml` version/id and the DLL version resource from the project,
    deploying to the game's `anvil/plugins/<id>/` through an environment
    variable, with unit tests that run without the game.
