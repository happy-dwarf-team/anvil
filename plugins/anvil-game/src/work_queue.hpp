// Work for the game's main thread (AnvilGameService::run_on_main_thread).
#pragma once

#include <anvil/anvil_game.h>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <vector>

namespace anvil_game {

// Any thread queues; the main thread runs, after a frame of the main loop,
// the work that was queued before that run started, in order. Work queued
// while it runs (also by the work itself) waits for the next run.
class WorkQueue {
public:
    void push(AnvilGameWork work, void* context);  // may throw std::bad_alloc
    // Main thread only. Returns how many items ran.
    std::size_t run();
    std::size_t pending() const { return pending_.load(std::memory_order_acquire); }

private:
    struct Item {
        AnvilGameWork work;
        void* context;
    };
    std::mutex mutex_;
    std::vector<Item> queued_;
    std::vector<Item> running_;  // main thread only; keeps its capacity
    std::atomic<std::size_t> pending_{0};
};

}  // namespace anvil_game
