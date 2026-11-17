#pragma once

// The boundary of the core's C API: the functions of AnvilHost, anvil.control
// and anvil.benchmark, which plugins call through C function pointers. Under
// /EHsc a caller assumes such a call does not throw, and a plugin may call
// from a hook's detour or a game callback, where nothing above would catch:
// an exception that left one would end the process (anvil#457). c_api<F>
// wraps F so that none leaves it: std::bad_alloc answers
// ANVIL_ERROR_OUT_OF_MEMORY, any other exception ANVIL_ERROR_INTERNAL, and a
// function of another return type its "nothing" (0, false, an empty string,
// a null pointer). Structured exceptions are not C++ exceptions and pass on
// to the crash handler as before.

#include "anvil/anvil_plugin.h"

#include <new>
#include <type_traits>

namespace anvil {

// Logs the first exception a C API function caught; never throws.
void note_api_exception(bool out_of_memory) noexcept;

template <typename R>
R api_failure(bool out_of_memory) noexcept {
    if constexpr (std::is_void_v<R>) {
        return;
    } else if constexpr (std::is_same_v<R, AnvilResult>) {
        return out_of_memory ? ANVIL_ERROR_OUT_OF_MEMORY : ANVIL_ERROR_INTERNAL;
    } else if constexpr (std::is_same_v<R, const char*>) {
        return "";
    } else if constexpr (std::is_same_v<R, const wchar_t*>) {
        return L"";
    } else {
        return R{};
    }
}

template <auto F>
struct CApi;

template <typename R, typename... A, R (*F)(A...)>
struct CApi<F> {
    static R call(A... a) noexcept {
        bool out_of_memory = false;
        try {
            return F(a...);
        } catch (const std::bad_alloc&) {
            out_of_memory = true;
        } catch (...) {
        }
        note_api_exception(out_of_memory);
        return api_failure<R>(out_of_memory);
    }
};

template <auto F>
inline constexpr auto c_api = &CApi<F>::call;

}  // namespace anvil
