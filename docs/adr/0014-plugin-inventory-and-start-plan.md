# Plugin inventory and start plan

Status: **accepted** (2026-11-15). The maintainer decided every point before the change was made. Follows [ADR 0013](0013-module-settings-and-overlay-writes.md), whose point 9 put the rules that are not drawing into one view model.

Before this change, four places answered "what loads at a start and why not", each with its own copy of the rules:

- the loader (`src/core/runtime.cpp`) discovered the plugins of `anvil/plugins` and the playset, decided the mods' plugins, made the approved copies, moved retired settings, verified each plugin's symbols through a callback of the resolver (`ResolveInput::verify`), and resolved;
- `anvil check --game` (`src/cli/game.cpp`) did the same reads with its own per-plugin check (`check_plugin`), its own `game_build` for the resolver and its own string states (`status`, `state`, `load_order`, `skipped`), and wrote nothing;
- the Manager took that report and recomputed parts of it: the view model walked the dependencies again (`load_state`), `model_reason` sorted the resolver's reasons into the ones it works out itself, and `required()` repeated the rule that `affects_gameplay = true` needs `anvil-game`;
- the overlay did the same from `anvil.control`, with a second `model_reason` and `required()`.

The copies drifted. One case was visible to players: after an update that retires a setting into a module switch (ADR 0013, point 4), the Manager showed the module on before the first start, because it read `anvil.toml` as saved; the start then moved the retired value and turned the module off.

## Decision

1. **Two steps in the core** (`anvil_core`, so the loader, the CLI and the Manager share them).
   - **Plugin inventory** (`take_inventory`, `src/core/plugin_inventory.hpp`) reads the disk and the game image and writes nothing. It holds every plugin of `anvil/plugins` and of the playset's mods with its manifest, the result of its check against the game (symbols, `settings.toml`, module names) and its settings; for a mod's plugin also its hash and the approved copies on disk that decide its approval.
   - **Start plan** (`plan_start`, `src/core/start_plan.hpp`) is a pure function of the inventory, the player's choices (`PlayerChoices`: `anvil.toml`, the approvals in `trust.toml`, the players' settings files of plugins that retire settings) and the start conditions (`StartConditions`: `ANVIL_DISABLE`, the safe mode record and its crashes, the benchmark arm, the host's API and version). It gives the load order with each plugin's manifest, symbols and modules on; the skipped plugins with their reason and why they do not load as the player reads it (`NotLoading`: off, needs a plugin, not installed, waits for approval, cannot load); and the writes the start makes first (`StartWrites`: approved copies to remove and to make, retired settings to move).
2. **One check per plugin** (`check_plugin_folder`, `src/core/plugin_check.hpp`), in the inventory step. `ResolveInput::verify` and `game_build` go; the resolver takes the check's result as `check_failures`. `anvil check` adds only the checks for authors (code ranges, absent symbols, instruction boundaries, `symbols.lock`) on top. The difference between the running game and a file stays in the image reader: the loader reads through `OriginalImageReader`, so bytes another plugin already changed read in their original form; the CLI and the Manager read `eu4.exe` through `FileImageReader`.
3. **The loader carries the plan out.** It takes the inventory, reads the choices, computes the plan, carries out its writes in order (remove, copy, move), gives the plan to the plugin set and writes `last-start.toml` from it. An approved copy that cannot be made is no longer a skip: the plugin gets the outcome failed at load time ("cannot make its approved copy: …"), and the plugins that require it fail with it. With `enabled = false` or `ANVIL_DISABLE=1` the loader still returns before it takes the inventory; the plan of such a start, for the other readers, marks every plugin off and writes nothing.
4. **`anvil check --game` and the Manager show the plan of a normal start.** The next start's safe mode is one line (`next_start` in JSON, read through `read_next_start`, the function the loader's start marker uses), and so are the writes of that start. The Manager asks for a benchmark arm's plan through the start conditions (`StartConditions::benchmark`), for both arms of a series.
5. **`GameReport` stays**, built on the inventory, the player's choices, the plan, each plugin's `CheckReport` and the facts of `eu4.exe`. Its own `load_order`, `skipped`, `status` and string states go; states are enums (`ModPluginState`, `NotLoading`) and become strings only when the report is formatted. Text and JSON keep every field they had and gain the next start's section.
6. **The view model stops deciding.** `PluginSnap::next` is the plan's verdict on a plugin, which the apps copy in; the `load_state` walk, both `model_reason` copies and the Manager's `required()` rule go. The required list stays only for `turn_on_chain` and `needed_by`, which ask what to turn on, not what loads.
7. **`anvil.control` gives the next start's plan** (private, appended in 1.24: `next_start`, `mod_plugin_next_start`, `AnvilControlNextStart`). The core computes it from this start's inventory and the choices saved now, without reading the plugin folders again, and again after each switch, approval or revocation. The overlay shows this start's outcome (`runs`, `failed`) and the next start's plan apart. A plugin installed while the game runs appears after a restart.
8. **Tests.** The plan is tested in memory, on inventories built in the test (`tests/start_plan_tests.cpp`); the inventory on folder fixtures (`tests/game_tests.cpp`); a child-process or DLL test only where it checks execution (a DLL loaded, the writes made, the safe mode counter, `last-start.toml`). The "skipped with reason X" tests of the view model moved to the plan's tests.
9. **Glossary.** *Plugin inventory* and *Start plan* join it; *Outcome* stays what actually happened at a start.

## Consequences

- One answer for every reader: the Manager, `anvil check --game`, the overlay's "next start" and the loader agree on what loads, also before the first start after an update that retires a setting. The Manager shows the module off there, as the start will have it, and a switch the player makes first carries the move out as that start would, so the start has nothing left to undo it with.
- The loader checks the symbols of every plugin it finds, also of one the player turned off, so the check's result is in the inventory for the other readers. This costs start-up time per disabled plugin with symbols; the log's "N symbol(s) verified" counts only the plugins that load.
- A failed approved copy shows as failed in the overlay and the log, with the reason, rather than as skipped.
- Reasons of plugins from mods in `anvil check --game` use the loader's words (`from mod '<name>' (…): …`).

## Compatibility

- **API 2.** No host function changes; nothing in the SDK changes.
- **`anvil.control`.** Two calls appended after the existing ones; the overlay checks the table's size and, with an older core, falls back to this start's outcome.
- **Files.** No new keys in `anvil.toml`, `trust.toml` or the players' files.
- **`anvil check --game` JSON.** A `next_start` object (`safe_mode`, `unfinished_starts`, `writes` with `remove`, `copies` and `retired`) between `skipped` and `warnings`; consumers ignore fields they do not know (design §16).

## Considered options

- *Keep the loader as it was and make the CLI and the Manager call it.* The loader's discovery writes (copies, moves) and runs only in the game; a dry-run flag through it would keep the decisions and the writes mixed.
- *Let the Manager read `last-start.toml` for what loads.* It says what the last start did, not what the next will do with the files saved now; it cannot show a plugin installed since, an approval made since, or the retired-setting case.
- *Split the change into a core PR and one per reader.* Every reader would carry a second copy of the rules until the last PR; the maintainer chose one PR.
