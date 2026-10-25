/* Compiles the C headers `anvil gen` writes for tests/fixtures/gen/symbols.toml
   and settings.toml. */
#include "settings.gen.h"
#include "symbols.gen.h"

#include <anvil/anvil_benchmark.h>

_Static_assert(SYM_SPEED_OBJECT == 0 && SYM_ROOT_AND_DATE == 3, "ids follow the declaration order");
_Static_assert(SYMVAL_ROOT_AND_DATE_ROOT == 0x233FE78 && SYMVAL_ROOT_AND_DATE_DATE_OFFSET == 0x1DD0, "values");
_Static_assert(CFG_ENABLED == 0 && CFG_MODE == 3 && CFG_PANEL_KEY == 5, "setting ids follow the declaration order");
_Static_assert(CFG_MODE_OFF == 0 && CFG_MODE_VERIFY == 1 && CFG_MODE_ON == 2, "enum options");
_Static_assert(ANVIL_KEY_CTRL == 0x200 && ANVIL_KEY_VK(0x37B) == 0x7B, "key codes");
_Static_assert(sizeof(AnvilBenchmarkField) == 40 && ANVIL_BENCHMARK_FLAG == 4, "anvil.benchmark v1 layout");

static void* speed_object(void* session) { return session; }
static const char* write_dir(void) { return "x"; }

int c_last_symbol(void) {
    sym_speed_object_fn speed = speed_object;
    sym_physfs_get_write_dir_fn dir = write_dir;
    return speed(0) == 0 && dir()[0] == 'x' ? SYM_PHYSFS_GET_WRITE_DIR : -1;
}

int c_last_setting(void) { return CFG_PANEL_KEY; }
