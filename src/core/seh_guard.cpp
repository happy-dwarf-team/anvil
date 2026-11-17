// Structured-exception boundary around a plugin's load function. Kept in its
// own translation unit with only <windows.h>, so no C++ objects with
// destructors and no other headers interfere with __try/__except.
#include "seh_guard.hpp"

#include <windows.h>

#include <malloc.h>

namespace anvil {
namespace {

// How many of the guards below this thread is inside.
thread_local int t_guards = 0;

int record_exception(unsigned long code, unsigned long* out) {
    if (out != nullptr) *out = code;
    return EXCEPTION_EXECUTE_HANDLER;
}

// After a caught stack overflow the thread has no guard page: without it the
// next overflow ends the process with no exception at all.
void after_exception(unsigned long code) {
    if (code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
}

}  // namespace

bool inside_guard() { return t_guards > 0; }

AnvilResult call_plugin_load(PluginLoadFn fn, const AnvilHost* host, AnvilPlugin* plugin,
                             unsigned long* code_out) {
    unsigned long code = 0;
    ++t_guards;
    __try {
        const AnvilResult r = fn(host, plugin);
        --t_guards;
        return r;
    } __except (record_exception(GetExceptionCode(), &code)) {
        --t_guards;
        if (code_out != nullptr) *code_out = code;
        after_exception(code);
        return ANVIL_ERROR_INTERNAL;
    }
}

bool call_guarded(void (*fn)(void*), void* argument, unsigned long* code_out) {
    unsigned long code = 0;
    ++t_guards;
    __try {
        fn(argument);
        --t_guards;
        return true;
    } __except (record_exception(GetExceptionCode(), &code)) {
        --t_guards;
        if (code_out != nullptr) *code_out = code;
        after_exception(code);
        return false;
    }
}

}  // namespace anvil
