#include <anvil/anvil.h>

#include "symbols.gen.h"

static const AnvilHost *g_host;
static AnvilPlugin *g_self;
static sym_speed_object_fn g_speed_next;

static void *on_speed_object(void *session) {
    return g_speed_next(session);
}

static void on_idle_exit(AnvilCpuContext *cpu, void *user) {
    (void)cpu;
    (void)user;
    g_host->log(g_self, ANVIL_LOG_INFO, "returning to the menu");
}

ANVIL_PLUGIN_EXPORT AnvilResult anvil_plugin_load(const AnvilHost *host, AnvilPlugin *self) {
    AnvilResult r;
    if (host->api_version != ANVIL_API_VERSION) return ANVIL_ERROR_UNSUPPORTED;
    g_host = host;
    g_self = self;
    host->log(self, ANVIL_LOG_INFO, "hello from C");
    /* Any failure: return it; Anvil rolls back everything staged so far. */
    r = host->hook_function(self, SYM_SPEED_OBJECT, (void *)on_speed_object, (void **)&g_speed_next, NULL);
    if (r != ANVIL_OK) return r;
    return host->mid_hook(self, SYM_IDLE_EXIT, on_idle_exit, NULL, NULL);
}
