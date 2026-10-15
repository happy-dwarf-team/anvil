// Structured-exception boundary around a plugin's load function. Kept in its
// own translation unit with only <windows.h>, so no C++ objects with
// destructors and no other headers interfere with __try/__except.
#include "seh_guard.hpp"

#include <windows.h>

namespace anvil {
namespace {

int record_exception(unsigned long code, unsigned long* out) {
    *out = code;
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

AnvilResult call_plugin_load(PluginLoadFn fn, const AnvilHost* host, AnvilPlugin* plugin,
                             unsigned long* code_out) {
    __try {
        return fn(host, plugin);
    } __except (record_exception(GetExceptionCode(), code_out)) {
        return ANVIL_ERROR_INTERNAL;
    }
}

}  // namespace anvil
