# Anvil

Anvil is a plugin loader for **Europa Universalis IV** (Windows, Steam). It lets
native plugins improve or extend the game **without modifying `eu4.exe`**.

> Status: stage 1 (core loader), early public releases — see
> [Releases](https://github.com/happy-dwarf-team/anvil/releases) and
> [CHANGELOG.md](CHANGELOG.md). Supported game build: EU4 1.37.5 (Steam,
> Windows 64-bit); on other builds only plugins that use no game addresses load. Design:
> [docs/design.md](docs/design.md). Plugin API 2:
> [include/anvil/anvil_plugin.h](include/anvil/anvil_plugin.h) (C) and
> [include/anvil/anvil.hpp](include/anvil/anvil.hpp) (C++20 wrapper); examples:
> [examples/hello-c](examples/hello-c) and [examples/hello-cpp](examples/hello-cpp).

## How it works (in short)

- Anvil ships as a `version.dll` placed next to `eu4.exe`. Windows loads it
  together with the game; it forwards every `version.dll` call to the system
  copy and, in addition, loads plugins.
- Plugins live in `anvil/plugins/<plugin-id>/` (manifest + DLL). Later, plugins
  can also come from EU4 mods enabled in the Paradox launcher (Steam Workshop),
  each version only after the player approves it.
- A plugin that uses game addresses declares them, for one EU4 build, in its
  `symbols.toml`; on another build it is skipped. Broken plugins, plugins
  written for an older plugin API or an empty `plugins` folder mean the game
  simply starts as usual. The manifest format is described in
  [docs/design.md](docs/design.md#51-plugintoml-manifest-v2).
- Anvil ships the standard plugin `anvil-game` (`anvil/plugins/anvil-game/`):
  game readiness, a frame event, work on the main thread and game sessions
  ([include/anvil/anvil_game.h](include/anvil/anvil_game.h)). Plugins that
  change the simulation need it: in ironman and multiplayer games Anvil takes
  their changes out unless the player allows them in `anvil/anvil.toml`.
- Each start writes `anvil/anvil.log`: what was loaded, what was skipped and why.
- A crash writes a report to `anvil/crashes/` (the last 10 are kept): where it
  happened and which plugins were involved, with each plugin's `issues` link.
  After two starts in a row that ended before the game was ready, the next
  start loads no plugins (safe mode).

## Install / uninstall

Download `anvil-windows-x64.zip` from the
[Releases page](https://github.com/happy-dwarf-team/anvil/releases) (the newest
build is always at
[releases/latest](https://github.com/happy-dwarf-team/anvil/releases/latest)).
`SHA256SUMS.txt` next to it holds the archive's checksum, and the archive
contains its own `SHA256SUMS.txt` for the extracted files.

Install: extract the archive into the game folder (the one that contains
`eu4.exe`). Uninstall: delete `version.dll` and the `anvil` folder.
`eu4.exe` is never changed, so Steam updates and "Verify integrity of game
files" keep working.

## Building

Requirements: Visual Studio 2022 (or Build Tools) with the C++ workload, CMake
≥ 3.25, Git (dependencies are fetched at configure time: toml++ 3.4.0,
SafetyHook 0.7.0 with Zydis 4.1.0).

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --component anvil --prefix dist   # version.dll + anvil/ layout
```

The build also produces `anvil.exe`, the command-line tool for plugin authors.
It shares the `anvil_core` library (manifests, `symbols.toml` and its
verification, `settings.toml`, the game build identity) with `version.dll`:

```powershell
anvil gen symbols.toml build\generated            # symbols.gen.h and symbols.gen.hpp
anvil gen settings.toml build\generated           # settings.gen.h and settings.gen.hpp
anvil check my-plugin --exe "C:\...\eu4.exe"     # verify against the game, write symbols.lock
anvil check my-plugin --format github             # in CI, without the game: schemas and symbols.lock
```

See [docs/design.md §16](docs/design.md#16-tooling-for-authors) for the
checks, the instruction-boundary rule and the `symbols.lock` format.

Contributions, the commit convention and the release process: see
[CONTRIBUTING.md](CONTRIBUTING.md).

## License

MIT — see [LICENSE](LICENSE). Unofficial community project, not affiliated with
Paradox Interactive.
