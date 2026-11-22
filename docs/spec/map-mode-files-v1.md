# Map modes from files v1 specification

Status: anvil-map 1.26.0 ([#577](https://github.com/happy-dwarf-team/anvil/issues/577); the owner and colour scales [#580](https://github.com/happy-dwarf-team/anvil/issues/580)). The C service behind it is [map-modes-v1.md](map-modes-v1.md).

## 1. Scope

A mod defines map modes in text files, without a plugin of its own and without C++. Each mode is a button on the minimap's list, a colour for each province from a list of rules, a line in the province's tooltip and a legend in Anvil's overlay. The rules read what the game keeps on a province or on its owner: a flag, a variable, a modifier, one of the game's own values such as development or the treasury, who owns and controls the province, whose cores it has and whether the owner is a subject, and whose. A rule gives one colour, or a scale between two colours.

What it does not do in v1: no other scope than the province and its owner (no reads of the overlord's own values, no area), no scripted triggers, no localisation keys for texts, no scale with more than two colours. A mode that needs those is a plugin on `anvil-map.modes`.

A mode from a file changes nothing in the game's state, like any plugin mode: it reads `anvil-game.world` and paints. It works in ironman and multiplayer. It does not filter what the player may know: a rule on a flag of a province under terra incognita colours it too.

## 2. Where the files are

`<mod>/anvil/map_modes/*.txt` of every mod enabled in the playset (the launcher's `dlc_load.json` in the game's user folder, `enabled_mods`, each `.mod` file's `path`).

- The folder is under `anvil/`, which the game does not read, so the files do not change the game's checksum. Anvil's discovery of plugins from mods (design §14) passes `anvil/map_modes/` by: it is not a plugin folder, and before 1.26.0 it showed as a plugin that cannot load (its name is not a plugin id).
- Mods are read in the playset's load order and each mod's files by name. A file of the same name (case ignored) in a later mod replaces an earlier one, as the game's own files do.
- A mode's name is unique in the playset: when two files define the same name, the first one read keeps it and the other is left out with a line in `anvil.log`.
- A zipped mod (a `.mod` file with `archive` and no `path`) is skipped with a line in the log.
- A file larger than 1 MiB is not read.
- The files are read at `anvil-game.ready`, and the modes they define are registered then.
- **Editing a file while the game runs.** Each time a mode from the files becomes shown and at each `anvil-game.month` while one is shown, anvil-map looks at the playset's files again (their paths, sizes and write times; no file is read when none changed). After a change it reads them all again and gives each registered mode the rules and legend of its new definition, so a modder sees an edit by picking the mode again, from another mode, without a restart. `anvil.log` gets `map mode files changed: the rules of N mode(s) read again`. What the game registered at the start waits for a restart, with a line in the log: a mode's `label`, `description`, `tab` and `sprite` (its button), a new mode, and a mode no longer read (its file gone, or a fault in it now; it keeps the rules it had). A fault is logged as at the start.

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
| `owned_by` | a country tag (`RUS`, case ignored): the province passes while that country owns it |
| `controlled_by` | a country tag: the province passes while that country controls it (its owner, or an occupier) |
| `core` | `yes`: the owner has a core on the province; `no`: it has none. An unowned province passes neither. A country tag instead: that country has a core on it, owner or not (`core = RUS`) |
| `occupied` | `yes`: another country (or rebels) controls the province; `no`: its owner does. An unowned province passes neither |
| `subject` | with `owner = yes`: `yes` when the owner has an overlord (any kind of subject), `no` when it has none |
| `subject_of` | with `owner = yes`, a country tag: the owner's overlord is that country |
| `religion` | a religion's key as the game's files write it (`orthodox`, compared as written): the province's religion; with `owner = yes` the owner's state religion |
| `culture` | a culture's key (`russian`): the province's culture; with `owner = yes` the owner's primary culture |
| `trade_goods` | a trade good's key (`grain`): what the province produces |
| `value` | one of the game's own numbers: the province's `development`, `base_tax`, `base_production` or `base_manpower`; with `owner = yes` the owner's `treasury`, `stability`, `prestige`, `manpower` (in thousands of men, as the game shows it), `monthly_income` (before expenses, as of the last monthly update) or `provinces` (how many it owns) |
| `owner` | `yes` or `no` (default): with `yes` the `flag`, `variable`, `modifier` or `value` is the province's owner's (`has_country_flag`, a country variable, the country's modifier value), and `always = yes owner = yes` passes every owned province. A province without an owner never passes such a rule |
| `at_least`, `at_most`, `above`, `below` | a number with at most three decimals; for `variable`, `modifier` and `value` only |
| `color` | `{ r g b }`, each 0 to 255; required. With a `scale`, the colour at its low end |
| `scale` | `{ low high }`, two numbers with at most three decimals, `low` below `high`; for `variable`, `modifier` and `value` only, and only with `high_color` |
| `high_color` | `{ r g b }`: the colour at the scale's high end; only with `scale` |
| `label` | text, 1 to 64 bytes; required: the tooltip's line and the legend's entry |

A rule has exactly one of `always`, `flag`, `variable`, `modifier`, `value`, `owned_by`, `controlled_by`, `core`, `occupied`, `subject`, `subject_of`, `religion`, `culture` and `trade_goods`. A `value` of the owner, `subject` and `subject_of` need `owner = yes`; a `value` of the province, `owned_by`, `controlled_by`, `core`, `occupied` and `trade_goods` refuse it; `religion` and `culture` take either. These ten tests, like `flag`, take no bounds and no scale. A province or country without a religion, culture or trade goods (the sea, wasteland) passes no such rule. With no bound, a `variable`, `modifier` or `value` rule passes when the value is not 0. With bounds, all of them hold together (`at_least = 1 below = 5` is 1 ≤ value < 5). A variable the province or country does not have is 0, as for the game's triggers. Numbers are compared as the game keeps them, in thousandths.

A scale does not change whether a rule passes; it only gives the colour: `color` at `low` and below, `high_color` at `high` and above, and each of red, green and blue linear between them. Write `at_least = 0` (or the scale's low end) to let a value of 0 take the low colour.

```
prosperity = {
    label = "Prosperity"
    rule = { variable = my_prosperity owner = yes at_least = 0 scale = { 0 100 } color = { 60 30 30 } high_color = { 40 200 80 } label = "Prosperity" }
}
development = {
    label = "Development"
    rule = { value = development above = 0 scale = { 3 30 } color = { 240 240 200 } high_color = { 160 0 0 } label = "Development" }
}
cores = {
    label = "Cores and occupation"
    rule = { occupied = yes color = { 200 40 40 } label = "Occupied" }
    rule = { core = no color = { 230 160 0 } label = "Owned without a core" }
    rule = { subject = yes owner = yes color = { 120 120 200 } label = "A subject's" }
}
russian_sphere = {
    label = "Russia's sphere"
    rule = { owned_by = RUS color = { 40 160 40 } label = "Russia's" }
    rule = { subject_of = RUS owner = yes color = { 120 200 120 } label = "Russia's subjects'" }
    rule = { controlled_by = RUS color = { 200 40 40 } label = "Held by Russia" }
    rule = { core = RUS color = { 230 160 0 } label = "Russia's claims" }
}
faiths = {
    label = "Russian lands"
    rule = { culture = russian color = { 200 160 40 } label = "Russian" }
    rule = { religion = orthodox owner = yes color = { 120 90 200 } label = "Under an Orthodox ruler" }
    rule = { trade_goods = grain color = { 230 220 120 } label = "Grain" }
}
```

## 4. Colours and the tooltip

- A province takes the colour of the **first** rule it passes. A province that passes none keeps the political map mode's colour, so sea, wasteland and empty land look as players know them.
- The colours are computed only while the mode is shown: when the player picks it (in the same step that publishes `anvil-map.shown`) and at each `anvil-game.month` while it stays shown. A mode nobody looks at costs nothing.
- A pass over the map reads each province's flags, variable or modifier through `anvil-game.world`: about as much as one read per rule a province reaches. An owner's answers are read once per country and computation, however many provinces it owns.
- A flag's name is looked up at each computation, so a flag that no effect had set when the mode was shown first counts from the next month on.
- A `modifier` key the game does not know is logged once (`the game has no modifier …; its rule never passes`), and its rule never passes.
- The tooltip of a province gives the label of the rule it passes; for a `variable` or `value` rule and a rule with a scale also the value, as `Faith 5 or more (my_faith 7.5)`. A province that passes no rule has no line. An owner's value is the one the colours were computed from.

## 5. The legend

Unless `legend = no`, the mode's legend lists each rule's colour and label once, in rule order (two rules with the same colour and label are one entry), at most 32 (map-modes-v1 §6a). A rule with a scale gives two entries, its label with the low end in `color` and with the high end in `high_color` (`Prosperity 0`, `Prosperity 100`); a label that would pass 64 bytes so is cut at a character.

## 6. Faults

A fault in a mode leaves out that mode whole and writes a line to `anvil.log` with the file and the line: `map mode file My mod/anvil/map_modes/holy.txt:7: a rule needs color = { r g b }`. The other modes of the file are read. Comparison operators (`>=`, `<`) are refused with a hint to write `at_least` and the others. A mode that finds the pool of 13 numbers lent is not registered and logged.

## 7. Test in the game before the release

Under the lab's game lock, in a test profile with a test mod that has `anvil/map_modes/test.txt`, on Steam and GOG:

1. The button is on its tab; picking it paints the provinces that pass the rules and leaves the others political.
2. The legend window shows with the mode and goes with it.
3. A month later, after a console effect that sets a flag on a province (`set_province_flag` on a province in the console's scope), the province changes colour.
4. The tooltip's line for a province that passes a rule, and none for one that does not.
5. A faulty file: its line in `anvil.log`, the other modes still there.
6. A rule with `owner = yes` on a country modifier colours every province of a country alike, and a scale colours between its two colours, with both ends in the legend.
7. A `value = development` scale, and an owner's `value` (`treasury`), with the value in the tooltip.
8. While the game runs, a changed colour and bound in a file show after picking another mode and the mode again, with the log's line; a changed label and a new mode are logged as waiting for a restart.
9. `owned_by`, `core = no`, `occupied = yes` and `subject = yes owner = yes` on a save with an occupation and a subject.
10. `controlled_by`, `core = TAG` on a province another country owns, and `subject_of` on the same save.
11. `religion`, `culture` and `trade_goods` of the province, and `religion` and `culture` with `owner = yes`, against the province view and the country's.
