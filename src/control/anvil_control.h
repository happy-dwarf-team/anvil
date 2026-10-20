/*
 * anvil.control v1: the core's private service for anvil-overlay (design §15,
 * #58). Status and start-up warnings, the settings of every plugin, the
 * previous crash report, the log tail and "disable at the next start".
 *
 * Not part of the SDK: it lives in the Anvil repository, only Anvil's own
 * plugins use it, and it may change in a minor release (a new version is
 * registered next to the old one only when a released overlay needs it).
 * Every function may be called from any thread. Strings are UTF-8.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ANVIL_CONTROL_H
#define ANVIL_CONTROL_H

#include <anvil/anvil_plugin.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANVIL_CONTROL_SERVICE "anvil.control"
#define ANVIL_CONTROL_VERSION 1

/* Receives a text that is valid only during the call. */
typedef void (*AnvilControlSink)(void *context, const char *text, size_t size);

typedef struct AnvilControlStatus {
    uint32_t size;
    const char *anvil_version;
    const char *game_build; /* NULL: unknown build */
    int loading_done;       /* every plugin was processed (anvil.plugins_loaded) */
    int safe_mode;          /* this start loads only the standard plugins */
    uint32_t unfinished_starts; /* starts before this one that ended before the game was ready */
    uint32_t plugins;           /* entries of plugin(): resolved, then skipped */
    uint32_t loaded, failed, skipped;
    uint32_t warnings;          /* entries of warning() */
    /* The current game session (anvil-game.session), design §13. */
    int session;
    int ironman, multiplayer;
    /* Loaded plugins with affects_gameplay = true that act now (not suspended). */
    uint32_t gameplay_active;
    /* A hash of the active gameplay plugin set (ids and versions), for
     * players to compare by hand in multiplayer; "" when there are none. */
    char gameplay_hash[20];
} AnvilControlStatus;

typedef struct AnvilControlPlugin {
    uint32_t size;
    /* Valid for the process. */
    const char *id;
    const char *name;    /* "" for a skipped plugin whose manifest did not parse */
    const char *version; /* "" likewise */
    const char *state;   /* "pending" | "loaded" | "failed" | "skipped" */
    const char *reason;  /* why it was skipped or failed; "" otherwise */
    const char *issues;  /* plugin.toml `issues`, may be "" */
    int early;
    int affects_gameplay;
    int suspended;           /* taken out of the current session (design §13) */
    int disabled;            /* listed in disabled_plugins at this start */
    int disabled_next_start; /* listed in disabled_plugins of anvil.toml now */
    uint32_t settings;       /* declared settings (setting_info) */
} AnvilControlPlugin;

enum {
    ANVIL_CONTROL_SETTING_BOOL = 0,
    ANVIL_CONTROL_SETTING_INT = 1,
    ANVIL_CONTROL_SETTING_FLOAT = 2,
    ANVIL_CONTROL_SETTING_ENUM = 3,
    ANVIL_CONTROL_SETTING_STRING = 4,
    ANVIL_CONTROL_SETTING_KEY = 5
};

typedef struct AnvilControlSetting {
    uint32_t size;
    /* Valid for the process. */
    const char *name;
    const char *label;
    const char *description;
    const char *group;
    uint32_t type;     /* ANVIL_CONTROL_SETTING_* */
    int restart;       /* a new value applies at the next start */
    int has_min, has_max, has_step;
    double min, max, step; /* int and float */
    uint32_t options;      /* enum: entries of setting_option */
} AnvilControlSetting;

/* Which value setting_value gives. */
enum {
    ANVIL_CONTROL_VALUE_CURRENT = 0, /* what the plugin reads now */
    ANVIL_CONTROL_VALUE_SAVED = 1,   /* what the next start reads (the player's file) */
    ANVIL_CONTROL_VALUE_DEFAULT = 2
};

/* A value for setting_set: `integer` for bool (0/1), int, enum (the option's
 * index) and key (the code), `real` for float, `text` for string. */
typedef struct AnvilControlValue {
    int64_t integer;
    double real;
    const char *text;
} AnvilControlValue;

typedef struct AnvilControlResult {
    uint32_t size;
    int applied;       /* settings: the plugin reads the new value now */
    int saved;         /* the file holds it */
    uint32_t notified; /* settings: callbacks run */
    char error[256];   /* why, when the call did not return ANVIL_OK or the file was not written */
} AnvilControlResult;

typedef struct AnvilControlCrash {
    uint32_t size;
    /* Valid for the process. */
    const char *name;    /* the report's file name without extension, e.g. "2026-10-20_18-05-11" */
    const char *level;   /* "in_plugin_code" | "in_plugin_change" | "plugin_on_stack" | "no_plugin" */
    const char *summary; /* "in plugin x's code", ... */
    const char *plugins; /* the plugins involved, most involved first, ", " between them */
    const char *issues;  /* the issues link of the first involved plugin that has one, or "" */
    const char *report;  /* the text report (.txt) */
    const wchar_t *path; /* of the text report */
    int before_game_ready;
    /* Written since the last start that showed it: notify. The first call of
     * previous_crash in a process marks the report as shown for later starts. */
    int is_new;
} AnvilControlCrash;

typedef struct AnvilControlService {
    uint32_t size; /* sizeof(AnvilControlService) as known by the core */

    void (*status)(AnvilControlStatus *out);
    /* 0 when index >= status.plugins. */
    int (*plugin)(uint32_t index, AnvilControlPlugin *out);
    /* A warning or error the log got (most of them at start-up), as
     * "[source] message"; 0 when index >= status.warnings. */
    int (*warning)(uint32_t index, AnvilControlSink sink, void *context);

    /* --- settings of resolved plugins (design §11) --- */
    uint32_t (*setting_count)(const char *plugin_id); /* 0: unknown plugin or none declared */
    int (*setting_info)(const char *plugin_id, AnvilSetting setting, AnvilControlSetting *out);
    int (*setting_option)(const char *plugin_id, AnvilSetting setting, uint32_t index, const char **name,
                          const char **label);
    /* The value as text, as setting_set_text reads it: true/false, a decimal
     * int, a number, an option's name, the string, a key ("Ctrl+F12"). */
    int (*setting_value)(const char *plugin_id, AnvilSetting setting, uint32_t which, AnvilControlSink sink,
                         void *context);
    /* Check, save in anvil/data/<id>/settings.toml, and for restart = false
     * apply and run the plugin's callbacks on this thread. NOT_FOUND for an
     * unknown plugin, NOT_DECLARED for an unknown setting, INVALID_ARGUMENT
     * for a value that does not fit. */
    AnvilResult (*setting_set)(const char *plugin_id, AnvilSetting setting, const AnvilControlValue *value,
                               AnvilControlResult *out);
    AnvilResult (*setting_set_text)(const char *plugin_id, AnvilSetting setting, const char *text,
                                    AnvilControlResult *out);
    AnvilResult (*setting_reset)(const char *plugin_id, AnvilSetting setting, AnvilControlResult *out);
    /* Warnings of the player's file and of ANVIL_<ID>_<NAME> overrides. */
    int (*setting_warning)(const char *plugin_id, uint32_t index, AnvilControlSink sink, void *context);

    /* --- crashes and the log --- */
    /* The newest crash report written before this start; 0 when none. */
    int (*previous_crash)(AnvilControlCrash *out);
    /* The last lines of anvil.log (at most max_lines, at most 64 KB), one
     * call of the sink with the whole text. Returns the number of lines. */
    uint32_t (*log_tail)(uint32_t max_lines, AnvilControlSink sink, void *context);

    /* --- "disable at the next start" --- */
    /* Adds the id to disabled_plugins in anvil/anvil.toml, keeping the rest
     * of the file and its comments, written atomically. Only on the player's
     * request. ANVIL_OK when it is listed (also when it already was). */
    AnvilResult (*disable_next_start)(const char *plugin_id, AnvilControlResult *out);
} AnvilControlService;

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_CONTROL_H */
