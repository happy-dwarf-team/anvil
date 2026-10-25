// PROTOTYPE, throwaway (anvil#134): does a benchmark start quit itself cleanly?
// After anvil-game.ready it waits BENCH_QUIT_DELAY_MS (default 3000, so Bellows
// saves its profiles first), then pushes SDL_QUIT on the main thread, which is
// what the game's own Quit does (research for anvil#128). BENCH_QUIT_MODE=off
// keeps it passive, so the same install can test the Manager's WM_CLOSE.
#include <anvil/anvil.hpp>
#include <anvil/anvil_game.h>

#include "symbols.gen.hpp"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

constexpr std::uint32_t kSdlQuit = 0x100;  // SDL_QUIT

struct alignas(8) SdlEvent {
    unsigned char bytes[56];
};

using PushEventFn = int (*)(SdlEvent* event);

const AnvilGameService* g_game = nullptr;
std::uintptr_t g_image_base = 0;

std::string Env(const char* name, const char* fallback) {
    char buffer[64] = {};
    const DWORD n = GetEnvironmentVariableA(name, buffer, sizeof buffer);
    return n > 0 && n < sizeof buffer ? std::string(buffer, n) : std::string(fallback);
}

// UTC FILETIME (100 ns since 1601), the clock of GetProcessTimes.
std::uint64_t NowFileTime() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

void PushQuit(void*) {
    anvil::Host host;
    const std::uintptr_t expected = g_image_base + sym::sdl_dynapi_init.push_event;
    std::uintptr_t slot = 0;
    std::memcpy(&slot, reinterpret_cast<const void*>(g_image_base + sym::sdl_dynapi_init.push_event_slot), sizeof slot);
    if (slot != expected) {
        host.warn("SDL dynapi slot points elsewhere; not quitting");
        return;
    }
    SdlEvent e{};
    std::memcpy(e.bytes, &kSdlQuit, sizeof kSdlQuit);
    const std::uint64_t at = NowFileTime();
    const int result = reinterpret_cast<PushEventFn>(expected)(&e);
    host.info("SDL_QUIT pushed on thread {} (frame {}), result {}, utc_filetime {}", GetCurrentThreadId(),
              g_game->frame_number(), result, at);
}

struct BenchQuit : anvil::Plugin {
    void load(anvil::Host& host) override {
        g_game = host.service<AnvilGameService>(ANVIL_GAME_SERVICE, ANVIL_GAME_SERVICE_VERSION);
        if (g_game == nullptr) throw anvil::Error(ANVIL_ERROR_UNSUPPORTED, "anvil-game service missing");
        g_image_base = host.game().image_base;
        const std::string mode = Env("BENCH_QUIT_MODE", "sdl");
        const int delay_ms = std::atoi(Env("BENCH_QUIT_DELAY_MS", "3000").c_str());
        host.info("PROTOTYPE bench-quit: mode {}, delay {} ms after ready", mode, delay_ms);
        host.subscribe(ANVIL_GAME_EVENT_READY, [host, mode, delay_ms] {
            host.info("ready seen, utc_filetime {}", NowFileTime());
            if (mode != "sdl") return;
            std::thread([host, delay_ms] {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                host.info("delay over, queueing SDL_QUIT for the main thread");
                g_game->run_on_main_thread(&PushQuit, nullptr);
            }).detach();
        });
    }
};

}  // namespace

ANVIL_PLUGIN(BenchQuit)
