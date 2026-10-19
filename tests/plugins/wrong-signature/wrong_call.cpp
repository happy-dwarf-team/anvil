// Must NOT compile (ctest anvil_wrong_call): twice is `int (int x)`.
#include <anvil/anvil.hpp>

#include "symbols.gen.hpp"

int wrong_call(const anvil::Host& host) { return host.call(sym::twice, "twenty-one"); }
