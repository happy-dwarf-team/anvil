#pragma once

#include "json.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::manager {

// The start-up benchmark's series and result (docs/spec/benchmark-v1.md
// §3, §4): which starts count, in which order the arms run, when the series
// ends, and what the player is shown. Nothing here starts a process; the
// runner feeds in what it saw of each start.

enum class Arm { Without, With };  // A and B of the ABBA order
const char* arm_name(Arm arm);     // "without" | "with"
inline Arm other_arm(Arm arm) { return arm == Arm::Without ? Arm::With : Arm::Without; }
inline size_t arm_index(Arm arm) { return arm == Arm::Without ? 0 : 1; }

// Warm and cold thresholds of the open cost per file, in ms (§3). Starting
// values from one lab machine; a cost between the two is unclear.
inline constexpr double kWarmGameOpenMs = 0.2;
inline constexpr double kColdGameOpenMs = 1.0;
inline constexpr double kWarmPluginOpenMs = 0.04;
inline constexpr double kColdPluginOpenMs = 0.2;

// Starts appended for starts that do not count (discarded or unclear); over
// it the series stops with too_many_discarded (§3).
inline constexpr int kMaxExtraStarts = 4;
// Cold starts do not use that budget, but leave their arm short too. So that
// a computer where every start is cold still ends, at most this many cold
// starts are appended; one more stops the series with too_many_discarded.
inline constexpr int kMaxColdStarts = 4;

inline constexpr std::array<int, 3> kTargets{3, 5, 10};  // valid warm starts per arm
inline constexpr int kDefaultTarget = 5;

// ---- the start record (§6), as the Manager reads it --------------------------------

struct OpenCost {
    uint64_t files = 0;
    double ms = 0;
};

struct RecordField {
    std::string name;
    std::string kind;  // int | number | enum | flag
    std::string unit;  // "" or ms | count | bytes | mb | percent
    std::string label; // "" when none; shown in the preview only
    std::vector<std::string> options;  // enum only
};

struct RecordPlugin {
    std::string id;
    std::string version;  // "" when the plugin did not load
    std::string state;    // loaded | disabled | skipped | failed | pending
    std::string by;       // disabled: arm | player
    std::string reason;   // skipped: safe_mode | conflict | dependency | verification | approval | other
    bool participant = false;
    bool done = false;
    std::string learning;  // the reason of learning(); "" when it was not called
    std::optional<OpenCost> open_cost;
    std::vector<RecordField> schema;
    Json fields = Json::object();  // values by field name, as the core wrote them
};

struct StartRecord {
    std::string write;  // "ready" | "end"
    std::string anvil;  // the Anvil version
    Arm arm = Arm::Without;
    std::string arm_plugin;
    uint32_t process_id = 0;
    uint64_t creation_filetime = 0;
    std::optional<double> time_ms;  // creation -> anvil-game.ready
    std::vector<RecordPlugin> plugins;
    std::optional<OpenCost> game_first_opens;
    std::optional<bool> first_start_since_boot;
    std::optional<bool> definitions_changed;
    std::optional<bool> input_during_start;

    const RecordPlugin* plugin(std::string_view id) const;
};

// The record anvil/data/anvil/benchmark-start.json of format 1. nullopt with
// `error` for any other document; fields the Manager does not use are
// ignored, so the core may add some.
std::optional<StartRecord> parse_start_record(std::string_view json, std::string& error);

// ---- one start ---------------------------------------------------------------------

// A crash report the start wrote (design §12), as the report carries it.
struct StartCrash {
    std::string attribution;  // in_plugin_code | in_plugin_change | plugin_on_stack | no_plugin
    std::string plugin;       // the most involved plugin; "" when none
    bool before_ready = false;
};

// What the runner saw of a start (§2).
struct StartObservation {
    std::optional<StartRecord> record;  // of this process; nullopt when it left none
    std::optional<StartCrash> crash;
    bool timed_out = false;            // no ready within 600 s: the Manager killed it
    bool killed = false;               // after ready: WM_CLOSE did not end it in 15 s
    bool environment_changed = false;  // Steam, the mod set or the plugin changed during it
};

struct StartOpenCost {
    bool from_plugin = false;  // a participant's open_cost(); otherwise the game's first opens
    std::string plugin;        // that participant
    uint64_t files = 0;
    double ms = 0;
};

// One row of the report (§7).
struct StartRow {
    int position = 0;  // 1 for the first start
    Arm arm = Arm::Without;
    std::optional<int64_t> time_ms;  // to anvil-game.ready, rounded
    bool valid = false;
    std::string discard_reason;  // "" when valid
    bool killed = false;
    std::optional<bool> first_since_boot;
    std::optional<bool> definitions_changed;
    std::optional<StartOpenCost> open_cost;
    std::string cold_class;  // warm | cold | unclear; "" for a discarded start
    std::optional<bool> input_during_start;
    std::optional<StartCrash> crash;
    std::vector<RecordPlugin> plugins;  // from the record; empty without one

    bool warm() const { return valid && cold_class == "warm"; }
    bool cold() const { return valid && cold_class == "cold"; }
    // Outside both groups: discarded or unclear.
    bool not_counted() const { return !valid || cold_class == "unclear"; }
};

// The row for a start of `arm` comparing `plugin`: valid or discarded with
// its reason (§3, "Which starts count"), and the warm, cold or unclear class
// from the open cost. `stop` gets the series' stop reason when this start
// ends the series by itself ("" otherwise; the budget is the Series' part).
StartRow classify_start(const StartObservation& seen, Arm arm, const std::string& plugin, std::string& stop);

// "warm" | "cold" | "unclear" for an open cost (§3).
std::string open_cost_class(const std::optional<StartOpenCost>& cost);

// ---- the series ----------------------------------------------------------------------

class Series {
public:
    // `target` valid warm starts per arm (3, 5 or 10), `first` the first arm.
    Series(std::string plugin, int target, Arm first);

    const std::string& plugin() const { return plugin_; }
    int target() const { return target_; }
    Arm first() const { return first_; }

    // The arm of the next start; nullopt when the series is over.
    std::optional<Arm> next() const;
    // Adds the start that ran with next()'s arm; it may end the series.
    const StartRow& add(const StartObservation& seen);
    // Ends the series before its next start: `player` (the running start has
    // finished) or `environment_changed` (found before a start).
    void stop(std::string reason);

    bool over() const { return over_; }
    bool complete() const { return over_ && stop_reason_.empty(); }
    const std::string& stop_reason() const { return stop_reason_; }  // "" while running or when complete
    const std::vector<StartRow>& rows() const { return rows_; }
    int warm(Arm arm) const { return warm_[arm_index(arm)]; }
    int extra_starts_used() const { return extra_; }
    // About how many starts the series takes, from what is known now.
    int expected_starts() const;

private:
    int planned_left(Arm arm) const;

    std::string plugin_;
    int target_;
    Arm first_;
    std::vector<Arm> planned_;  // ABBA, target starts per arm
    std::vector<StartRow> rows_;
    std::array<int, 2> warm_{};
    int extra_ = 0;
    int cold_ = 0;
    int crashes_ = 0;
    bool over_ = false;
    std::string stop_reason_;
};

// ---- the result (§4) -----------------------------------------------------------------

struct ArmSummary {
    std::vector<int64_t> times_ms;  // the warm starts, in the order they ran
    std::optional<double> median_ms;
    std::optional<int64_t> min_ms, max_ms;
};

struct Difference {
    double ms = 0;       // with the plugin minus without it
    double percent = 0;  // of the median without it, two decimals
    bool ranges_overlap = false;
};

struct ColdStart {
    int position = 0;
    Arm arm = Arm::Without;
    int64_t time_ms = 0;
    std::string kind;  // after_boot | after_definitions_update | open_cost
};

struct NotCounted {
    int position = 0;
    std::string reason;  // a discard_reason, or "unclear"
};

struct SeriesResult {
    std::array<ArmSummary, 2> warm;  // by arm_index
    std::optional<Difference> difference;  // both arms have a warm start
    std::vector<ColdStart> cold;
    std::optional<Difference> cold_difference;  // of the cold medians; both arms have a cold start
    std::vector<NotCounted> not_counted;
    std::vector<int> input_during_start;  // positions
    int extra_starts_used = 0;
    // time_to_ready (always), no_cold_without, no_cold_with, below_target, stopped.
    std::vector<std::string> not_measured;
};

SeriesResult series_result(const Series& series);

// ---- words ---------------------------------------------------------------------------

// One sentence for a stop reason (§3), for the result screen.
std::string stop_reason_text(std::string_view reason);
// A few words for a discard reason or "unclear".
std::string not_counted_text(std::string_view reason);
// A few words for a cold start's kind.
std::string cold_kind_text(std::string_view kind);

}  // namespace anvil::manager
