#include "crash_summaries.hpp"

namespace anvil::manager {

const std::optional<CrashSummary>& CrashSummaries::get(const std::filesystem::path& crash_dir, const std::string& name) {
    auto it = cache_.find(name);
    if (it == cache_.end()) it = cache_.emplace(name, load_crash_summary(crash_dir, name)).first;
    return it->second;
}

void CrashSummaries::folder_refreshed() {
    std::erase_if(cache_, [](const auto& entry) { return !entry.second || entry.second->level.empty(); });
}

}  // namespace anvil::manager
