# Map modes from files v1 specification

Status: anvil-map 1.27.0 ([#577](https://github.com/happy-dwarf-team/anvil/issues/577)). The C service behind it is [map-modes-v1.md](map-modes-v1.md).

## 1. Scope

A mod defines map modes in text files, without a plugin of its own and without C++. Each mode is a button on the minimap's list, a colour for each province from a list of rules, a line in the province's tooltip and a legend in Anvil's overlay. The rules read what the game keeps on a province: a flag, a variable, a modifier.

What it does not do in v1: no country scope (the owner's flags or variables), no scripted triggers, no localisation keys for texts, no colour scales between two colours. A mode that needs those is a plugin on `anvil-map.modes`.

A mode from a file changes nothing in the game's state, like any plugin mode: it reads `anvil-game.world` and paints. It works in ironman and multiplayer. It does not filter what the player may know: a rule on a flag of a province under terra incognita colours it too.

## 2. Where the files are

`<mod>/anvil/map_modes/*.txt` of every mod enabled in the playset (the launcher's `dlc_load.json` in the game's user folder, `enabled_mods`, each `.mod` file's `path`).

- The folder is under `anvil/`, which the game does not read, so the files do not change the game's checksum.
- Mods are read in the playset's load order and each mod's files by name. A file of the same name (case ignored) in a later mod replaces an earlier one, as the game's own files do.
- A mode's name is unique in the playset: when two files define the same name, the first one read keeps it and the other is left out with a line in `anvil.log`.
- A zipped mod (a `.mod` file with `archive` and no `path`) is skipped with a line in the log.
- A file larger than 1 MiB is not read.
- The files are read once, at `anvil-game.ready`. A change needs a restart of the game.

## 3. Syntax

The game's script syntax: `key = value`, blocks in `{ }`, comments from `#` to the end of the line. Texts are quoted, or a single word. A file that is not valid UTF-8 is read as Windows-1252, the encoding of the game's own script files; a UTF-8 BOM is allowed.

```
holy_sites = {
    label = "Holy sites"
    description = "Where the faith is strong"
    tab = 4
    sprite = GFX_mapmode_religion
    rule = { flag = my_holy_site color = { 224 176 0 } label = "Holy site" }
    rule = { variable = my_faith at_least = 5 color = { 128 64 192 } label = "Faith 5 or more" }
    rule = { modifier = local_missionary_strength above = 0.01 color = { 64 128 255 } label = "Missionaries" }
}
```

### A mode

| Key | Value | |
| --- | --- | --- |
| (the block's name) | `[a-z0-9_-]`, 1 to 32 characters | the mode's id is `anvil-map.<name>` |
| `label` | text, 1 to 64 bytes | required; the button's name and the legend's title |
| `description` | text, at most 512 bytes | the button's tooltip |
| `tab` | 1, 2, 3 or 4 | the minimap's tab (map-modes-v1 §5); default 4 |
| `sprite` | a sprite name, `[A-Za-z0-9_]` | default: the game's empty button frame |
| `legend` | `yes` or `no` | default `yes` |
| `rule` | a block, see below | 1 to 64, in order |

### A rule

| Key | Value |
| --- | --- |
| `always` | `yes`: every province passes; a last rule for "everything else" |
| `flag` | a province flag's name: the province passes while it has the flag (what `has_province_flag` sees) |
| `variable` | a province variable's name: the province passes while its value is within the bounds |
| `modifier` | a modifier's key as the game's files write it (`local_missionary_strength`): the province's own value of it, not its owner's |
| `at_least`, `at_most`, `above`, `below` | a number with at most three decimals; for `variable` and `modifier` only |
| `color` | `{ r g b }`, each 0 to 255; required |
| `label` | text, 1 to 64 bytes; required: the tooltip's line and the legend's entry |

A rule has exactly one of `always`, `flag`, `variable` and `modifier`. With no bound, a `variable` or `modifier` rule passes when the value is not 0. With bounds, all of them hold together (`at_least = 1 below = 5` is 1 ≤ value < 5). A variable the province does not have is 0, as for the game's triggers. Numbers are compared as the game keeps them, in thousandths.

## 4. Colours and the tooltip

- A province takes the colour of the **first** rule it passes. A province that passes none keeps the political map mode's colour, so sea, wasteland and empty land look as players know them.
- The colours are computed only while the mode is shown: when the player picks it (in the same step that publishes `anvil-map.shown`) and at each `anvil-game.month` while it stays shown. A mode nobody looks at costs nothing.
- A pass over the map reads each province's flags, variable or modifier through `anvil-game.world`: about as much as one read per rule a province reaches.
- A flag's name is looked up at each computation, so a flag that no effect had set when the mode was shown first counts from the next month on.
- A `modifier` key the game does not know is logged once (`the game has no modifier …; its rule never passes`), and its rule never passes.
- The tooltip of a province gives the label of the rule it passes; for a `variable` rule also the value, as `Faith 5 or more (my_faith 7.5)`. A province that passes no rule has no line.

## 5. The legend

Unless `legend = no`, the mode's legend lists each rule's colour and label once, in rule order (two rules with the same colour and label are one entry), at most 32 (map-modes-v1 §6a).

## 6. Faults

A fault in a mode leaves out that mode whole and writes a line to `anvil.log` with the file and the line: `map mode file My mod/anvil/map_modes/holy.txt:7: a rule needs color = { r g b }`. The other modes of the file are read. Comparison operators (`>=`, `<`) are refused with a hint to write `at_least` and the others. A mode that finds the pool of 13 numbers lent is not registered and logged.

## 7. Test in the game before the release

Under the lab's game lock, in a test profile with a test mod that has `anvil/map_modes/test.txt`, on Steam and GOG:

1. The button is on its tab; picking it paints the provinces that pass the rules and leaves the others political.
2. The legend window shows with the mode and goes with it.
3. A month later, after a console effect that sets a flag on a province (`set_province_flag` on a province in the console's scope), the province changes colour.
4. The tooltip's line for a province that passes a rule, and none for one that does not.
5. A faulty file: its line in `anvil.log`, the other modes still there.
