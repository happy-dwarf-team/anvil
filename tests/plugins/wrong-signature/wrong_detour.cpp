// Must NOT compile (ctest anvil_wrong_detour): the detour's signature differs
// from the one symbols.toml declares for add_one, `int (int x)`.
#include <anvil/anvil.hpp>

#include "symbols.gen.hpp"

namespace {
anvil::HookOf<sym::add_one> g_add;
int on_add(const char* text) noexcept { return text != nullptr; }
}  // namespace

void wrong_detour(const anvil::Host& host) { host.hook(g_add, sym::add_one, on_add); }
