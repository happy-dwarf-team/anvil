// Symbol types for the C++ header that `anvil gen` writes (symbols.gen.hpp,
// design §16). Each declared symbol becomes a constant in namespace `sym`
// whose type derives from one of these and carries the symbol's name
// (sym::Types::<name>), so the C++ wrapper (anvil.hpp) can only hook a
// function with a matching detour, swap a pointer slot, and so on, and its
// compile errors name the symbol.
//
// Header-only, C++17 or newer; the wrapper needs C++20.
#pragma once

#include <anvil/anvil_plugin.h>

#include <cstdint>

namespace anvil {

// A symbol's id: its declaration order in symbols.toml.
using SymbolId = AnvilSymbol;

// A place in code: a mid hook, a block replacement, a call redirect or a byte patch.
struct Site {
    SymbolId id;
};

// A pointer slot (`points_to`): a pointer swap.
struct Slot {
    SymbolId id;
};

// Data of type T (`type`): read or written through its verified address.
// `value` is null for a data symbol; for a `rip_relative` value with a `type`
// it names that value of symbol `id`, whose result is the data's RVA.
template <class T> struct Data {
    using Type = T;
    SymbolId id;
    const char* value = nullptr;
};

// A function with the signature declared in symbols.toml (`signature`).
template <class F> struct Function;
template <class R, class... A> struct Function<R(A...)> {
    using Signature = R(A...);
    using Pointer = R (*)(A...);
    SymbolId id;
};

}  // namespace anvil
