/*
 * anvil-game: the standard plugin that knows the game's main loop
 * (docs/design.md §4, §13). It is a plugin like any other: depend on it in
 * plugin.toml ([[dependencies]] id = "anvil-game", version = "^1") and look up
 * its service, or subscribe to its events. Every event below is published on
 * the game's main thread, from anvil-game's hook of the frame function.
 *
 * Plain C11, like anvil_plugin.h. Within service version 1 fields are only
 * appended to the end of a struct; check `size` before using a later one.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ANVIL_GAME_H
#define ANVIL_GAME_H

#include <anvil/anvil_plugin.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANVIL_GAME_ID "anvil-game"

/* Once, when the game has finished initializing (the main menu is up).
 * Payload NULL. A plugin that subscribes later asks the service (ready). */
#define ANVIL_GAME_EVENT_READY "anvil-game.ready"

/* After every frame of the main loop while the game is not quitting.
 * Payload const AnvilGameFrame*. Frames drawn from inside a game update or a
 * save load are not reported. Keep the handler short: it runs every frame. */
#define ANVIL_GAME_EVENT_FRAME "anvil-game.frame"

/* A game session started or ended: a save or a new game is loaded and
 * running, or the player left it (to the menu, or by loading another save,
 * which ends the old session and starts the new one). Payload
 * const AnvilGameSession*. Anvil itself listens to it to suspend the changes
 * of plugins with affects_gameplay = true in ironman and multiplayer (§13);
 * it does so before any plugin is told. Quitting the game from a session
 * publishes no ENDED: frames stop first. */
#define ANVIL_GAME_EVENT_SESSION "anvil-game.session"

typedef struct AnvilGameFrame {
    uint32_t size;   /* sizeof(AnvilGameFrame) as known by anvil-game */
    uint32_t reserved;
    uint64_t number; /* the game's own frame counter after this frame */
} AnvilGameFrame;

typedef enum AnvilGameSessionChange {
    ANVIL_GAME_SESSION_STARTED = 1,
    ANVIL_GAME_SESSION_ENDED = 2
} AnvilGameSessionChange;

typedef struct AnvilGameSession {
    uint32_t size;        /* sizeof(AnvilGameSession) as known by anvil-game */
    uint32_t change;      /* AnvilGameSessionChange; ENDED carries the flags the session had */
    uint32_t ironman;     /* nonzero: an ironman game */
    uint32_t multiplayer; /* nonzero: a multiplayer game */
    uint64_t number;      /* 1 for the first session since the game started, then 2, ... */
} AnvilGameSession;

/* Work for the main thread; `context` is the caller's. */
typedef void (*AnvilGameWork)(void *context);

#define ANVIL_GAME_SERVICE "anvil-game.game"
#define ANVIL_GAME_SERVICE_VERSION 1

typedef struct AnvilGameService {
    uint32_t size; /* sizeof(AnvilGameService) as known by anvil-game */

    /* Nonzero once ANVIL_GAME_EVENT_READY was published. */
    int (*ready)(void);
    /* The game's main thread (the thread of the main loop); 0 before its
     * first frame. */
    uint32_t (*main_thread_id)(void);
    /* The frame number of the last ANVIL_GAME_EVENT_FRAME; 0 before it. */
    uint64_t (*frame_number)(void);
    /* Queues `work` to run on the main thread after the next frame of the
     * main loop, in the order queued, before that frame's
     * ANVIL_GAME_EVENT_FRAME. Callable from any thread, also from the main
     * thread (it then runs a frame later). Work still queued when the game
     * quits never runs, and frames stop while a save loads, so never wait for
     * it without a limit. ANVIL_ERROR_INVALID_ARGUMENT for a NULL `work`. */
    AnvilResult (*run_on_main_thread)(AnvilGameWork work, void *context);
    /* The current session: nonzero and *out filled (change =
     * ANVIL_GAME_SESSION_STARTED) while one runs, 0 otherwise. `out` may be
     * NULL. Its view is as of the last frame of the main loop. */
    int (*session)(AnvilGameSession *out);
} AnvilGameService;

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_GAME_H */
