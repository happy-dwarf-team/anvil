# Model reload

A window of Anvil's overlay for model modders: it lists the files of your enabled mods that you saved
since the last reload (meshes, model textures, animations, entities and the `pdxmesh` entries of
`interface/**/*.gfx`) and reloads them in the running game with one click. It is the standard plugin
`anvil-model-reload` (design §4), off by default. Its shape was decided in
[#470](https://github.com/happy-dwarf-team/anvil/issues/470).

It changes only what the game draws: mesh types, textures, animations and entity definitions. No
country, province, unit or other game state changes, and a reload takes no random draw, so
`affects_gameplay = false`. It still refuses ironman and multiplayer games (below).

## Turning it on

- The setting **Model reload window** (`window`, default off, applies at once) of anvil-model-reload, in
  the overlay's Plugins tab or the Manager's.
- On: the window opens, at every start too. Once closed, its switch is above the list of the **Plugins**
  tab of the Anvil panel, like every plugin's window.
- Off: the window hides and the plugin looks at no file.
- It needs `anvil-game` (required) and draws with `anvil-overlay` (optional: without it the plugin loads
  and does nothing). On OpenGL nothing is drawn.

## What it watches

The mods enabled in the launcher (`dlc_load.json` in the game's user folder, the folder `-userdir`
names), each read from its descriptor's `path`. In each mod's folder it looks at:

| Files | Reloaded with |
| --- | --- |
| `.dds` and `.tga` under `gfx/models` | the console's `reloadtexture <file>`, one per file |
| `.mesh` under `gfx` | the game's own `CPdxMeshType::ReloadType` for every mesh type whose `file` is that file |
| `.anim` under `gfx` | `reload anim` (every animation) |
| `.dds` and `.tga` under `gfx/particles` | `reload particle` |
| `.asset` under `gfx` (entities and particles) | `reload assets` |
| `.gfx` under `interface` | the plugin's own reload of its `pdxmesh` entries (below) |

The window shows the mods watched, the files changed since the last reload (by size and write time, at
most 40 listed and then **N more**) and the **Reload the changed files** button. It looks at the files
when it opens, every 2 seconds while it is shown and right before a reload. A mod packed as a zip, a
missing folder or an unreadable descriptor is named in `anvil.log` and counted in the window. The game's
own folder is not watched, and neither are the other mods' unchanged files.

## What a reload does

Only in a loaded single-player game that is not ironman: in the main menu the window says to load a
game, and in ironman or multiplayer it says it does not reload there. The steps run on the game's main
thread after a frame, in this order:

1. `reloadtexture` for each changed model texture, with the game's own spelling of its folder.
2. Each changed `.mesh`: every mesh type whose `file` names it is reloaded (`ReloadType`: the mesh,
   skeleton, locators and bounds from the file, then the animations looked up again by name).
3. `reload anim` once, when an `.anim` changed.
4. `reload particle` once, when a particle texture changed.
5. `reload assets` once, when an `.asset` changed.
6. Each changed `.gfx`: its `pdxmesh` entries (below).

The window then shows each step's result and the time; `anvil.log` has the same lines. A step that
faults in the game's code stops the rest, which the window marks **Not run**, and asks to save and
restart.

### The `pdxmesh` entries of a `.gfx`

For each `pdxmesh = { ... }` entry of the file, by `name`, the plugin finds the game's mesh type of that
name and writes the fields that changed: `file`, `scale`, `cull_distance` and the `type` of each
`animation`. Then it reloads that mesh type, which applies the new file and scale at once, also to the
units already on the map. When the same name is in the file twice, the later block counts, as when the
game reads it.

It leaves an entry as it is and says why:

- **A new mesh type: it needs a restart.** No mesh type of the game has that name.
- **Its animation ids changed: it needs a restart.** The entry's animation `id`s are not the game's, in
  the same order. The game keys an animation by a hash of its id, and units look it up by that hash.
- **Its file is in no mod and not in the game: left as it was.** The new `file` exists in no enabled mod
  and not in the game's folder. Files inside the game's DLC zips are not seen.
- **A value cannot be read: left as it was.** A field such as `scale = big`.

Keys other than these (`meshsettings`, for one) are not reloaded; the log names them. A later mod that
has the same `.gfx` file overrides the earlier one, so an unchanged later copy is not overwritten with
the earlier mod's entries.

### Entities

`reload assets` reads every `.asset` again, but the units already on the map keep the entity they were
created with: the window says to load a save to see the change. The plugin does not load one by itself.

### What it never runs

- `reload guianim`: it crashes the game (eu4-engine-lab, 3D asset reload).
- `reload texture` without a file: it reloads every texture of the game, flags included, which takes
  minutes on a large mod.
- `reload mesh <text>`: it matches the text against the mesh types' names, not their files, so the
  plugin reloads the mesh types by their `file` itself.

## For the lab

The environment variable `ANVIL_MODEL_RELOAD_CONTROL` names a file of one line, `<n> look` or
`<n> reload`, that the plugin reads every 200 ms; it answers in `<file>.state` with `<n> changed <count>`
and the files, `<n> done` and the log lines, `<n> refused <why>` or `<n> failed`. Test runs use it in
place of a click, which the overlay's input does not take from Tongs.

## How it is built

- **Symbols** (`symbols.toml`, all three 1.37.5 builds): the graphics object and its hash table of 3D
  types (bucket count `+0x554`, buckets `+0x558`, entries `{type, next}`), read from a site in
  `CGraphics::GetPdx3DType`; the `CPdxMeshType` type id `0x180` and vtable; `CPdxMeshType::ReloadType`;
  the game's `std::string::assign`; and the console's dispatcher, the value manager and the string free
  that Tongs also uses.
- **Mesh type fields** (from `CPdx3DType::ReadMember`, `CPdxMeshType::ReadMember` and
  `SAnimationLookup::ReadMember`): name `+0x10`, scale `+0x30`, the square of `cull_distance` `+0x34`,
  `file` `+0xA0`, the animation lookups `+0xC0` (entries of `0x40`: id hash `+0x10`, animation name
  `+0x20`; the hash is 32-bit FNV-1 of the id's bytes). `ReloadType` frees the mesh, loads it again from
  `file` through `InitForDevice`, which bakes `scale` into it, and looks the animations up again by name.
- **Safety:** every read and write of game memory is guarded and checks the type's vtable and type id
  first; strings are assigned with the game's own `assign`, so the game frees them as its own. A fault
  ends the reload. The parser of `.gfx` files and the plan of steps are plain C++ with unit tests
  (`tests/model_reload_tests.cpp`).
