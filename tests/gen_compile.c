/* Compiles the C header `anvil gen` writes for tests/fixtures/gen/symbols.toml. */
#include "symbols.gen.h"

_Static_assert(SYM_SPEED_OBJECT == 0 && SYM_ROOT_AND_DATE == 3, "ids follow the declaration order");
_Static_assert(SYMVAL_ROOT_AND_DATE_ROOT == 0x233FE78 && SYMVAL_ROOT_AND_DATE_DATE_OFFSET == 0x1DD0, "values");

static void* speed_object(void* session) { return session; }
static const char* write_dir(void) { return "x"; }

int c_last_symbol(void) {
    sym_speed_object_fn speed = speed_object;
    sym_physfs_get_write_dir_fn dir = write_dir;
    return speed(0) == 0 && dir()[0] == 'x' ? SYM_PHYSFS_GET_WRITE_DIR : -1;
}
