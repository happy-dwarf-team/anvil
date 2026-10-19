/* Hello in C: a function hook and a mid hook on declared symbols, with the
 * plain C API (anvil_plugin.h) and the C header `anvil gen` writes. */
#include <anvil/anvil_plugin.h>

#include "symbols.gen.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>

static const AnvilHost *g_host;
static AnvilPlugin *g_self;
static sym_speed_object_fn g_speed_next; /* the rest of the chain, written by Anvil */
static volatile LONG g_first = 1;

/* Must keep the declared signature, pass on through next, and never unwind. */
static void *on_speed_object(void *session) {
    void *speed = g_speed_next(session);
    if (InterlockedExchange(&g_first, 0) != 0) {
        char message[96];
        snprintf(message, sizeof(message), "first game-speed object: %p", speed);
        g_host->log(g_self, ANVIL_LOG_INFO, message);
    }
    return speed;
}

static void on_idle_exit(AnvilCpuContext *cpu, void *user) {
    char message[96];
    (void)user;
    snprintf(message, sizeof(message), "returning to the menu (rcx=0x%llx)", (unsigned long long)cpu->rcx);
    g_host->log(g_self, ANVIL_LOG_INFO, message);
}

static void on_plugins_loaded(const char *event, const void *payload, void *context) {
    (void)event;
    (void)payload;
    (void)context;
    g_host->log(g_self, ANVIL_LOG_INFO, "every plugin is loaded");
}

ANVIL_PLUGIN_EXPORT AnvilResult anvil_plugin_load(const AnvilHost *host, AnvilPlugin *self) {
    char message[160];
    AnvilResult r;
    if (host->api_version != ANVIL_API_VERSION) return ANVIL_ERROR_UNSUPPORTED;
    g_host = host;
    g_self = self;
    snprintf(message, sizeof(message), "hello from C on Anvil %s, game build %s", host->anvil_version,
             host->game->build_id ? host->game->build_id : "unknown");
    host->log(self, ANVIL_LOG_INFO, message);

    /* On any failure return it: Anvil rolls back everything staged so far. */
    r = host->hook_function(self, SYM_SPEED_OBJECT, (void *)on_speed_object, (void **)&g_speed_next, NULL);
    if (r != ANVIL_OK) return r;
    r = host->mid_hook(self, SYM_IDLE_EXIT, on_idle_exit, NULL, NULL);
    if (r != ANVIL_OK) return r;
    return host->subscribe(self, ANVIL_EVENT_PLUGINS_LOADED, on_plugins_loaded, NULL);
}
