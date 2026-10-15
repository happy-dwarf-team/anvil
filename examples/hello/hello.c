/* Minimal Anvil plugin: no game patches, only the host API. */
#include <anvil/anvil_plugin.h>

#include <stdio.h>

typedef struct HelloMath {
    uint32_t size;
    int (*add)(int a, int b);
} HelloMath;

static const AnvilHost *g_host;
static AnvilPlugin *g_self;

static int hello_add(int a, int b) { return a + b; }

static const HelloMath g_math = {sizeof(HelloMath), hello_add};

static void on_game_ready(const char *event, const void *payload, void *context) {
    (void)event;
    (void)payload;
    (void)context;
    g_host->log(g_self, ANVIL_LOG_INFO, "the game reported that it is ready");
}

ANVIL_PLUGIN_EXPORT AnvilResult anvil_plugin_load(const AnvilHost *host, AnvilPlugin *self) {
    char message[256];
    g_host = host;
    g_self = self;
    snprintf(message, sizeof(message), "hello from Anvil %s; game build %s", host->anvil_version,
             host->game->build_id ? host->game->build_id : "unknown");
    host->log(self, ANVIL_LOG_INFO, message);
    /* Run-time dependency check; must come before the plugin's first patch. */
    if (ANVIL_HOST_HAS(host, expect_bytes) &&
        host->expect_bytes(self, 0, "MZ", NULL, 2) != ANVIL_OK) {
        return ANVIL_ERROR_UNSUPPORTED;
    }
    host->register_service(self, "hello.math", 1, &g_math);
    if (host->game->ready_signal_known) {
        host->subscribe(self, ANVIL_EVENT_GAME_READY, on_game_ready, NULL);
    }
    return ANVIL_OK;
}
