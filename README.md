# Anvil

Anvil is a plugin loader for **Europa Universalis IV** (Windows, Steam). It lets
native plugins improve or extend the game **without modifying `eu4.exe`**.

> Status: design phase. Nothing here is ready for players yet — see
> [docs/design.md](docs/design.md).

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

## License

MIT — see [LICENSE](LICENSE). Unofficial community project, not affiliated with
Paradox Interactive.
