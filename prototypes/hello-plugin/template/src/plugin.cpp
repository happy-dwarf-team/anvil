#include <anvil/anvil.hpp>

#include "symbols.gen.hpp"

#include <atomic>

namespace {

anvil::HookOf<sym::speed_object> g_speed;  // the hook and the rest of its chain
std::atomic<bool> g_first{true};

void* on_speed_object(void* session) {     // must match the signature in symbols.toml
    void* speed = g_speed.next(session);
    if (g_first.exchange(false)) anvil::Host{}.info("first speed object: {}", speed);
    return speed;
}

struct Hello : anvil::Plugin {
    anvil::Change menu_return;

    void load(anvil::Host& host) override {
        host.info("hello on {}", host.game().build_id);
        host.hook(g_speed, sym::speed_object, on_speed_object);
        menu_return = host.mid_hook(sym::idle_exit, [host](anvil::Cpu& cpu) mutable {
            host.info("returning to the menu (rcx={:#x})", cpu.rcx);
        });
    }
};

}  // namespace

ANVIL_PLUGIN(Hello)
