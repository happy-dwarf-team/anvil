# PROTOTYPE: bench-quit (throwaway, never merged)

Answers [#134](https://github.com/happy-dwarf-team/anvil/issues/134) of the start-up benchmark map
[#127](https://github.com/happy-dwarf-team/anvil/issues/127): does a benchmark start quit itself
cleanly once the game is ready, and what does it leave for the next start?

- `bench_quit.cpp`: after `anvil-game.ready` it waits `BENCH_QUIT_DELAY_MS` (default 3000) and
  queues work on the main thread that pushes `SDL_QUIT` through the game's SDL dynamic-API slot
  (the symbols are Tongs'). `BENCH_QUIT_MODE=off` keeps it passive.
- `lab-run-quit-trial.ps1`: the harness used in the EU4 engine lab (needs the lab's
  `scripts/lib` modules and folders; kept as a record, not runnable elsewhere). It starts the
  original `eu4.exe` in a junction copy suspended with `CreateProcess`, waits for both
  `Running application` and `game ready at`, then ends the start one of three ways: the plugin
  (`sdl`), `WM_CLOSE` posted to the game's window (`wmclose`, what the Manager would do), or a
  verified `TerminateProcess` (`kill`); afterwards it looks for a restarted game for 15 s and diffs
  `settings.txt` against the file the start began with.

Build against an Anvil SDK: `cmake -S prototypes/bench-quit -B build -G Ninja
-DAnvilPlugin_DIR=<sdk>/cmake && cmake --build build`.

## Results (2026-10-25, EU4 1.37.5 `9AD3EFE1…771A`, Anvil 1.3.0 release, n=1 per row)

Child affinity CPU 1–7, `eu4.exe` started directly with the game folder as working directory.
"Quit → exit" is from the push or post (or kill) to the end of the process.

| Start | Ends by | Exit code | Quit → exit | `settings.txt` afterwards | Restart |
| --- | --- | --- | --- | --- | --- |
| anvil-game + bench-quit | `SDL_QUIT` 3 s after ready | 0 | ~1 s | unchanged | none |
| same | `WM_CLOSE` | 0 | ~1 s | unchanged | none |
| + anvil-overlay | `SDL_QUIT` / `WM_CLOSE` | 0 / 0 | 1.07 / 1.10 s | unchanged | none |
| + Bellows 0.9.0 (Workshop build) | `SDL_QUIT` 3 s after ready | 0 | 1.07 s | unchanged; profiles saved | none |
| + Bellows | `SDL_QUIT` 0 ms after ready | 0 | 1.19 s | unchanged; profiles saved (see below) | none |
| + Bellows | kill 3 s after ready | 1 | 0.2 s | `graceful_exit=no` added, `refreshRate` 144 → 60 | none |
| + Bellows + overlay, started with the killed start's `settings.txt` | `SDL_QUIT` | 0 | 1.11 s | written back byte-equal to the pre-kill file | none |
| no `SteamAppId`/`SteamGameId`, Steam running | `SDL_QUIT` | 0 | ~1.1 s | unchanged | none |
| `SteamAppId`, Steam closed | `SDL_QUIT` | 0 | ~1.1 s | unchanged | none |

Observations:

- Neither way asks for confirmation; both end like the menu's Quit. `anvil-overlay`'s window
  procedure passes `WM_CLOSE` on.
- No `anvil/data/anvil/unfinished-starts.txt` after any start, the kill after ready included, so
  safe mode does not count such starts.
- The start after a kill showed no dialog and wrote the same game logs as a normal start; its clean
  exit restored `refreshRate` and dropped `graceful_exit`. The only lasting effect of a kill is the
  wrong `refreshRate` until the next clean exit.
- With a 0 ms delay Bellows still saved its profiles (~220 ms after ready), only because the first
  frame after ready took ~390 ms before the queued work ran. That is a race, not a guarantee: a
  series needs a "plugins are done after ready" point or a margin.
- Steam modules in the process at ready: with `SteamAppId` and Steam running
  `steam_api64.dll`, `steamclient64.dll`, `gameoverlayrenderer64.dll`; without `SteamAppId`, or
  with Steam closed, only `steam_api64.dll`. The game logs (DLC list included) were identical in
  all three, and a closed Steam was not started. So those starts run without the Steam client and
  its overlay: a different start from the player's usual one.
- Not measured: start-time effects of the above (the Steam-closed start reached ready in 50 s
  against ~41 s, n=1, after a long pause; cause unknown). `GetProcessTimes` returned no exit time
  in this harness, so "quit → exit" is polled every 100 ms.
