#pragma once

#include "core/crash_history.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace anvil::manager {

// The Crashes tab's summaries, each read once from anvil/crashes/<name>.json.
// A report that could not be read whole (no file yet, or no level: the game
// was still writing it, or a fault stopped the writing) is read again after
// the next refresh of the game folder, instead of staying unreadable until
// another game folder is chosen.
class CrashSummaries {
public:
    const std::optional<CrashSummary>& get(const std::filesystem::path& crash_dir, const std::string& name);
    // A new snapshot of the folder: forget the summaries that were not whole.
    void folder_refreshed();
    void clear() { cache_.clear(); }

private:
    std::map<std::string, std::optional<CrashSummary>> cache_;
};

}  // namespace anvil::manager
