/*
 * anvil.benchmark v1: the core's service through which plugins join the
 * start-up benchmark and report their own fields (docs/spec/benchmark-v1.md
 * §5). Look it up with get_service(self, ANVIL_BENCHMARK_SERVICE,
 * ANVIL_BENCHMARK_SERVICE_VERSION); an Anvil older than 1.4 returns NULL, and
 * the plugin then works on without the benchmark. No manifest key is
 * involved. The C++ wrapper is anvil_benchmark.hpp.
 *
 * Declaring fields inside anvil_plugin_load, even zero of them, makes the
 * plugin a benchmark participant, which promises one done() call: in a
 * benchmark start the game quits once it is ready and every participant is
 * done, or 30 s after ready. The schema is fixed for the start.
 *
 * Outside a benchmark start (active() is 0) every call checks its arguments
 * the same way and nothing is recorded, so a plugin may call it always.
 * Every function may be called from any thread. A refused call returns its
 * error and writes one warning to the plugin's log; the plugin keeps running.
 *
 * Plain C11, like anvil_plugin.h. Within service version 1 functions are only
 * appended to the end of AnvilBenchmarkService (check with
 * ANVIL_BENCHMARK_HAS); AnvilBenchmarkField does not change.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ANVIL_BENCHMARK_H
#define ANVIL_BENCHMARK_H

#include <anvil/anvil_plugin.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANVIL_BENCHMARK_SERVICE "anvil.benchmark"
#define ANVIL_BENCHMARK_SERVICE_VERSION 1

/* Limits of one plugin's declaration. Names and enum options are `group.field`
 * or `field`, from identifiers of [a-z0-9_] with zero or one dot. */
#define ANVIL_BENCHMARK_MAX_FIELDS 128
#define ANVIL_BENCHMARK_MAX_OPTIONS 32
#define ANVIL_BENCHMARK_MAX_NAME 64
#define ANVIL_BENCHMARK_MAX_LABEL 80

/* There is no string kind: the kinds keep paths and text out of a report. */
typedef enum AnvilBenchmarkKind {
    ANVIL_BENCHMARK_INT = 1,    /* int64_t */
    ANVIL_BENCHMARK_NUMBER = 2, /* double; NaN and infinity are refused */
    ANVIL_BENCHMARK_ENUM = 3,   /* one of the declared options, by name */
    ANVIL_BENCHMARK_FLAG = 4    /* false or true: 0 or nonzero */
} AnvilBenchmarkKind;

typedef enum AnvilBenchmarkUnit {
    ANVIL_BENCHMARK_UNIT_NONE = 0,
    ANVIL_BENCHMARK_UNIT_MS = 1,
    ANVIL_BENCHMARK_UNIT_COUNT = 2,
    ANVIL_BENCHMARK_UNIT_BYTES = 3,
    ANVIL_BENCHMARK_UNIT_MB = 4,
    ANVIL_BENCHMARK_UNIT_PERCENT = 5
} AnvilBenchmarkUnit;

/* One field; Anvil copies everything it points to during declare(). A name
 * never changes its meaning or kind: a new meaning gets a new name. */
typedef struct AnvilBenchmarkField {
    const char *name;
    uint32_t kind;              /* AnvilBenchmarkKind */
    uint32_t unit;              /* AnvilBenchmarkUnit; NONE for an enum or a flag */
    const char *label;          /* short English text for the player's preview only; may be NULL */
    const char *const *options; /* ENUM: the option names; NULL otherwise */
    uint32_t option_count;      /* ENUM: 1 to ANVIL_BENCHMARK_MAX_OPTIONS; 0 otherwise */
} AnvilBenchmarkField;

typedef struct AnvilBenchmarkService {
    uint32_t size; /* sizeof(AnvilBenchmarkService) as known by Anvil */

    /* 1 in a benchmark start, 0 otherwise. */
    int (*active)(void);

    /* Inside anvil_plugin_load only (ANVIL_ERROR_WRONG_PHASE later). May be
     * called more than once; each call adds its fields, all or none of them:
     * ANVIL_ERROR_INVALID_ARGUMENT for a bad name, kind, unit, label or
     * option list or over a limit, ANVIL_ERROR_ALREADY_EXISTS for a name
     * declared before. Any call, a refused one included, makes the plugin a
     * participant. */
    AnvilResult (*declare)(AnvilPlugin *self, const AnvilBenchmarkField *fields, uint32_t count);

    /* The last value wins; a field never set is null in the start's record.
     * ANVIL_ERROR_NOT_DECLARED for an unknown field (or a plugin that is not
     * a participant), ANVIL_ERROR_WRONG_KIND for another kind,
     * ANVIL_ERROR_INVALID_ARGUMENT for NaN, infinity or an unknown option,
     * ANVIL_ERROR_WRONG_PHASE after done(). */
    AnvilResult (*set_int)(AnvilPlugin *self, const char *field, int64_t value);
    AnvilResult (*set_number)(AnvilPlugin *self, const char *field, double value);
    AnvilResult (*set_enum)(AnvilPlugin *self, const char *field, const char *option);
    AnvilResult (*set_flag)(AnvilPlugin *self, const char *field, int value);

    /* The participant's work after ready is finished (its fields are set).
     * Values set later are refused. Calling it again does nothing. */
    AnvilResult (*done)(AnvilPlugin *self);

    /* This start is not representative for the plugin (e.g. a cache still
     * being built): the start is discarded and replaced. `reason` is a name
     * like a field's, e.g. "new_mod_set". Before done() only. */
    AnvilResult (*learning)(AnvilPlugin *self, const char *reason);

    /* The participant opened `files` of the game's files ahead of the game,
     * which took `ms` in total (finite, >= 0). In the arm with the compared
     * plugin, its value is the start's open cost. Before done() only; the
     * last call wins. */
    AnvilResult (*open_cost)(AnvilPlugin *self, uint64_t files, double ms);
} AnvilBenchmarkService;

/* Nonzero when `service` provides `field`. */
#define ANVIL_BENCHMARK_HAS(service, field) \
    ((service)->size >= offsetof(AnvilBenchmarkService, field) + sizeof((service)->field))

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_BENCHMARK_H */
