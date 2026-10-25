# Research: how a benchmark start ends itself, and how the series learns its time

Ticket: [#128](https://github.com/happy-dwarf-team/anvil/issues/128), map [#127](https://github.com/happy-dwarf-team/anvil/issues/127).
Date: 2026-10-25. Nothing here started the game; every game fact comes from the code, the lab's
recorded runs (EU4 engine lab, `docs/` and `work/` run records) or a static read of `eu4.exe`
1.37.5 (SHA-256 `9AD3EFE1…771A`) with `dumpbin`.

Labels: **verified** (code, docs or a recorded run say so), **static** (read from the binary, not
checked in a running game), **hypothesis** (not checked).

## Summary

- **Ending a start.** The game has one quit path, and it can be reached without the player:
  the game's own quit posts an `SDL_QUIT` event (static), and SDL 2.0.4 turns a `WM_CLOSE` on the
  game's only window into the same `SDL_QUIT` (SDL source). So either a plugin pushes `SDL_QUIT`
  on the main thread after `anvil-game.ready`, or the Manager posts `WM_CLOSE` to the game's
  window. A kill (`TerminateProcess`) is what the lab does today: it leaves `graceful_exit=no` and
  `refreshRate=60` in the player's `settings.txt` (verified, every lab run), which a clean exit did
  not (verified, n=1). Neither automatic quit has been tried live.
- **Starting a start.** `steam://rungameid/236850` runs `dowser.exe`, the Paradox launcher, and
  needs a click on Play (verified), so it cannot drive a series. Starting `eu4.exe` directly from
  the game folder with Steam running and `SteamAppId=236850` in its environment is what the lab
  does for every start (verified) and needs no rights beyond the player's. The launcher path was
  about 2 s slower on warm files (n=2 to 3, cause a hypothesis).
- **Learning the time.** Today the core logs `game ready at … ms`, counted with
  `QueryPerformanceCounter` from `AnvilEntry` (the game's entry point, before its C runtime), not
  from process creation. It only reaches `anvil.log` at `log_level = "info"` and is gone after two
  more starts. `Running application` in `system.log` has no timestamp and comes about 1.3 to 1.6 s
  before `anvil-game.ready`. A program that starts `eu4.exe` itself holds the process handle and
  knows the creation time exactly; the missing piece is a machine-readable record of the ready time
  written by the game side.
- **Workshop players and toggling.** Anvil cannot come from the Workshop, and both ways to install
  Anvil 1.3 put `AnvilManager.exe` next to `eu4.exe` (verified, design and README). Switching
  Bellows off and on through `disabled_plugins` leaves its approval, its approved copy and its
  learned profiles alone (verified, code); safe mode, a pending update and Bellows' first start on a
  new mod set are the cases a series has to detect.

## 1. A clean quit after `anvil-game.ready`

### What the game does on quit (static)

- The game links SDL 2.0.4. It calls `SDL_PushEvent` in one place only, a function at `0x156AC80`
  that builds an event of type `0x100` (`SDL_QUIT`) and pushes it (Tongs `docs/design.md`, "Input",
  and the disassembly: `mov dword [rsp+20h],100h; call SDL_PushEvent thunk`).
- The event loop at `0x1569C70` switches on the event type. Its jump table at `0x156AA0C` sends
  `0x100` to `0x156A9BC`, which does some clean-up and returns `true` from the loop.
- The frame function (`0x14C25A0`) stores that result in `app+0x2F8` and, when it is set, writes
  `1` to `app+0x83` (`0x14C267C`, `0x14C2686`).
- The main loop (`0x14C2DC3`…`0x14C2E1F`) runs frames while `app+0x83` and `app+0x2F8` are both 0,
  then calls the application's virtual function `+0x50` and the shutdown code after it. These are
  the two quit flags `anvil-game` already declares (`main_loop_frame`, `main_loop_quit` in
  `plugins/anvil-game/symbols.toml`).

So the menu's Quit and a pushed `SDL_QUIT` end in the same place (static). It was not checked
whether the menu's Quit asks for confirmation before it reaches `0x156AC80`, nor what the
clean-up at `0x156A9BC` does.

### Three ways to reach it

1. **A plugin pushes `SDL_QUIT`.** Tongs already calls `SDL_PushEvent` from its own thread
   through the SDL dynamic-API slot (`0x1FB02C0`, checked to point at `0x1746B90`); SDL locks the
   queue, so any thread may push (Tongs design). A benchmark plugin, or the core through
   `anvil-game`'s `run_on_main_thread`, could push `SDL_QUIT` or call `0x156AC80` once it has decided
   the start is over. Needs two new symbols. **Not tried live.**
2. **The Manager posts `WM_CLOSE` to the game's window.** In SDL 2.0.4 `WIN_WindowProc` turns
   `WM_CLOSE` into `SDL_WINDOWEVENT_CLOSE` (`src/video/windows/SDL_windowsevents.c`), and
   `SDL_SendWindowEvent` calls `SDL_SendQuit()` when the closed window is the last one
   (`src/events/SDL_windowevents.c`, tag `release-2.0.4`). The lab already finds the game's largest
   visible top-level window by PID and posts window messages to it from a normal, non-elevated
   process (`scripts/post-window-input.ps1`, used in the Workshop trust test on 25.10.2026). No
   game symbols are needed, and it works without any plugin. That EU4 has only one SDL window, and
   that `anvil-overlay`'s subclassed window procedure passes `WM_CLOSE` on, are **hypotheses**
   (the overlay design only says it takes mouse and keyboard messages).
3. **A console command.** Not usable: in the main menu Tongs' `console` answered `not_in_game`
   (Tongs design, "Main menu"), and no quit command was found. The string `quit` exists in the exe
   (`0x20A7194`), purpose unknown.

### What a clean quit and a kill leave behind

| | Clean quit | Kill (`TerminateProcess`) |
| --- | --- | --- |
| `settings.txt` | no difference to the baseline (`sb-play-2`, quit through the game's menu, n=1; lab `docs/anvil-gameplay.md`) | `graceful_exit=no` added and `refreshRate=144` → `60` (every recorded lab run, e.g. `br090-*`, `bellows-ws-1`; lab `docs/startup-investigation.md`, `docs/safety.md`) |
| DLL detach | `DLL_PROCESS_DETACH` runs (Microsoft: `ExitProcess`, step 3): Anvil's `shutdown()` writes the call counters (`profile = true`) and the safe-mode bookkeeping | DLLs are not notified (Microsoft: `ExitProcess`, `TerminateProcess` remarks); nothing at exit is written |
| Other threads | ended without notice before the detach (Microsoft: `ExitProcess`, step 1) | ended |
| Safe-mode count | `anvil-game.ready` already removed `unfinished-starts.txt` (`StartMarker::game_ready`), so neither way counts a start that reached ready | same |
| Logs | the game's logs in `<user dir>/logs` are rewritten by the next start; `anvil.log` becomes `anvil.prev.log` at the next start and is lost at the one after (`Log::open`) | same |

The game itself wrote `graceful_exit=no` and `refreshRate=60` during a killed run (the lab puts
the baseline in place before each start and copies the file right after the kill). Reading
(hypothesis): the game marks the session unfinished early and writes the real values at a clean
exit. For a player's benchmark this matters more than for the lab: killing every start would leave
the player's refresh rate changed, and the lab only gets away with it by restoring its baseline.
What the next start does with `graceful_exit=no` was never observed (the lab always restored the
file first).

**Restarts.** After "Exit to Menu" and Quit on the end-of-game summary the game started itself
again as its own child (`-skiplauncher`, the `continue=` of the first command line; lab
`docs/anvil-gameplay.md`, `docs/reverse-engineering.md`). Whether a quit from the main menu, or a
pushed `SDL_QUIT`, can do that is not known. A series has to watch for a child `eu4.exe` of the
start it ended, as the lab's `Stop-RestartedGame` does.

**Bellows' work after ready.** Bellows saves its learned profiles and writes its counters on a
thread it starts at `anvil-game.ready` ("bellows ready report", `src/plugin.cpp`), which waits for
its workers and can take seconds. A quit before that thread ends ends it too (both ways). Its
profile write goes through a temporary file and `MoveFileEx`, so the old profile survives
(`src/modules/access_profile.cpp`, `WriteList`), but the start's learning is lost and a `.tmp`
may stay. A series therefore needs a "plugins are done after ready" point before it quits; today
the only signal is Bellows' last log lines (`range_witness` is the last report in
`FinishAfterReady`), and the lab simply waits 3 s after ready (`scripts/anvil-smoke.ps1`).

## 2. Starting the game for a benchmark

- **`steam://rungameid/236850`** (what the Manager's "Play EU4" opens, `tools/manager`). Steam's
  cached app configuration lists `dowser.exe` as EU4's default Windows launch (read from Steam's
  local `appinfo.vdf`), and in the lab it showed the Paradox launcher, whose Play button had to be
  clicked; `eu4.exe`'s parent was `Paradox Launcher` (lab `docs/anvil-real-folder.md`). A click too
  early was ignored (`sb-launcher-2`). The launcher rewrites `dlc_load.json` from its active
  playset (lab `docs/safety.md`). Not usable for an unattended series.
- **`eu4.exe` started directly, Steam running.** The launcher itself runs `./eu4.exe
  -enabletelemetry -gdpr-compliant` with the game folder as working directory
  (`launcher-settings.json`). `eu4.exe` imports `SteamAPI_Init` but not
  `SteamAPI_RestartAppIfNecessary` (its import table), so a direct start does not bounce back
  through Steam. Steam's documentation says that outside Steam `SteamAPI_Init` needs the App ID,
  e.g. from `steam_appid.txt`; the game folder has none, and the lab passes `SteamAppId` and
  `SteamGameId = 236850` in the child's environment (`scripts/lib/LabProcess.psm1`), which worked
  in every lab start. Steam's API page does not mention the variable, so that it is what makes
  `SteamAPI_Init` succeed is a **hypothesis**, as is what the game does without it or without Steam
  running.
- **What each adds to the measured time** (counted from the creation of `eu4.exe`, so the
  launcher's own start-up and the click are outside it):
  - launcher, warm files, Anvil + the startup plugin: 13.72 / 13.81 / 14.79 s, against 11.91 /
    11.97 s for a direct start (lab `docs/anvil-real-folder.md`, n=1 to 3 each);
  - only launcher starts load `gameoverlayrenderer64.dll` and `steamclient64.dll` (verified, read
    8 s after creation). That the Steam overlay causes the ~2 s is a hypothesis;
  - without Anvil the launcher start took 30.87 / 30.08 s.

A series that starts `eu4.exe` itself gets the process handle (`CreateProcess`), the exact
creation time and the exit without searching for the process, and both arms of the comparison go
the same way. Its numbers are those of a direct start, which is also how the Bellows README table
was measured.

## 3. How the Manager learns that a start ended, and its time

| Source | What it holds | Clock and origin | Limits |
| --- | --- | --- | --- |
| `anvil.log`: `game ready at … ms` | first main-loop frame after the game's ready byte (`0x233FF4E`), from `anvil-game.ready` (`src/core/runtime.cpp`, `on_game_ready`) | `QueryPerformanceCounter` since `AnvilEntry`, before the game's C runtime (`startup_impl`) | an `info` line: absent at `log_level = "warning"` or `"error"`; free text, not a contract; the file is opened with `FILE_SHARE_READ`, so it can be read while the game runs; rotated at the next start |
| `anvil.log` line prefixes `[ … ms]` | every line | QPC since the log state's first use, a separate origin that is close to, not equal to, `AnvilEntry` (`src/core/log.cpp`) | as above |
| `system.log`: `Running application...` | written by the game (`main.cpp:1721`) | none: the line has no time; an outside program only sees when the file changed | 1.3 to 1.6 s before `game ready` in the same runs (`br090-bellows-1..3`: 13.03 vs 14.36 s, 13.32 vs 14.90 s, 12.26 vs 13.85 s; `br090-standard-1`: 38.51 vs 40.10 s; both polled every 250 ms); also present without Anvil; it is the event the Bellows README table uses |
| The process | creation time (`GetProcessTimes`: FILETIME, UTC, 100 ns units) and exit | system time | needs `PROCESS_QUERY_LIMITED_INFORMATION`, which a normal user has for their own process |

Facts that bear on the choice:

- QPC is monotonic, consistent across threads and processes on current Windows, and independent of
  system time changes; it is not tied to UTC (Microsoft, "Acquiring high-resolution time stamps").
  `GetSystemTimePreciseAsFileTime` is the UTC clock with the same resolution and the same base as
  the process creation time.
- The gap between process creation and `AnvilEntry` is not logged anywhere. The lab's numbers give
  an upper bound: its polled ready time (from just before `CreateProcess`) minus `game ready at`
  was 0.10 to 0.28 s in `br090-bellows-1..3` and `br090-standard-1`, which also contains up to
  250 ms of polling.
- The series design therefore has three ways to get a ready time from process creation, all
  **proposals**: (a) the game side records, at `AnvilEntry`, the QPC value and
  `GetSystemTimePreciseAsFileTime`, and at ready the QPC value, and writes them with the process
  creation time to a small file (for example under `anvil/data/anvil/`), which the Manager reads
  after the game has exited; (b) the Manager converts its own QPC reading at the moment it learns
  of readiness, which adds its polling delay; (c) the Manager parses `anvil.log` as the lab does.
  (a) keeps QPC for the long interval and uses system time only for the short creation→entry gap.
- Readiness is a byte the game sets once at the end of start-up; the lab notes that `ready=1`
  alone does not prove a visible menu (lab `AGENTS.md`, "Pomiary"). `game ready` is published one
  frame later, by `anvil-game` (§13.1).

## 4. Workshop players, the Manager, and switching a plugin between starts

- **Do they have the Manager?** Bellows from the Workshop needs Anvil 1.3 or newer (Bellows README),
  and Anvil cannot be installed by the Workshop (design §14). Anvil is installed either by the
  Manager, which copies itself to `<EU4>/AnvilManager.exe`, or by hand from `anvil-windows-x64.zip`,
  which contains `AnvilManager.exe` (design §15, §20; README "Install / uninstall"). So every such
  player has the Manager next to `eu4.exe` unless they deleted it. A benchmark added in a later
  Manager reaches them only when they run that newer Manager, which is also how Anvil updates.
- **`disabled_plugins`** is applied in the resolver to every candidate, local or from a mod
  (`src/core/resolver.cpp`: "disabled in anvil.toml"). Plugins from mods are discovered, hashed and
  checked against `trust.toml` before that (`src/core/runtime.cpp`), so both arms pay the discovery
  (26.2 ms with one mod in the Workshop test; 2 to 3 ms in the design's numbers) and the arm
  without Bellows does not load its DLL at all. The Manager already edits `disabled_plugins`, only
  while no `eu4.exe` of that folder runs (design §15).
- **Approval per version.** Disabling neither touches `trust.toml` nor the approved copy; the next
  enabled start loads the same approved copy (§14.1). A Workshop update during a series makes the
  plugin "update pending", and the old approved copy keeps loading until the player approves (§14.1),
  so a series stays on one version, but the report should name the version and hash that loaded
  (the log line `approved on … (hash …); loads from …`). A first approval in the overlay takes
  effect only from the next start, so a series must start with Bellows already approved (the
  Manager can approve before launch).
- **Bellows' learned profiles** are keyed by the build and the hash of `dlc_load.json`
  (`anvil/data/bellows/profiles/<build>-<dlc_load hash>.txt`) and saved at ready by Bellows itself
  (Bellows `docs/design.md` §3a). A start without Bellows does not read or write them. Disabling
  the mod in the playset instead would change `dlc_load.json`, and with it the profile key and the
  mod set, so `disabled_plugins` is the right switch. The first start on a new mod set uses the
  shipped base layer (README: 13.61 s against a 13.03 s median after learning), so a series
  should recognise it. Whether the "unused for N starts" pruning counts only Bellows' own starts
  was not checked in the code (**hypothesis**: yes, since only Bellows writes the file).
- **Safe mode.** Two starts in a row that end before ready make the next start load no plugins
  from mods ("safe mode", §10, `runtime.cpp`). A series that kills or loses a start before ready
  can thus produce a "with Bellows" start without Bellows; the series has to read the status (log
  or `anvil.control`) and discard it. Starts that reach ready, however they end, do not count.

## Not verified

- Any automatic quit in a running game: a pushed `SDL_QUIT`, `WM_CLOSE` to the window, with and
  without `anvil-overlay`; whether either asks for confirmation, how long the shutdown takes, and
  whether it can make the game start itself again.
- What a clean quit writes to `settings.txt` beyond the one lab session, and what the game does at
  the start after `graceful_exit=no`.
- A direct start without `SteamAppId`, or without Steam running.
- The creation→`AnvilEntry` gap on its own (only an upper bound from polled lab runs).
- The cause of the slower launcher start.

## Sources

- Anvil: `docs/design.md` §3, §10, §12–15, §20; `src/core/runtime.cpp`, `log.cpp`, `resolver.cpp`,
  `safe_mode.cpp`; `src/anvil_main.cpp`; `plugins/anvil-game/src/plugin.cpp`, `symbols.toml`;
  `tools/manager/src/game.cpp`, `game.hpp`; `README.md`.
- Bellows: `README.md`; `docs/design.md` §3a; `src/plugin.cpp`; `src/modules/access_profile.cpp`,
  `open_warmup.cpp`, `ram_prefetch.cpp`; `plugin/plugin.toml`.
- Tongs: `docs/design.md` ("Input", "Main menu", live results).
- EU4 engine lab: `docs/anvil-real-folder.md`, `anvil-gameplay.md`, `anvil-workshop-trust-game-test.md`,
  `reverse-engineering.md`, `safety.md`, `startup-investigation.md`; `scripts/anvil-smoke.ps1`,
  `scripts/lib/LabProcess.psm1`, `scripts/lib/StartupBaseline.psm1`, `scripts/post-window-input.ps1`;
  run records `work/br090-*`, `work/bellows-ws-1`.
- `eu4.exe` 1.37.5: `dumpbin /disasm` of `0x14C25A0`–`0x14C2EA0`, `0x1569C70`–`0x156AD20`, the jump
  table at `0x156AA0C`; `dumpbin /imports`; strings (`graceful_exit`, `-skiplauncher`,
  `Running application...`); the game folder's `launcher-settings.json`.
- SDL 2.0.4: [`SDL_windowsevents.c`](https://github.com/libsdl-org/SDL/blob/release-2.0.4/src/video/windows/SDL_windowsevents.c),
  [`SDL_windowevents.c`](https://github.com/libsdl-org/SDL/blob/release-2.0.4/src/events/SDL_windowevents.c).
- Microsoft: [ExitProcess](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-exitprocess),
  [TerminateProcess](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-terminateprocess),
  [GetProcessTimes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesstimes),
  [Acquiring high-resolution time stamps](https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps).
- Steam: [Steamworks API overview](https://partner.steamgames.com/doc/sdk/api) (initialization,
  `steam_appid.txt`, `SteamAPI_RestartAppIfNecessary`); Steam's local app configuration cache for
  app 236850.
