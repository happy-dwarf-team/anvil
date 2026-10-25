#pragma once

#include "benchmark.hpp"
#include "changes.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_set>

namespace anvil {

// The game's first opens of its existing files in a benchmark start
// (docs/spec/benchmark-v1.md §3, §6): the time of each successful
// CreateFileW for reading an existing file, the first time its path is
// opened in this start. The measurement is passive; nothing is opened for it.
// Every call may come from any thread.
class FirstOpens {
public:
    // One successful open that took `ticks` of the performance counter.
    void add(const wchar_t* path, int64_t ticks);
    // Counting ends (at anvil-game.ready); later opens are not counted.
    void stop() { counting_.store(false, std::memory_order_release); }
    bool counting() const { return counting_.load(std::memory_order_acquire); }
    // The files counted so far and their time in ms.
    BenchmarkOpenCost total(int64_t frequency) const;

private:
    std::atomic<bool> counting_{true};
    mutable std::mutex mutex_;
    std::unordered_set<uint64_t> seen_;  // path keys, under mutex_
    uint64_t files_ = 0;                 // under mutex_
    int64_t ticks_ = 0;                  // under mutex_
};

// The same key for every spelling Windows treats as the same path: ASCII
// case and '/' or '\' do not matter.
uint64_t open_path_key(const wchar_t* path);

// Opens the meter counts: reading (GENERIC_READ or FILE_READ_DATA) an
// existing file (OPEN_EXISTING), not a directory (FILE_FLAG_BACKUP_SEMANTICS).
bool counts_as_file_open(DWORD access, DWORD disposition, DWORD flags);

// Replaces image's import of KERNEL32.dll!CreateFileW with the meter, as a
// change of `owner` (the core's own, after every plugin's, so it is the
// innermost link and times the call itself). `opens` must live for the
// process.
ChangeStatus install_first_open_meter(ChangeRegistry& changes, OwnerId owner, HMODULE image, FirstOpens& opens);

}  // namespace anvil
