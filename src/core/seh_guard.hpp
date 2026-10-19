#pragma once

#include <anvil/anvil_plugin.h>

namespace anvil {

using PluginLoadFn = AnvilResult (*)(const AnvilHost*, AnvilPlugin*);

// Calls fn; a structured exception (access violation ...) is caught, its code
// stored in *code_out, and ANVIL_ERROR_INTERNAL returned.
AnvilResult call_plugin_load(PluginLoadFn fn, const AnvilHost* host, AnvilPlugin* plugin,
                             unsigned long* code_out);

// Calls fn(argument); a structured exception is caught, its code stored in
// *code_out, and false returned. For the crash writer, which reads memory of a
// crashed thread.
bool call_guarded(void (*fn)(void*), void* argument, unsigned long* code_out);

}  // namespace anvil
