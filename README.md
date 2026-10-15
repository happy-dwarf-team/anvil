# Anvil

Anvil is a plugin loader for **Europa Universalis IV** (Windows, Steam). It lets
native plugins improve or extend the game **without modifying `eu4.exe`**.

> Status: 0.1.0, stage 1 (core loader) — not released to players yet. Design:
> [docs/design.md](docs/design.md). Plugin SDK:
> [include/anvil/anvil_plugin.h](include/anvil/anvil_plugin.h); example:
> [examples/hello](examples/hello).

## How it works (in short)

- Anvil ships as a `version.dll` placed next to `eu4.exe`. Windows loads it
  together with the game; it forwards every `version.dll` call to the system
  copy and, in addition, loads plugins.
- Plugins live in `anvil/plugins/<plugin-id>/` (manifest + DLL). Later, plugins
  can also come from EU4 mods enabled in the Paradox launcher (Steam Workshop),
  each version only after the player approves it.
- Every plugin declares which EU4 builds it supports. Unsupported builds,
  broken plugins or an empty `plugins` folder mean the game simply starts as
  usual.
- Each start writes `anvil/anvil.log`: what was loaded, what was skipped and why.

## Install / uninstall (planned)

Install: extract the release archive into the game folder (the one that
contains `eu4.exe`). Uninstall: delete `version.dll` and the `anvil` folder.
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

## License

MIT — see [LICENSE](LICENSE). Unofficial community project, not affiliated with
Paradox Interactive.
