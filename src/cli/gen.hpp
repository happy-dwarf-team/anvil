#pragma once

#include "core/settings.hpp"
#include "core/symbols.hpp"

#include <cstdint>
#include <string>

namespace anvil::cli {

// The generated headers for one symbols.toml or settings.toml (design §16). A
// header is empty when it was not asked for or when `error` is set.
struct GeneratedHeaders {
    std::string c;    // symbols.gen.h / settings.gen.h
    std::string cpp;  // symbols.gen.hpp / settings.gen.hpp
    std::string error;
    uint32_t line = 0;  // the line of the .toml the error is about; 0 = the whole file
};

// symbols.gen.h (C): SYM_<NAME> ids, sym_<name>_fn function pointer types
// and SYMVAL_<SYMBOL>_<VALUE> constants.
// symbols.gen.hpp (C++): namespace sym with an anvil::Function, Site, Slot or
// Data constant per symbol (include/anvil/anvil_symbols.hpp), values as
// static constexpr members, and an anvil::Data<T> for every typed value.
// `source` names symbols.toml in the header comment.
GeneratedHeaders generate_headers(const SymbolFile& file, const std::string& source, bool c, bool cpp);

// The headers for one settings.toml (design §16): `c` is settings.gen.h,
// `cpp` settings.gen.hpp, `line` a line of settings.toml.
// settings.gen.h (C): CFG_<NAME> ids for host->setting_*(self, id),
// CFG_<NAME>_<OPTION> indices of enum options, and ANVIL_KEY_* for key codes.
// settings.gen.hpp (C++): namespace cfg with an anvil::Setting<T> constant per
// setting (cfg::<name>.get(), include/anvil/anvil.hpp) whose type
// cfg::Types::<name> holds setting_name, restart, default_value, the limits,
// and for an enum the options of its enum class cfg::Types::<name>_value.
GeneratedHeaders generate_settings_headers(const SettingsFile& file, const std::string& source, bool c, bool cpp);

}  // namespace anvil::cli
