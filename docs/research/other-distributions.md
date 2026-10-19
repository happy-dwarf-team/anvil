# Other distributions of EU4 1.37.5 (GOG, Microsoft Store, Epic) and how to identify them

Research note for [issue #36](https://github.com/happy-dwarf-team/anvil/issues/36),
input to build identity in [`docs/design.md`](../design.md) section 5 and to the
`symbols.toml` format ([#18](https://github.com/happy-dwarf-team/anvil/issues/18),
[#21](https://github.com/happy-dwarf-team/anvil/issues/21)). Researched 2026-10-19.
Only the Steam install exists on the research machine; it was read, never modified,
and the game was not run. No GOG, Microsoft Store or Epic binary was available, so
every statement about those executables comes from public store metadata, not from
the files themselves.

Sections: **verified facts** (each with its source), **inferences** (reasoning from
the facts, not observed), **open questions** (things that need a binary or a test),
and **suggestions for `symbols.toml`**.

## Short answer

- EU4 is sold today on Steam, GOG, the Microsoft Store (Xbox app) and the Epic Games
  Store. Paradox's own store sells Steam keys.
- **GOG ships a different `eu4.exe`**, and two of them: 1.37.5.0 (2024-10-03) and a
  later 1.37.5.2 (2025-11-13) that Steam never received. GOG's public depot manifests
  give their size and SHA-256; both differ from Steam's. GOG also ships `eu4.pdb`
  and `Galaxy64.dll` instead of `steam_api64.dll`.
- **The Microsoft Store edition is an MSIXVC (GDK) package.** Microsoft documents that
  such games install as flat, user-writable files under `XboxGames`, *except the
  executables, which stay encrypted*. Its version and its `eu4.exe` are unknown.
- A proxy `version.dll` is plausible on GOG (plain Win32 install) and possibly on the
  Microsoft Store (the executable's folder is in the packaged-app DLL search order and
  `version.dll` is not a Known DLL), but neither is tested.
- "EU4 1.37.5" is therefore not one build. Anvil already fails safely on these builds
  (unknown build, plugins skipped); supporting them means new build ids, new header
  identities, and per-build RVAs and bytes for every symbol.

## Verified facts

### Storefronts and versions

1. Paradox's game page lists four storefronts under "Available at": Steam, Epic,
   Microsoft and GOG
   ([paradoxinteractive.com](https://www.paradoxinteractive.com/games/europa-universalis-iv/about)).
2. PCGamingWiki's availability table lists: Steam (DRM-free), GOG.com (DRM-free,
   Windows/OS X/Linux), Microsoft Store (Microsoft Store DRM, Windows only), Epic Games
   Store (Epic DRM, Windows/OS X, "Running through Epic Games Launcher will
   automatically install Paradox Launcher"); the publisher store and resellers sell
   Steam keys. It lists Epic Online Services as multiplayer middleware
   ([PCGamingWiki, wikitext via API](https://www.pcgamingwiki.com/w/api.php?action=parse&page=Europa_Universalis_IV&prop=wikitext&format=json)).
3. **Steam:** the public branch is build 15918133, last updated 2024-10-17, and a
   branch named `1.37.5` ("Old Version 1.37.5") has the same build id; there is no
   newer public build
   ([SteamCMD API, app 236850](https://api.steamcmd.net/v1/info/236850)). The local
   install matches: `appmanifest_236850.acf` has `buildid` 15918133,
   `launcher-settings.json` says `"rawVersion": "v1.37.5.0"`, `"distPlatform": "steam"`,
   and `eu4_branch.txt` is `release_1.37.5`.
4. **GOG:** the content-system API lists Windows builds 1.37.5.0 (build
   57980449153615183, published 2024-10-03) and 1.37.5.2 (build 59158195034859227,
   published 2025-11-13), plus a 1.37.5.0 to 1.37.5.2 Windows patch
   ([GOG content-system](https://content-system.gog.com/products/2057001589/os/windows/builds?generation=2),
   [GOG Database](https://www.gogdb.org/product/2057001589)). I found no patch notes
   for 1.37.5.2; Steam news since 2024-09 has none either
   ([Steam news API](https://api.steampowered.com/ISteamNews/GetNewsForApp/v2/?appid=236850&count=60)).
5. **Microsoft Store:** product 9PHQJWGLTZM8, "Europa Universalis IV - Microsoft Store
   Edition", released 2019-12-04, package family
   `ParadoxInteractive.EuropaUniversalisIV-MicrosoftSt_zfnrdv2de78ny`, package format
   `MSIXVC`, x64, capabilities `runFullTrust` and `internetClient`, Xbox Live
   multiplayer and achievements. The catalog gives no version (`"Version": "0"`)
   ([display catalog](https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=9PHQJWGLTZM8&market=US&languages=en-US),
   [Xbox store page](https://www.xbox.com/en-US/games/store/europa-universalis-iv-microsoft-store-edition/9PHQJWGLTZM8)).
   EU4 left PC Game Pass on 2023-05-31
   ([Pure Xbox](https://purexbox.com/news/2023/05/reminder-five-games-are-leaving-xbox-game-pass-today-may-31)),
   so today it is a purchase only.
6. **Epic:** listed by Paradox and PCGamingWiki (facts 1 and 2). Epic's store page
   refused automated access, and I found no public file manifest for it.

### Executables

7. **Steam `eu4.exe`** (local, read-only): 38,462,504 bytes, SHA-256
   `9AD3EFE1…771A`, PE32+, TimeDateStamp `0x66FE6AB9` (2024-10-03 09:58:17 UTC),
   SizeOfImage `0x25E0000`, CheckSum `0x024BAF19`, entry point `0x1A337C8`, linker 14.29,
   sections `.text .rdata .data .pdata _RDATA .rsrc .reloc` (no SteamStub `.bind`
   section, so no Steam DRM wrapper), Authenticode-signed by Paradox Interactive AB
   (valid), PDB path `C:\mnt\gsg\eu4\eu4\outputdir_windows\eu4.pdb`, PDB GUID
   `BF218AD8-BA8A-4F1E-938B-5143D11E0CB0` age 1. Static imports include
   `steam_api64.dll`, `EOSSDK-Win64-Shipping.dll`, `pops_api.dll`, `nakama-cpp.dll` and
   `VERSION.dll`. No `eu4.pdb` ships with it.
8. **GOG `eu4.exe`**, from GOG's public depot manifests (the manifests carry size and
   hashes per file; the binaries themselves were not downloaded):

   | GOG version | Size (bytes) | SHA-256 |
   |---|---|---|
   | 1.37.5.0 | 38,345,256 | `5f4145ce5ff9a4140cc4d14440920382b698dd91e1cdae1d1dc053357d9388ee` |
   | 1.37.5.2 | 38,345,336 | `d29cf745f245a5cdd74fe7b500599dddd4a61089affb04b02044056f23155319` |

   Manifests:
   [1.37.5.0 build](https://gog-cdn-fastly.gog.com/content-system/v2/meta/2f/40/2f40747f31c131cab0118fb104b8c25d)
   → depot `5dcecf1a7b952de0b43613159505bff9`;
   [1.37.5.2 build](https://gog-cdn-fastly.gog.com/content-system/v2/meta/b5/cc/b5cca0c0821db6c9329b38b3344978f8)
   → depot `b0a0a8415639b52fc276d1b40bbf24af` (zlib-compressed JSON, path
   `content-system/v2/meta/<2>/<2>/<id>`).
9. The GOG executable depots contain `Galaxy64.dll`, `eu4.pdb` (265,801,728 bytes in
   1.37.5.0; 265,777,152 in 1.37.5.2), `pdx_core_test.pdb`, `pops_api.dll`,
   `nakama-cpp.dll`, `tbb.dll`, and no `steam_api64.dll` and no
   `EOSSDK-Win64-Shipping.dll`. `eu4_branch.txt` is 14 bytes in 1.37.5.0 (the length of
   `release_1.37.5`) and 22 bytes in 1.37.5.2. The 1.37.5.2 build replaces the Paradox
   launcher installer (`launcher-installer-windows_2024.14.msi` →
   `launcher-installer-windows_2025.12.exe`). GOG's data depots also carry
   `steam_appid.txt` and `dbghelp.dll`.
10. GOG describes its games as having no DRM; GOG Galaxy is optional; a Paradox account
    is required for multiplayer ([GOG store page](https://www.gog.com/en/game/europa_universalis_iv)).

### Microsoft Store packaging and DLL loading

11. Since the March 2022 GDK, MSIXVC games use Flat File Install: packages install under
    `C:\XboxGames` by default, the folder "does not require elevated permissions", and
    "All files, outside of the executable, are modifiable by default". Gamers get read
    and write access to all game files "except executables designated in the
    MicrosoftGame.config, which will remain encrypted". MSIXVC2 installs flat by
    default. UWP-model Store games do not get this
    ([Flat File Install overview](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/common/packaging/packaging-flatfileinstall),
    [Mods support for PC GDK titles](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/common/packaging/packaging-mods)).
12. For packaged apps, the loader searches: DLL redirection, API sets, SxS manifests
    (desktop apps), the loaded-module list, Known DLLs, the package dependency graph,
    then "The folder the calling process was loaded from (the executable's folder)",
    then `System32`
    ([DLL search order](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order)).
13. `version.dll` is not in `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\KnownDLLs`
    on the research machine (Windows 11 Pro 10.0.26200). That is why a proxy next to
    the Steam `eu4.exe` loads at all.

### Other EU4 tools

14. matanki-saito/EU4dll: README says "Windows is supported and dll only works on
    Steam" without giving a reason. Its loaders are `version.dll` and `d3d9.dll`
    proxies (the `version/` and `d3d9/` projects), and it finds the game version by
    scanning memory for the string `EU4 v1.` and finds code by byte patterns
    (`Plugin64/version.cpp`, `byte_pattern.cpp`), not by fixed RVAs
    ([repository](https://github.com/matanki-saito/EU4dll)).
15. PoXiao-zero/eu4dll_mac: "developed based on the GOG version of EU4 1.37.5", macOS
    only (a dylib injected with `insert_dylib`), also byte-pattern based
    (`src/memoryHelper.cpp`). It says nothing about Windows GOG
    ([repository](https://github.com/PoXiao-zero/eu4dll_mac)).

## Inferences

- **GOG code layout differs from Steam's.** The sizes differ by 117,248 bytes and the
  GOG build has no `steam_api64.dll` to import, so at least the platform layer was
  linked differently; RVAs of shared functions very likely moved. The Steam PE header
  timestamp and GOG's 1.37.5.0 publish time fall on the same day (2024-10-03), which
  suggests one source revision built per store, not one binary.
- **GOG 1.37.5.2 is a GOG-only rebuild** as far as public data shows (Steam's public
  branch never moved past build 15918133). A GOG Galaxy user with auto-update has
  1.37.5.2; an offline-installer user may still have 1.37.5.0. So Anvil would need two
  GOG build ids, not one.
- **Header identity can be filled only from a binary.** GOG's manifests give SHA-256
  and size, which is enough for Anvil's background check, but not TimeDateStamp,
  SizeOfImage, CheckSum or entry point, which Anvil's fast path needs.
- **Microsoft Store hashing may not work as on Steam.** Anvil reads the PE header of
  the running image in memory, which is decrypted, so the fast path should work. Its
  background SHA-256 of `eu4.exe` on disk, and `anvil check --exe` offline, read the
  file; if the file on disk is the encrypted form (fact 11), the hash will not match
  any plain binary and offline checks cannot read code bytes.
- **Microsoft Store proxy loading** is possible in principle: the executable's folder
  is searched (fact 12), `version.dll` is not a Known DLL (fact 13), and the folder is
  writable (fact 11). Whether the Store `eu4.exe` imports `VERSION.dll` at all, and
  whether something in the package (a `MicrosoftGame.config` setting, the Gaming
  Services file filter, code-integrity policy) blocks or repairs an added DLL, is not
  documented for this game.
- **EU4dll's "only Steam" is about its patterns and testing, not a hard limit**:
  pattern scanning would survive moved RVAs, so its restriction is probably untested
  builds or differing code, not loader failure. This is a guess; the README gives no
  reason.
- **`eu4.pdb` on GOG** is a debug-symbol file shipped to every GOG buyer. It matches the
  GOG binary, not Steam's (different link), but would let a contributor name functions
  in the GOG build and then find them in the Steam build by bytes. Using it in a public
  repository raises licensing questions this note does not answer.

## Open questions (need a binary or a test)

1. GOG `eu4.exe` 1.37.5.0 and 1.37.5.2: PE header identity (TimeDateStamp,
   SizeOfImage, CheckSum, entry point), import table (does it import `VERSION.dll`,
   `Galaxy64.dll`?), Authenticode signer, PDB GUID, and what 1.37.5.2 changed
   (`eu4_branch.txt`, `eu4_rev.txt`, a code diff against 1.37.5.0).
2. How many of the Steam build's declared symbols exist in the GOG builds with the same
   bytes, and at what RVAs. This decides whether per-build declarations are cheap or a
   full re-port.
3. Microsoft Store: current version, whether `eu4.exe` on disk is readable as a plain
   PE, its imports, and whether a `version.dll` dropped next to it is loaded (one test
   by someone who owns that edition, with the loader log).
4. Epic: version, whether its `eu4.exe` differs from Steam's (it likely needs no
   `steam_api64.dll`), and where it installs.
5. Whether GOG/Epic/Store saves and checksums match Steam's for the same version
   (relevant to multiplayer and ironman, not to loading).

## Suggestions for `symbols.toml` (not decided)

These are suggestions only; the format is owned by #18 and #21.

- **Build ids should carry the store and the full four-part version**, because one
  three-part version maps to several binaries: e.g. `eu4-1.37.5-steam-win64` (keep, it
  is already published), `eu4-1.37.5.0-gog-win64`, `eu4-1.37.5.2-gog-win64`,
  `eu4-1.37.5-msstore-win64`. The id is a name; the table in `game_build.cpp` (header
  identity plus SHA-256) stays the source of truth.
- **Option 1: one `symbols.toml` per build.** Today's shape (`build = "…"` at the
  top), one file per supported build, e.g. `symbols/eu4-1.37.5-steam-win64.toml`.
  Simple to verify and to generate; `game_builds` is the list of files. Drawback: names,
  `signature`, `memory`, `points_to` and notes are copied per build and can drift, and
  the generated header must stay identical across files (the generator would have to
  check that every file declares the same names and signatures).
- **Option 2: shared declarations with per-build placement.** Declare each symbol once
  with its build-independent fields, and give `rva` and `bytes` per build:

  ```toml
  [symbol.speed_object]
  memory = "code"
  signature = "void* (void* session)"
  note = "session vtable slot 0x100: returns the game-speed object"

  [symbol.speed_object.at."eu4-1.37.5-steam-win64"]
  rva = 0x8024D0
  bytes = "48 8B 81 18 04 00 00 C3"

  [symbol.speed_object.at."eu4-1.37.5.2-gog-win64"]
  rva = 0x…        # found with anvil check / a pattern tool
  bytes = "48 8B 81 18 04 00 00 C3"
  ```

  One generated header, one place for names and signatures; the plugin's supported
  builds are the builds for which every required symbol has a placement. `values`
  (#32) would also be per build, because offsets inside code can change with codegen.
  `anvil check --exe` checks one build at a time.
- **Leaning:** option 2 fits "same code, different build id" better, since the source
  revision (and so names, signatures and struct layout) is the same and only placement
  differs. Option 1 is enough while only Steam is supported. Either way, a plugin with
  no placement for the running build is skipped, which is today's behaviour.
- **Do not add GOG or Store builds before open question 2 is answered**: if the GOG
  code is a re-link with the same functions, option 2 plus an offline pattern tool
  (the "pattern search as an offline tool" from #18) can port a plugin cheaply; if it
  differs more, per-build files are no worse.
