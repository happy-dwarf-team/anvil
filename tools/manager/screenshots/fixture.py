"""Lays out the game folder the Manager's screenshot mode draws (ADR 0013, point 13).

    python -I tools/manager/screenshots/fixture.py <build dir> <out dir>

<out>/game is an EU4 folder with the package the build's AnvilManager.exe
carries (tools/manager/anvil-package.zip) and a few plugins that show the
Plugins tab's states: modules (one off with a kept multiplayer opt-in), settings with two changed (one a yes/no with a
long label), a plugin that is off, one that needs it, one whose plugin.toml does
not parse, a plugin from a mod that waits for approval, a copy of Camera in that
mod that the local Camera shadows (its settings shown read-only), and a retired setting
the next start moves into anvil.toml (ADR 0013, point 4), which turns its module
off (ADR 0014). <out>/user is EU4's user folder (game/userdir.txt
names it).

eu4.exe is a stand-in (the build's anvil.exe), so run the Manager with
--build eu4-1.37.5-steam-win64. The standard plugins lose their `symbols` line:
their symbols could only be checked against the real eu4.exe. The Anvil tab
therefore lists those plugin.toml files as changed. Nothing here is the game's.
"""

import json
import pathlib
import shutil
import sys
import zipfile

build, out = pathlib.Path(sys.argv[1]).resolve(), pathlib.Path(sys.argv[2]).resolve()
game, user = out / "game", out / "user"
if out.exists():
    shutil.rmtree(out)


def write(path: pathlib.Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


# A 64-bit PE that is not the game: the build's own anvil.exe.
game.mkdir(parents=True)
shutil.copyfile(build / "Release" / "anvil.exe", game / "eu4.exe")
write(game / "userdir.txt", str(user) + "\n")
with zipfile.ZipFile(build / "tools" / "manager" / "anvil-package.zip") as package:
    package.extractall(game)
for manifest in (game / "anvil" / "plugins").glob("*/plugin.toml"):
    lines = manifest.read_bytes().splitlines(keepends=True)
    manifest.write_bytes(b"".join(l for l in lines if not l.lstrip().startswith(b"symbols")))


def plugin(folder: pathlib.Path, id: str, name: str, body: str, settings: str = "") -> None:
    head = (f'id = "{id}"\nname = "{name}"\nversion = "2.1.0"\nauthors = ["Fixture"]\napi = 2\n'
            f'entry = "{id}.dll"\nphase = "main"\n')
    if settings:
        head += 'settings = "settings.toml"\n'
        write(folder / "settings.toml", settings)
    write(folder / "plugin.toml", head + body)
    write(folder / f"{id}.dll", "MZ fixture\n")


plugins = game / "anvil" / "plugins"
plugin(plugins / "tuning", "tuning", "Tuning", '''description = "Small fixes to the economy and the army screens, each a module of its own."
affects_gameplay = false

[[modules]]
id = "trade_tooltips"
name = "Trade tooltips"
description = "Shows where a trade node's value goes."
affects_gameplay = false

[[modules]]
id = "drill_all"
name = "Drill all selected armies"
description = "One key drills every selected army."
affects_gameplay = true
allow_ironman = "opt-in"
allow_multiplayer = "opt-in"

[[modules]]
id = "merge_fleets"
name = "Merge fleets in port"
affects_gameplay = true
allow_ironman = "opt-in"
allow_multiplayer = "opt-in"
default = false

[[modules]]
id = "ledger_export"
name = "Ledger export"
description = "Writes the ledger's tables to anvil/data/tuning."
affects_gameplay = false
default = false
''', '''[setting.skip_drilled]
type = "bool"
default = false
label = "Skip armies whose drill is already at its maximum"
group = "Army"

[setting.hotkey]
type = "key"
default = "Ctrl+Shift+D"
label = "Drill hotkey"
description = "Drills every selected army."
group = "Army"

[setting.max_armies]
type = "int"
default = 20
min = 1
max = 200
label = "Most armies drilled at once"
group = "Army"

[setting.tooltip_style]
type = "enum"
default = "brief"
values = [{ name = "brief", label = "Brief" }, { name = "full", label = "Full" }]
label = "Tooltip style"
group = "Economy"

[retired.tooltips_mode]
module = "trade_tooltips"
off = ["off"]
''')
write(game / "anvil" / "data" / "tuning" / "settings.toml", "max_armies = 40\nskip_drilled = true\ntooltips_mode = \"off\"\n")
plugin(plugins / "camera", "camera", "Camera", '''description = "Smoother zoom and a key to jump to the capital."
affects_gameplay = false
''', '''[setting.zoom_speed]
type = "float"
default = 1.0
min = 0.25
max = 4.0
label = "Zoom speed"
''')
plugin(plugins / "ledger-lib", "ledger-lib", "Ledger library", '''description = "Shared code for plugins that read the ledger."
affects_gameplay = false
''')
plugin(plugins / "trade-ledger", "trade-ledger", "Trade ledger", '''description = "A ledger page of trade income by node."
affects_gameplay = false

[[dependencies]]
id = "ledger-lib"
version = "*"
''')
write(plugins / "broken" / "plugin.toml", 'id = "broken"\nname = "Broken"\nversion = "1.0"\n')
# Merge fleets in port is off and keeps its multiplayer opt-in: its row names it.
write(game / "anvil" / "anvil.toml", 'disabled_plugins = ["ledger-lib"]\nallow_ironman = ["tuning.drill_all"]\n'
      'allow_multiplayer = ["tuning.merge_fleets"]\n')

# Two crash reports for the Crashes page, in the layout Anvil writes (two-space indent):
# one in a plugin's code, one with no plugin named.
crashes = game / "anvil" / "crashes"
write(crashes / "2026-11-12_21-04-33.json", json.dumps({
    "time": "2026-11-12 21:04:33", "level": "in_plugin_code", "summary": "in plugin tuning's code",
    "plugins_involved": ["tuning"], "issues": [{"plugin": "tuning", "url": "https://example.invalid/tuning/issues"}],
    "before_game_ready": False}, indent=2))
write(crashes / "2026-11-12_21-04-33.txt", "Anvil crash report (fixture)\nException: 0xC0000005 at tuning.dll+0x1a2b\n")
write(crashes / "2026-11-10_18-30-02.json", json.dumps({
    "time": "2026-11-10 18:30:02", "level": "no_plugin", "summary": "", "plugins_involved": [], "issues": [],
    "before_game_ready": True}, indent=2))
write(crashes / "2026-11-10_18-30-02.txt", "Anvil crash report (fixture)\nException: 0xC0000005 at eu4.exe+0x4b1f33\n")

# A log in the layout Anvil writes, with a warning and an error for the Log page's levels.
write(game / "anvil" / "anvil.log", "".join(f"[{ms:9.1f} ms] {level:<5} [{source}] {text}\r\n" for ms, level, source, text in [
    (0.4, "info", "anvil", "Anvil 1.23.0 in eu4.exe (eu4-1.37.5-steam-win64)"),
    (3.1, "info", "anvil", "6 plugins found, 1 off"),
    (12.8, "info", "tuning", "loaded 2 of 4 modules"),
    (13.0, "WARN", "anvil", "broken: plugin.toml does not parse: missing api"),
    (13.2, "info", "camera", "zoom speed 1.0"),
    (4210.6, "ERROR", "trade-ledger", "needs ledger-lib, which is off"),
    (5120.3, "info", "anvil-game", "ready"),
]))

plugin(user / "mod" / "fixture-mod" / "anvil" / "sieges", "sieges", "Siege overview", '''description = "A list of every siege the player's armies hold."
affects_gameplay = false
''')
# The same id as the local Camera, with a setting of its own: the local one loads instead,
# and this entry's settings (the same player's file, read with its schema) are shown read-only.
plugin(user / "mod" / "fixture-mod" / "anvil" / "camera", "camera", "Camera", '''description = "Smoother zoom and a key to jump to the capital."
affects_gameplay = false
''', '''[setting.zoom_speed]
type = "float"
default = 1.0
min = 0.25
max = 4.0
label = "Zoom speed"

[setting.edge_pan]
type = "bool"
default = true
label = "Move the map at the screen's edge"
''')
write(game / "anvil" / "data" / "camera" / "settings.toml", "zoom_speed = 1.5\n")
write(user / "mod" / "fixture-mod.mod", 'name="Fixture mod"\npath="mod/fixture-mod"\n')
write(user / "dlc_load.json", '{"enabled_mods":["mod/fixture-mod.mod"],"disabled_dlcs":[]}')
print(game)
