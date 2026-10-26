/* A plugin without symbols, so it loads in any process: the start-up tests
 * (tests/startup_tests.cpp) ship it in a mod and look for its log line.
 *
 * For the start-up benchmark tests it also reads two variables:
 *   ANVIL_PROBE_BENCHMARK=1   joins anvil.benchmark, sets one field of each
 *                             kind and an open cost, and calls done();
 *   ANVIL_PROBE_PUBLISH=<event> publishes that event, which Anvil allows only
 *                             when the copy's id owns it (a copy named
 *                             anvil-game publishes anvil-game.ready).
 *
 * For the call counters (profile = true):
 *   ANVIL_PROBE_HOOK=1        hooks a function of its own (outside eu4.exe)
 *                             and calls it three times once every plugin is
 *                             loaded. */
#include <anvil/anvil_benchmark.h>
#include <anvil/anvil_plugin.h>

#include <windows.h>

#include <string.h>

static void join_benchmark(const AnvilHost *host, AnvilPlugin *self) {
    static const char *const modes[] = {"cold", "warm"};
    const AnvilBenchmarkField fields[] = {
        {"probe.files", ANVIL_BENCHMARK_INT, ANVIL_BENCHMARK_UNIT_COUNT, "Files", NULL, 0},
        {"probe.time", ANVIL_BENCHMARK_NUMBER, ANVIL_BENCHMARK_UNIT_MS, NULL, NULL, 0},
        {"probe.mode", ANVIL_BENCHMARK_ENUM, ANVIL_BENCHMARK_UNIT_NONE, "Mode \"quoted\"", modes, 2},
        {"probe.ok", ANVIL_BENCHMARK_FLAG, ANVIL_BENCHMARK_UNIT_NONE, NULL, NULL, 0},
        {"unset", ANVIL_BENCHMARK_INT, ANVIL_BENCHMARK_UNIT_NONE, NULL, NULL, 0},
    };
    const AnvilBenchmarkService *bench =
        (const AnvilBenchmarkService *)host->get_service(self, ANVIL_BENCHMARK_SERVICE, ANVIL_BENCHMARK_SERVICE_VERSION);
    if (bench == NULL || !ANVIL_BENCHMARK_HAS(bench, open_cost)) {
        host->log(self, ANVIL_LOG_WARNING, "no anvil.benchmark");
        return;
    }
    if (bench->declare(self, fields, 5) != ANVIL_OK) return;
    bench->set_int(self, "probe.files", 42);
    bench->set_number(self, "probe.time", 1.5);
    bench->set_enum(self, "probe.mode", "warm");
    bench->set_flag(self, "probe.ok", 1);
    bench->open_cost(self, 42, 2.25);
    bench->done(self);
    host->log(self, ANVIL_LOG_INFO, bench->active() ? "benchmark fields set (active)" : "benchmark fields set (inactive)");
}

typedef int (*TargetFn)(int);
static const AnvilHost *g_host;
static AnvilPlugin *g_self;
static TargetFn g_target;      /* lea eax, [rcx+1]; nops; ret, in memory of its own */
static void *g_next;           /* the rest of the chain, written by Anvil */

static int target_detour(int x) { return ((TargetFn)g_next)(x); }

static void call_target(const char *event, const void *payload, void *context) {
    int sum = 0;
    (void)event;
    (void)payload;
    (void)context;
    for (int i = 0; i < 3; ++i) sum += g_target(i);
    g_host->log(g_self, sum == 6 ? ANVIL_LOG_INFO : ANVIL_LOG_WARNING, "probe target called 3 times");
}

static AnvilResult hook_target(const AnvilHost *host, AnvilPlugin *self) {
    static const unsigned char add_one[] = {0x8D, 0x41, 0x01, 0x90, 0x90, 0x90, 0x90,
                                            0x90, 0x90, 0x90, 0x90, 0x90, 0xC3};
    unsigned char *code = (unsigned char *)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    AnvilResult r;
    if (code == NULL) return ANVIL_ERROR_INTERNAL;
    memcpy(code, add_one, sizeof(add_one));
    g_target = (TargetFn)(void *)code;
    r = host->hook_function_at(self, code, add_one, 5, (void *)target_detour, (void **)&g_next, NULL);
    if (r != ANVIL_OK) return r;
    return host->subscribe(self, ANVIL_EVENT_PLUGINS_LOADED, call_target, NULL);
}

ANVIL_PLUGIN_EXPORT AnvilResult anvil_plugin_load(const AnvilHost *host, AnvilPlugin *self) {
    char value[64];
    g_host = host;
    g_self = self;
    host->log(self, ANVIL_LOG_INFO, "mod probe is running");
    if (GetEnvironmentVariableA("ANVIL_PROBE_HOOK", value, sizeof(value)) == 1 && value[0] == '1') {
        const AnvilResult r = hook_target(host, self);
        if (r != ANVIL_OK) return r;
    }
    if (GetEnvironmentVariableA("ANVIL_PROBE_BENCHMARK", value, sizeof(value)) == 1 && value[0] == '1') {
        join_benchmark(host, self);
    }
    const DWORD n = GetEnvironmentVariableA("ANVIL_PROBE_PUBLISH", value, sizeof(value));
    if (n > 0 && n < sizeof(value)) host->publish(self, value, NULL);
    return ANVIL_OK;
}
