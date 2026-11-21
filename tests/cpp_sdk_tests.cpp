// The C++ SDK's hook and swap objects (include/anvil/anvil.hpp) against a fake
// host table: what a failed request leaves in a reused object, and that one
// request at a time holds an object.
#include <anvil/anvil.hpp>

#include "test_support.hpp"

#include <cstdio>
#include <functional>
#include <string>

namespace {

using Fn = int (*)(int);

int game_fn(int x) { return x + 1; }
int other_fn(int x) { return x + 2; }
int detour(int x) { return x; }

// What the fake host answers, and what it saw.
struct Fake {
    AnvilResult answer = ANVIL_OK;
    void* next_value = reinterpret_cast<void*>(&game_fn);
    void* previous_value = reinterpret_cast<void*>(&game_fn);
    int handles = 0;
    int removed = 0;
    AnvilResult commit = ANVIL_OK;
    int aborts = 0;
    bool old_core = true;          // Anvil 1.25.0 and older: a failed request writes NULL into next
    std::function<void()> during;  // runs inside the request
} g_fake;

AnvilChange* handle_of(int n) { return reinterpret_cast<AnvilChange*>(static_cast<uintptr_t>(0x1000 + n)); }

// As the core's add_link: `next` gets the stub before the enable; a failed
// enable puts back what the cell held (Anvil after 1.25.0, anvil#534) or
// writes NULL over it (1.25.0 and older).
AnvilResult fake_hook_at(AnvilPlugin*, void*, const void*, size_t, void*, void** next, AnvilChange** out) {
    void* const before = *next;
    *next = g_fake.next_value;
    if (g_fake.during) g_fake.during();
    if (g_fake.answer != ANVIL_OK) {
        *next = g_fake.old_core ? nullptr : before;
        if (out != nullptr) *out = nullptr;
        return g_fake.answer;
    }
    if (out != nullptr) *out = handle_of(++g_fake.handles);
    return ANVIL_OK;
}

// As the core after anvil#495: a failed swap leaves the cell as it was.
AnvilResult fake_swap(AnvilPlugin*, AnvilSymbol, void*, void** previous, AnvilChange** out) {
    void* const before = *previous;
    *previous = g_fake.previous_value;
    if (g_fake.during) g_fake.during();
    if (g_fake.answer != ANVIL_OK) {
        *previous = before;
        if (out != nullptr) *out = nullptr;
        return g_fake.answer;
    }
    if (out != nullptr) *out = handle_of(++g_fake.handles);
    return ANVIL_OK;
}

AnvilResult fake_remove(AnvilPlugin*, AnvilChange*) {
    ++g_fake.removed;
    return ANVIL_OK;
}

void fake_log(AnvilPlugin*, AnvilLogLevel, const char*) {}
AnvilResult fake_begin(AnvilPlugin*) { return ANVIL_OK; }
AnvilResult fake_commit(AnvilPlugin*) { return g_fake.commit; }
void fake_abort(AnvilPlugin*) { ++g_fake.aborts; }

AnvilGameInfo g_game{};

AnvilHost make_host() {
    AnvilHost h{};
    h.game = &g_game;
    h.size = sizeof(AnvilHost);
    h.api_version = ANVIL_API_VERSION;
    h.log = &fake_log;
    h.hook_function_at = &fake_hook_at;
    h.swap_pointer = &fake_swap;
    h.remove = &fake_remove;
    h.begin_group = &fake_begin;
    h.commit_group = &fake_commit;
    h.abort_group = &fake_abort;
    return h;
}

const anvil::Slot kSlot{0};

}  // namespace

AnvilHost make_new_host() {
    AnvilHost h = make_host();
    h.anvil_version = "1.26.0";
    return h;
}

int main() {
    static const AnvilHost host = make_host();  // no version: treated as 1.25.0 or older
    static const AnvilHost new_host = make_new_host();
    anvil::detail::g_host.store(&host);
    anvil::detail::g_self.store(reinterpret_cast<AnvilPlugin*>(&g_fake));
    const anvil::Host h;
    int fakes[1] = {};
    void* const address = &fakes[0];

    run("hook: a failed request on a reused hook leaves next as the removed change had it", [&] {
        for (const bool old_core : {true, false}) {
            g_fake = {};
            g_fake.old_core = old_core;
            anvil::detail::g_host.store(old_core ? &host : &new_host);
            anvil::FunctionHook<int(int)> hook;
            h.hook_at(hook, address, &detour);
            CHECK(hook.active() && hook.next_pointer() == &game_fn && hook.next(1) == 2);
            hook.remove();
            CHECK(!hook.active() && g_fake.removed == 1);
            // A thread still inside the removed detour calls next(): it must stay the old chain.
            g_fake.answer = ANVIL_ERROR_INTERNAL;
            g_fake.next_value = reinterpret_cast<void*>(&other_fn);
            bool threw = false;
            try {
                h.hook_at(hook, address, &detour);
            } catch (const anvil::Error& e) {
                threw = e.result() == ANVIL_ERROR_INTERNAL;
            }
            CHECK(threw);
            CHECK(hook.next_pointer() == &game_fn && hook.next(1) == 2);
            CHECK(!hook.active());
            // Free again: a later request succeeds and takes its own next.
            g_fake.answer = ANVIL_OK;
            h.hook_at(hook, address, &detour);
            CHECK(hook.active() && hook.next_pointer() == &other_fn);
            hook.remove();
        }
        anvil::detail::g_host.store(&host);
    });

    run("hook: a reused hook has its new next as soon as the change is enabled (Anvil after 1.25.0)", [&] {
        g_fake = {};
        g_fake.old_core = false;
        anvil::detail::g_host.store(&new_host);
        anvil::FunctionHook<int(int)> hook;
        h.hook_at(hook, address, &detour);
        hook.remove();
        // A game thread enters the detour while the request on the reused hook
        // runs: it reads the new change's next, not the removed one's.
        g_fake.next_value = reinterpret_cast<void*>(&other_fn);
        Fn seen = nullptr;
        g_fake.during = [&] { seen = hook.next_pointer(); };
        h.hook_at(hook, address, &detour);
        CHECK(seen == &other_fn && hook.next_pointer() == &other_fn && hook.active());
        hook.remove();
        anvil::detail::g_host.store(&host);
    });

    run("restores_next: Anvil after 1.25.0", [&] {
        AnvilHost v = make_host();
        for (const char* old : {"", "1", "1.25", "1.25.0", "1.24.9", "1.9.99", "0.99.0", "x.y.z", "1.25.x"}) {
            v.anvil_version = old;
            CHECK(!anvil::detail::restores_next(v));
        }
        for (const char* later : {"1.25.1", "1.26.0", "1.26.0-beta.1", "2.0.0", "1.100.0"}) {
            v.anvil_version = later;
            CHECK(anvil::detail::restores_next(v));
        }
        v.anvil_version = nullptr;
        CHECK(!anvil::detail::restores_next(v));
    });

    run("hook: a detour the game enters as soon as a first change is enabled already has next", [&] {
        g_fake = {};
        anvil::FunctionHook<int(int)> hook;
        // Inside the request, after Anvil wrote next: when the core enables the
        // change, a game thread may call the detour before the request returns.
        Fn seen = nullptr;
        g_fake.during = [&] { seen = hook.next_pointer(); };
        h.hook_at(hook, address, &detour);
        CHECK(seen == &game_fn && hook.next_pointer() == &game_fn && hook.next(1) == 2);
        hook.remove();
        // A first request that fails leaves the hook empty.
        anvil::FunctionHook<int(int)> fresh;
        g_fake.during = {};
        g_fake.answer = ANVIL_ERROR_INTERNAL;
        bool threw = false;
        try {
            h.hook_at(fresh, address, &detour);
        } catch (const anvil::Error& e) {
            threw = e.result() == ANVIL_ERROR_INTERNAL;
        }
        CHECK(threw && fresh.next_pointer() == nullptr && !fresh.active());
    });

    run("hook: a second request while one runs is refused and leaves the first alone", [&] {
        g_fake = {};
        anvil::FunctionHook<int(int)> hook;
        std::string refused;
        g_fake.during = [&] {
            g_fake.during = nullptr;
            try {
                h.hook_at(hook, address, &detour);
            } catch (const anvil::Error& e) {
                refused = e.what();
            }
            CHECK(hook.active());  // held by the running request
        };
        h.hook_at(hook, address, &detour);
        CHECK(refused == "hook_function_at: the hook object is in use; remove() it first");
        CHECK(hook.active() && g_fake.handles == 1 && g_fake.removed == 0);
        // remove() while a request holds the object does nothing; afterwards it removes.
        g_fake.during = [&] {
            g_fake.during = nullptr;
            hook.remove();
        };
        hook.remove();
        CHECK(g_fake.removed == 1 && !hook.active());
    });

    run("hook: a request refused before it reaches Anvil frees the object", [&] {
        g_fake = {};
        anvil::FunctionHook<int(int)> hook;
        anvil::detail::g_host.store(nullptr);
        bool threw = false;
        try {
            h.hook_at(hook, address, &detour);
        } catch (const anvil::Error& e) {
            threw = e.result() == ANVIL_ERROR_WRONG_PHASE;
        }
        anvil::detail::g_host.store(&host);
        CHECK(threw && !hook.active());
        h.hook_at(hook, address, &detour);
        CHECK(hook.active());
    });

    run("swap: one request at a time, and a failed one frees the object", [&] {
        g_fake = {};
        anvil::PointerSwap<int(int)> swap;
        h.swap(swap, kSlot, &detour);
        CHECK(swap.active() && swap.previous() == &game_fn);
        swap.remove();
        CHECK(!swap.active() && g_fake.removed == 1);
        g_fake.answer = ANVIL_ERROR_INTERNAL;
        g_fake.previous_value = reinterpret_cast<void*>(&other_fn);
        bool threw = false;
        try {
            h.swap(swap, kSlot, &detour);
        } catch (const anvil::Error&) {
            threw = true;
        }
        CHECK(threw && !swap.active() && swap.previous() == &game_fn);
        g_fake.answer = ANVIL_OK;
        std::string refused;
        g_fake.during = [&] {
            g_fake.during = nullptr;
            try {
                h.swap(swap, kSlot, &detour);
            } catch (const anvil::Error& e) {
                refused = e.what();
            }
        };
        h.swap(swap, kSlot, &detour);
        CHECK(refused == "swap_pointer: the hook object is in use; remove() it first");
        CHECK(swap.active() && swap.previous() == &other_fn && g_fake.handles == 2);
    });

    run("hook: destroying the object removes its change", [&] {
        g_fake = {};
        {
            anvil::FunctionHook<int(int)> hook;
            h.hook_at(hook, address, &detour);
        }
        CHECK(g_fake.removed == 1);
    });

    run("group: a group whose changes are dropped frees the hooks it filled", [&] {
        g_fake = {};
        anvil::FunctionHook<int(int)> kept;  // filled before the group: not the group's
        h.hook_at(kept, address, &detour);
        anvil::FunctionHook<int(int)> hook;
        anvil::PointerSwap<int(int)> swap;
        // The body throws: Anvil drops what it requested.
        bool threw = false;
        try {
            h.group([&] {
                h.hook_at(hook, address, &detour);
                h.swap(swap, kSlot, &detour);
                throw anvil::Error(ANVIL_ERROR_INVALID_ARGUMENT, "the body failed");
            });
        } catch (const anvil::Error&) {
            threw = true;
        }
        CHECK(threw && g_fake.aborts == 1);
        CHECK(!hook.active() && !swap.active() && kept.active());
        // Anvil refuses the commit (enabling failed: none enabled).
        g_fake.commit = ANVIL_ERROR_CONFLICT;
        threw = false;
        try {
            h.group([&] { h.hook_at(hook, address, &detour); });
        } catch (const anvil::Error& e) {
            threw = e.result() == ANVIL_ERROR_CONFLICT;
        }
        CHECK(threw && !hook.active());
        // A hook destroyed in the body leaves nothing for the group to touch.
        g_fake.commit = ANVIL_ERROR_CONFLICT;
        try {
            h.group([&] {
                anvil::FunctionHook<int(int)> local;
                h.hook_at(local, address, &detour);
            });
        } catch (const anvil::Error&) {
        }
        // Committed: the hook keeps its change, and a later failed group leaves it.
        g_fake.commit = ANVIL_OK;
        h.group([&] { h.hook_at(hook, address, &detour); });
        CHECK(hook.active());
        try {
            h.group([] { throw anvil::Error(ANVIL_ERROR_INVALID_ARGUMENT, "nothing requested"); });
        } catch (const anvil::Error&) {
        }
        CHECK(hook.active() && kept.active());
        const int removed = g_fake.removed;
        hook.remove();
        kept.remove();
        CHECK(g_fake.removed == removed + 2);
    });

    run("game: the hash reads empty until the host sets it, and from an older host (S4, #545)", [&] {
        g_game = {};
        g_game.size = sizeof(AnvilGameInfo);
        CHECK(h.game_sha256().empty());
        g_game.sha256 = "9AD3EFE1";
        CHECK(h.game_sha256() == "9AD3EFE1");
        g_game.size = offsetof(AnvilGameInfo, sha256);
        CHECK(h.game_sha256().empty());
    });

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
