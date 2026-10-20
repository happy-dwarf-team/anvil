/* A plugin without symbols, so it loads in any process: the start-up tests
 * (tests/startup_tests.cpp) ship it in a mod and look for its log line. */
#include <anvil/anvil_plugin.h>

ANVIL_PLUGIN_EXPORT AnvilResult anvil_plugin_load(const AnvilHost *host, AnvilPlugin *self) {
    host->log(self, ANVIL_LOG_INFO, "mod probe is running");
    return ANVIL_OK;
}
