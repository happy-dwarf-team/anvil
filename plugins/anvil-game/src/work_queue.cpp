#include "work_queue.hpp"

namespace anvil_game {

void WorkQueue::push(AnvilGameWork work, void* context) {
    std::lock_guard lock(mutex_);
    queued_.push_back({work, context});
    pending_.store(queued_.size(), std::memory_order_release);
}

std::size_t WorkQueue::run() {
    // Most frames have nothing queued: no lock then.
    if (pending_.load(std::memory_order_acquire) == 0) return 0;
    {
        std::lock_guard lock(mutex_);
        running_.swap(queued_);
        pending_.store(0, std::memory_order_release);
    }
    for (const Item& item : running_) item.work(item.context);
    const std::size_t ran = running_.size();
    running_.clear();
    return ran;
}

}  // namespace anvil_game
