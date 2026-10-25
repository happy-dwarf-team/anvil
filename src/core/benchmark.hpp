#pragma once

#include <anvil/anvil_benchmark.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

struct AnvilPlugin;

namespace anvil {

struct BenchmarkField {
    std::string name;
    AnvilBenchmarkKind kind = ANVIL_BENCHMARK_INT;
    AnvilBenchmarkUnit unit = ANVIL_BENCHMARK_UNIT_NONE;
    std::string label;                 // "" when none
    std::vector<std::string> options;  // ENUM only
};

// An enum field's value: the index of the option in BenchmarkField::options.
struct BenchmarkOption {
    uint32_t index = 0;
    bool operator==(const BenchmarkOption&) const = default;
};

// A field's value; monostate while it was never set.
using BenchmarkValue = std::variant<std::monostate, int64_t, double, BenchmarkOption, bool>;

struct BenchmarkOpenCost {
    uint64_t files = 0;
    double ms = 0;
};

// A plugin that declared its fields (docs/spec/benchmark-v1.md §5).
struct BenchmarkParticipant {
    const AnvilPlugin* plugin = nullptr;
    std::string id;
    std::vector<BenchmarkField> fields;
    std::vector<BenchmarkValue> values;  // by field
    bool done = false;
    std::string learning;  // the reason of learning(); "" when it was not called
    std::optional<BenchmarkOpenCost> open_cost;
};

// The core's anvil.benchmark v1 (include/anvil/anvil_benchmark.h). One per
// process; service() is what the core registers. Every call may come from any
// thread. Outside a benchmark start the calls check their arguments the same
// way; the participants are kept, but nothing reads them.
class Benchmark {
public:
    // Warnings about refused calls, in the plugin's log.
    using LogSink = std::function<void(const AnvilPlugin& plugin, const std::string& message)>;

    Benchmark(bool active, LogSink log);
    ~Benchmark();
    Benchmark(const Benchmark&) = delete;
    Benchmark& operator=(const Benchmark&) = delete;

    // The table of function pointers. They reach this object through a
    // process-wide pointer that the constructor sets (and the destructor
    // clears), so only one Benchmark exists at a time.
    const AnvilBenchmarkService* service() const;

    bool active() const { return active_; }
    AnvilResult declare(AnvilPlugin* self, const AnvilBenchmarkField* fields, uint32_t count);
    AnvilResult set_int(AnvilPlugin* self, const char* field, int64_t value);
    AnvilResult set_number(AnvilPlugin* self, const char* field, double value);
    AnvilResult set_enum(AnvilPlugin* self, const char* field, const char* option);
    AnvilResult set_flag(AnvilPlugin* self, const char* field, int value);
    AnvilResult done(AnvilPlugin* self);
    AnvilResult learning(AnvilPlugin* self, const char* reason);
    AnvilResult open_cost(AnvilPlugin* self, uint64_t files, double ms);

    // A plugin whose anvil_plugin_load failed is no longer a participant.
    void forget(const AnvilPlugin* plugin);

    // A copy of the participants, in the order they joined.
    std::vector<BenchmarkParticipant> participants() const;

private:
    // Why a set_* call is refused, or ANVIL_OK with `index` of the field.
    AnvilResult find_field(const BenchmarkParticipant* p, const char* field, AnvilBenchmarkKind kind,
                           size_t& index) const;
    template <class Assign>
    AnvilResult assign(AnvilPlugin* self, const char* call, const char* field, AnvilBenchmarkKind kind,
                       Assign&& value);
    BenchmarkParticipant* find(const AnvilPlugin* self);  // under mutex_
    // The warning for a refused call: always for declare, once per plugin,
    // call, field and result for the others (they may run every frame).
    AnvilResult refuse(const AnvilPlugin& self, const std::string& call, const std::string& what,
                       const std::string& why, AnvilResult result, bool once);

    const bool active_;
    LogSink log_;
    mutable std::mutex mutex_;
    std::vector<BenchmarkParticipant> participants_;  // under mutex_
    std::set<std::string> warned_;                    // under mutex_
};

// `group.field` or `field`: identifiers of [a-z0-9_], zero or one dot, at
// most ANVIL_BENCHMARK_MAX_NAME characters. Enum options and learning()
// reasons follow the same rule.
bool is_valid_benchmark_name(std::string_view name);

}  // namespace anvil
