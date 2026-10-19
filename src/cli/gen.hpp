#pragma once

#include "core/symbols.hpp"

#include <cstdint>
#include <string>

namespace anvil::cli {

// The generated headers for one symbols.toml (design §16). A header is empty
// when it was not asked for or when `error` is set.
struct GeneratedHeaders {
    std::string c;    // symbols.gen.h
    std::string cpp;  // symbols.gen.hpp
    std::string error;
    uint32_t line = 0;  // the line of symbols.toml the error is about; 0 = the whole file
};

// symbols.gen.h (C): SYM_<NAME> ids, sym_<name>_fn function pointer types
// and SYMVAL_<SYMBOL>_<VALUE> constants.
// symbols.gen.hpp (C++): namespace sym with an anvil::Function, Site, Slot or
// Data constant per symbol (include/anvil/anvil_symbols.hpp), values as
// static constexpr members, and an anvil::Data<T> for every typed value.
// `source` names symbols.toml in the header comment.
GeneratedHeaders generate_headers(const SymbolFile& file, const std::string& source, bool c, bool cpp);

}  // namespace anvil::cli
