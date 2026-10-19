// Test plugin for tests/plugin_tests.cpp: stages changes, then throws in
// load(), so Anvil must roll every one of them back.
#include <anvil/anvil.hpp>

#include "symbols.gen.hpp"

#include <stdexcept>

namespace {

anvil::HookOf<sym::add_one> g_add;
int on_add(int x) noexcept { return g_add.next(x) - 1; }

struct Throwing : anvil::Plugin {
    anvil::Change zero;

    void load(anvil::Host& host) override {
        host.hook(g_add, sym::add_one, on_add);
        zero = host.mid_hook(sym::adjust, [](anvil::Cpu& cpu) { cpu.rcx = 0; });
        throw std::runtime_error("refused on purpose");
    }
};

}  // namespace

ANVIL_PLUGIN(Throwing)
