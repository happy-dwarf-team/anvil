// Unit tests for the Manager's start-up benchmark (docs/spec/benchmark-v1.md):
// the start record as the core writes it, which starts count, the ABBA order,
// the extra-start budget and the result. Part of anvil_manager_tests.
#include "benchmark_series.hpp"
#include "core/benchmark_record.hpp"
#include "json.hpp"
#include "test_support.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace anvil;
using namespace anvil::manager;

namespace {

// A participant of the core's record, with one field of each kind.
BenchmarkParticipant participant(const std::string& id) {
    BenchmarkParticipant p;
    p.id = id;
    p.done = true;
    BenchmarkField opened{"open_warmup.opened", ANVIL_BENCHMARK_INT, ANVIL_BENCHMARK_UNIT_COUNT, "Files opened", {}};
    BenchmarkField mode{"open_warmup.mode", ANVIL_BENCHMARK_ENUM, ANVIL_BENCHMARK_UNIT_NONE, "", {"off", "on"}};
    BenchmarkField share{"share", ANVIL_BENCHMARK_NUMBER, ANVIL_BENCHMARK_UNIT_PERCENT, "", {}};
    BenchmarkField flag{"flag", ANVIL_BENCHMARK_FLAG, ANVIL_BENCHMARK_UNIT_NONE, "", {}};
    p.fields = {opened, mode, share, flag};
    p.values = {int64_t{11500}, BenchmarkOption{1}, 12.5, BenchmarkValue{}};
    p.open_cost = BenchmarkOpenCost{11500, 115.0};  // 0.01 ms per file: warm
    return p;
}

// What the core writes for a start of `arm` comparing "bellows".
BenchmarkRecord core_record(bool with, double ready_ms) {
    BenchmarkRecord r;
    r.write = "end";
    r.anvil_version = "1.5.0";
    r.arm = BenchmarkArm{with, "bellows"};
    r.process_id = 4242;
    r.qpc_frequency = 10'000'000;
    r.entry_qpc = 1'000'000;
    r.ready_qpc = r.entry_qpc + static_cast<int64_t>((ready_ms - 100.0) * 10'000);
    r.creation_filetime = 133'000'000'000'000'000ull;
    r.entry_filetime = r.creation_filetime + 100 * 10'000;  // 100 ms after creation
    r.plugins.push_back(BenchmarkRecordPlugin{"anvil-game", "1.5.0", "loaded", false, "", std::nullopt});
    BenchmarkRecordPlugin bellows{"bellows", "", "disabled", true, "", std::nullopt};
    if (with) bellows = BenchmarkRecordPlugin{"bellows", "0.10.0", "loaded", false, "", participant("bellows")};
    r.plugins.push_back(bellows);
    r.game_first_opens = BenchmarkOpenCost{11817, 11817 * 0.031};  // warm
    r.first_start_since_boot = false;
    r.definitions_changed = std::nullopt;
    r.input_during_start = false;
    return r;
}

StartObservation seen(const BenchmarkRecord& core) {
    StartObservation o;
    std::string error;
    o.record = parse_start_record(benchmark_record_json(core), error);
    if (!o.record) std::printf("  record error: %s\n", error.c_str());
    return o;
}

// A start of `arm` with the given time and open cost per file (ms).
StartObservation start(Arm arm, double ready_ms, double game_ms_per_file = 0.03) {
    BenchmarkRecord core = core_record(arm == Arm::With, ready_ms);
    core.game_first_opens = BenchmarkOpenCost{10000, 10000 * game_ms_per_file};
    if (arm == Arm::With) {
        // Without an open_cost() of its own, the game's first opens decide.
        core.plugins[1].participant->open_cost.reset();
    }
    return seen(core);
}

std::string arms(const Series& s) {
    std::string out;
    for (const StartRow& r : s.rows()) out += r.arm == Arm::Without ? 'A' : 'B';
    return out;
}

}  // namespace

void manager_benchmark_tests() {
    run("json: values, order and escapes read back; bad documents are refused", [] {
        std::string error;
        const auto v = parse_json(R"({"b":[1,-2,3.5,1e3,true,null],"a":"x\"\u00e9\ud83d\ude00\n","n":9007199254740993})", error);
        CHECK(v && v->is_object() && v->members().size() == 3 && v->members()[0].first == "b");
        CHECK((*v)["b"].items().size() == 6 && (*v)["b"].items()[1].is_integer() && (*v)["b"].items()[1].as_integer() == -2);
        CHECK(!(*v)["b"].items()[2].is_integer() && (*v)["b"].items()[2].as_number() == 3.5);
        CHECK(!(*v)["b"].items()[3].is_integer() && (*v)["b"].items()[3].as_number() == 1000.0);
        CHECK((*v)["a"].as_string() == "x\"\xC3\xA9\xF0\x9F\x98\x80\n");
        CHECK((*v)["n"].as_integer() == 9007199254740993);  // beyond a double's precision
        CHECK((*v)["missing"].is_null());
        const std::string compact = "{\"b\":[1,-2,3.5,1000,true,null],\"a\":\"x\\\"\xC3\xA9\xF0\x9F\x98\x80\\n\","
                                    "\"n\":9007199254740993}";
        CHECK(write_json(*v) == compact);
        const auto again = parse_json(write_json(*v, true), error);
        CHECK(again && *again == *v);
        for (const char* bad : {"", "{", "[1,]", "{\"a\":1,}", "01", "1.", "\"\\ud800\"", "{\"a\":1,\"a\":2}", "nul",
                                "[1] [2]", "\"a\tb\"", "1e999"}) {
            CHECK(!parse_json(bad, error));
        }
        std::string deep(70, '[');
        CHECK(!parse_json(deep + std::string(70, ']'), error) && error.find("deep") != std::string::npos);
    });

    run("json: pretty output keeps short scalar groups on one line", [] {
        Json o = Json::object();
        o.set("id", "bellows");
        o.set("version", "0.10.0");
        Json doc = Json::object();
        doc.set("compared", o);
        Json times = Json::array();
        for (int t : {40900, 41500}) times.push(t);
        doc.set("times_ms", times);
        doc.set("empty", Json::object());
        doc.set("ms", 0.1);
        CHECK(write_json(doc, true) ==
              "{\n  \"compared\": { \"id\": \"bellows\", \"version\": \"0.10.0\" },\n  \"times_ms\": [40900, 41500],\n"
              "  \"empty\": {},\n  \"ms\": 0.1\n}\n");
        doc.set("ms", std::nan(""));
        CHECK(write_json(doc).find("\"ms\":null") != std::string::npos);
    });

    run("start record: the core's own record reads back", [] {
        const StartObservation o = seen(core_record(true, 33800.0));
        CHECK(o.record.has_value());
        const StartRecord& r = *o.record;
        CHECK(r.write == "end" && r.anvil == "1.5.0" && r.arm == Arm::With && r.arm_plugin == "bellows");
        CHECK(r.process_id == 4242 && r.creation_filetime == 133'000'000'000'000'000ull);
        CHECK(r.time_ms && std::abs(*r.time_ms - 33800.0) < 0.01);
        CHECK(r.plugins.size() == 2 && r.plugin("anvil-game") && !r.plugin("anvil-game")->participant);
        const RecordPlugin* b = r.plugin("bellows");
        CHECK(b && b->state == "loaded" && b->version == "0.10.0" && b->participant && b->done && b->learning.empty());
        CHECK(b && b->open_cost && b->open_cost->files == 11500 && b->open_cost->ms == 115.0);
        CHECK(b && b->schema.size() == 4 && b->schema[0].label == "Files opened" && b->schema[0].unit == "count");
        CHECK(b && b->schema[1].kind == "enum" && b->schema[1].options.size() == 2 && b->schema[1].unit.empty());
        CHECK(b && b->fields["open_warmup.opened"].as_integer() == 11500 && b->fields["open_warmup.mode"].as_string() == "on");
        CHECK(b && b->fields["share"].as_number() == 12.5 && b->fields["flag"].is_null());
        CHECK(r.game_first_opens && r.game_first_opens->files == 11817);
        CHECK(r.first_start_since_boot == false && !r.definitions_changed && r.input_during_start == false);

        const StartObservation without = seen(core_record(false, 41000.0));
        CHECK(without.record && without.record->plugin("bellows")->state == "disabled" &&
              without.record->plugin("bellows")->by == "arm");

        std::string error;
        CHECK(!parse_start_record("{\"format\":2}", error));
        CHECK(!parse_start_record("[]", error));
        BenchmarkRecord before_ready = core_record(false, 1000.0);
        before_ready.ready_qpc = 0;
        const auto early = parse_start_record(benchmark_record_json(before_ready), error);
        CHECK(early && !early->time_ms);
    });

    run("classify: valid starts and their open cost class", [] {
        std::string stop;
        StartRow row = classify_start(start(Arm::Without, 41000.4), Arm::Without, "bellows", stop);
        CHECK(row.valid && row.discard_reason.empty() && stop.empty() && row.time_ms == 41000);
        CHECK(row.open_cost && !row.open_cost->from_plugin && row.cold_class == "warm");
        CHECK(row.plugins.size() == 2 && row.input_during_start == false && !row.killed);

        row = classify_start(start(Arm::Without, 52000.0, 1.5), Arm::Without, "bellows", stop);
        CHECK(row.valid && row.cold_class == "cold");
        row = classify_start(start(Arm::Without, 45000.0, 0.33), Arm::Without, "bellows", stop);
        CHECK(row.valid && row.cold_class == "unclear" && row.not_counted());

        // With the plugin, its open_cost() replaces the game's first opens.
        BenchmarkRecord core = core_record(true, 33800.0);
        core.game_first_opens = BenchmarkOpenCost{10000, 10000 * 2.0};  // the game's own would say cold
        row = classify_start(seen(core), Arm::With, "bellows", stop);
        CHECK(row.valid && row.open_cost && row.open_cost->from_plugin && row.open_cost->plugin == "bellows");
        CHECK(row.cold_class == "warm");
        core.plugins[1].participant->open_cost = BenchmarkOpenCost{11500, 11500 * 0.1};  // 0.1 ms: unclear for a plugin
        CHECK(classify_start(seen(core), Arm::With, "bellows", stop).cold_class == "unclear");
        core.plugins[1].participant->open_cost = BenchmarkOpenCost{11500, 9100.0};
        CHECK(classify_start(seen(core), Arm::With, "bellows", stop).cold_class == "cold");

        // No open cost at all is unclear; labels never decide.
        core = core_record(false, 41000.0);
        core.game_first_opens.reset();
        core.first_start_since_boot = true;
        CHECK(classify_start(seen(core), Arm::Without, "bellows", stop).cold_class == "unclear");
        core.game_first_opens = BenchmarkOpenCost{10000, 300.0};
        row = classify_start(seen(core), Arm::Without, "bellows", stop);
        CHECK(row.cold_class == "warm" && row.first_since_boot == true);

        // A participant with done: false does not invalidate the time.
        core = core_record(true, 33800.0);
        core.plugins[1].participant->done = false;
        CHECK(classify_start(seen(core), Arm::With, "bellows", stop).valid);
        CHECK(open_cost_class(std::nullopt) == "unclear");
        CHECK(open_cost_class(StartOpenCost{false, "", 0, 0.0}) == "unclear");
        CHECK(open_cost_class(StartOpenCost{false, "", 100, 20.0}) == "warm");
        CHECK(open_cost_class(StartOpenCost{false, "", 100, 100.0}) == "cold");
    });

    run("classify: discarded starts and the reasons that stop the series", [] {
        std::string stop;
        StartObservation o;
        StartRow row = classify_start(o, Arm::Without, "bellows", stop);
        CHECK(!row.valid && row.discard_reason == "no_record" && stop.empty() && row.cold_class.empty() && !row.time_ms);
        o.crash = StartCrash{"in_plugin_code", "bellows", true};
        row = classify_start(o, Arm::Without, "bellows", stop);
        CHECK(row.discard_reason == "crash" && stop.empty() && row.crash && row.crash->plugin == "bellows");
        o = StartObservation{};
        o.timed_out = true;
        row = classify_start(o, Arm::Without, "bellows", stop);
        CHECK(row.discard_reason == "timeout" && stop == "timeout" && row.killed);

        BenchmarkRecord core = core_record(true, 33800.0);
        core.plugins[1].participant->learning = "new_mod_set";
        row = classify_start(seen(core), Arm::With, "bellows", stop);
        CHECK(row.discard_reason == "plugin_learning" && stop.empty() && row.time_ms == 33800);
        CHECK(row.open_cost && row.open_cost->from_plugin);  // raw values stay in the row

        core = core_record(true, 33800.0);
        core.plugins[1] = BenchmarkRecordPlugin{"bellows", "", "skipped", false, "safe_mode", std::nullopt};
        CHECK(classify_start(seen(core), Arm::With, "bellows", stop).discard_reason == "safe_mode" && stop == "safe_mode");
        core.plugins[1] = BenchmarkRecordPlugin{"bellows", "", "skipped", false, "verification", std::nullopt};
        CHECK(classify_start(seen(core), Arm::With, "bellows", stop).discard_reason == "arm_mismatch" &&
              stop == "plugin_not_loaded");
        core.plugins[1] = BenchmarkRecordPlugin{"bellows", "0.9.0", "loaded", false, "", std::nullopt};
        CHECK(classify_start(seen(core), Arm::With, "bellows", stop).discard_reason == "not_participant" &&
              stop == "plugin_not_participant");
        // Without the plugin, only the arm's switch may have disabled it.
        CHECK(classify_start(seen(core), Arm::Without, "bellows", stop).discard_reason == "arm_mismatch");
        core = core_record(false, 41000.0);
        core.plugins[1].disabled_by_arm = false;
        CHECK(classify_start(seen(core), Arm::Without, "bellows", stop).discard_reason == "arm_mismatch");
        // The record of another arm or plugin.
        CHECK(classify_start(seen(core_record(false, 41000.0)), Arm::With, "bellows", stop).discard_reason == "arm_mismatch");
        CHECK(classify_start(seen(core_record(false, 41000.0)), Arm::Without, "other", stop).discard_reason == "arm_mismatch");

        o = start(Arm::Without, 41000.0);
        o.environment_changed = true;
        CHECK(classify_start(o, Arm::Without, "bellows", stop).discard_reason == "environment_changed" &&
              stop == "environment_changed");

        // A crash after ready or a kill keeps the time and stops the series.
        o = start(Arm::Without, 41000.0);
        o.crash = StartCrash{"no_plugin", "", false};
        row = classify_start(o, Arm::Without, "bellows", stop);
        CHECK(row.valid && row.time_ms == 41000 && row.crash && stop == "crashed");
        o = start(Arm::Without, 41000.0);
        o.killed = true;
        row = classify_start(o, Arm::Without, "bellows", stop);
        CHECK(row.valid && row.killed && stop == "quit_failed");
    });

    run("series: ABBA pairs, the first arm as given, complete at the target", [] {
        Series s("bellows", 3, Arm::Without);
        CHECK(s.expected_starts() == 6 && s.next() == Arm::Without);
        while (const auto arm = s.next()) s.add(start(*arm, *arm == Arm::Without ? 41000.0 : 34000.0));
        CHECK(arms(s) == "ABBAAB" && s.complete() && s.stop_reason().empty() && s.extra_starts_used() == 0);
        CHECK(s.warm(Arm::Without) == 3 && s.warm(Arm::With) == 3 && s.expected_starts() == 6);

        Series b("bellows", 5, Arm::With);
        while (const auto arm = b.next()) b.add(start(*arm, 40000.0));
        CHECK(arms(b) == "BAABBAABBA" && b.complete() && b.rows().back().position == 10);
    });

    run("series: starts that do not count are appended, alternating between the short arms", [] {
        Series s("bellows", 3, Arm::Without);
        std::vector<StartObservation> planned = {start(Arm::Without, 41000.0), StartObservation{}, start(Arm::With, 34000.0),
                                                 start(Arm::Without, 41000.0, 0.5), start(Arm::Without, 41000.0),
                                                 start(Arm::With, 34000.0)};
        for (const StartObservation& o : planned) s.add(o);
        // Starts 2 (no record, with) and 4 (unclear, without) left both arms short.
        CHECK(!s.over() && s.extra_starts_used() == 2 && s.expected_starts() == 8);
        CHECK(s.next() == Arm::With);  // the ABBA order goes on
        s.add(StartObservation{});
        CHECK(s.next() == Arm::Without);  // then the arms alternate
        s.add(start(Arm::Without, 41000.0));
        CHECK(s.next() == Arm::With);
        s.add(start(Arm::With, 34000.0));
        CHECK(s.complete() && arms(s) == "ABBAABBAB" && s.extra_starts_used() == 3);

        // A cold start leaves its arm short without using the budget.
        Series c("bellows", 3, Arm::Without);
        c.add(start(Arm::Without, 52000.0, 4.0));
        while (const auto arm = c.next()) c.add(start(*arm, 40000.0));
        CHECK(c.complete() && c.rows().size() == 7 && c.extra_starts_used() == 0 && arms(c) == "ABBAABA");
    });

    run("series: the budget, crashes and stop reasons end it", [] {
        Series s("bellows", 3, Arm::Without);
        for (int i = 0; i < 5; ++i) s.add(StartObservation{});
        CHECK(s.over() && !s.complete() && s.stop_reason() == "too_many_discarded" && s.extra_starts_used() == 5);

        Series cold("bellows", 3, Arm::Without);
        for (int i = 0; i < kMaxColdStarts + 1; ++i) cold.add(start(*cold.next(), 52000.0, 4.0));
        CHECK(cold.stop_reason() == "too_many_discarded" && cold.extra_starts_used() == 0);

        Series crash("bellows", 3, Arm::Without);
        StartObservation crashed;
        crashed.crash = StartCrash{"no_plugin", "", true};
        crash.add(crashed);
        CHECK(!crash.over());
        crash.add(start(*crash.next(), 34000.0));
        crash.add(crashed);
        CHECK(crash.stop_reason() == "crashed" && crash.rows().size() == 3);

        Series learn("bellows", 3, Arm::With);
        BenchmarkRecord core = core_record(true, 33800.0);
        core.plugins[1].participant->learning = "new_mod_set";
        learn.add(seen(core));
        CHECK(!learn.over() && learn.extra_starts_used() == 1 && learn.next() == Arm::Without);

        Series player("bellows", 3, Arm::Without);
        player.add(start(Arm::Without, 41000.0));
        player.stop("player");
        CHECK(player.over() && !player.next() && player.stop_reason() == "player" && player.expected_starts() == 1);

        Series safe("bellows", 3, Arm::Without);
        safe.add(start(Arm::Without, 41000.0));
        core = core_record(true, 33800.0);
        core.plugins[1] = BenchmarkRecordPlugin{"bellows", "", "skipped", false, "safe_mode", std::nullopt};
        safe.add(seen(core));
        CHECK(safe.stop_reason() == "safe_mode" && !safe.next());
        for (const char* reason : {"player", "too_many_discarded", "quit_failed", "crashed", "safe_mode", "plugin_not_loaded",
                                   "plugin_not_participant", "timeout", "environment_changed"}) {
            CHECK(stop_reason_text(reason).back() == '.');
        }
    });

    run("result: warm medians, the difference, cold starts and what was not measured", [] {
        Series s("bellows", 3, Arm::Without);
        BenchmarkRecord boot = core_record(false, 52300.0);
        boot.first_start_since_boot = true;
        boot.game_first_opens = BenchmarkOpenCost{2400, 9600.0};
        s.add(seen(boot));                                          // 1 A cold, after boot
        BenchmarkRecord learning = core_record(true, 39000.0);
        learning.plugins[1].participant->learning = "new_mod_set";
        s.add(seen(learning));                                      // 2 B learning
        s.add(start(Arm::With, 33800.0, 0.01));                     // 3 B
        s.add(start(Arm::Without, 40900.0));                        // 4 A
        s.add(start(Arm::Without, 41500.0));                        // 5 A
        s.add(start(Arm::With, 33500.0));                           // 6 B
        StartObservation input = start(Arm::With, 34100.0);
        input.record->input_during_start = true;
        s.add(input);                                               // 7 B, appended for 2
        s.add(start(Arm::Without, 41000.0));                        // 8 A, appended for 1
        CHECK(s.complete() && arms(s) == "ABBAABBA");

        const SeriesResult r = series_result(s);
        const ArmSummary& without = r.warm[arm_index(Arm::Without)];
        const ArmSummary& with = r.warm[arm_index(Arm::With)];
        CHECK((without.times_ms == std::vector<int64_t>{40900, 41500, 41000}));
        CHECK(without.median_ms == 41000.0 && without.min_ms == 40900 && without.max_ms == 41500);
        CHECK((with.times_ms == std::vector<int64_t>{33800, 33500, 34100}) && with.median_ms == 33800.0);
        CHECK(r.difference && r.difference->ms == -7200.0 && r.difference->percent == -17.56 && !r.difference->ranges_overlap);
        CHECK(r.cold.size() == 1 && r.cold[0].position == 1 && r.cold[0].kind == "after_boot" && r.cold[0].time_ms == 52300);
        CHECK(!r.cold_difference);
        CHECK(r.not_counted.size() == 1 && r.not_counted[0].position == 2 && r.not_counted[0].reason == "plugin_learning");
        CHECK((r.input_during_start == std::vector<int>{7}) && r.extra_starts_used == 1);
        CHECK((r.not_measured == std::vector<std::string>{"time_to_ready", "no_cold_with"}));

        // Overlapping ranges, an even median, a stopped series below its target.
        Series o("bellows", 3, Arm::Without);
        o.add(start(Arm::Without, 40000.0));
        o.add(start(Arm::With, 39000.0));
        o.add(start(Arm::With, 41000.0));
        o.add(start(Arm::Without, 42000.0));
        o.stop("player");
        const SeriesResult p = series_result(o);
        CHECK(p.warm[0].median_ms == 41000.0 && p.warm[1].median_ms == 40000.0);
        CHECK(p.difference && p.difference->ms == -1000.0 && p.difference->ranges_overlap);
        CHECK(std::abs(p.difference->percent - (-2.44)) < 1e-9);
        CHECK((p.not_measured == std::vector<std::string>{"time_to_ready", "no_cold_without", "no_cold_with", "below_target",
                                                          "stopped"}));

        // One arm without warm starts: no difference; cold in both arms: a cold difference.
        Series c("bellows", 3, Arm::Without);
        c.add(start(Arm::Without, 50000.0, 2.0));
        c.add(start(Arm::With, 40000.0, 2.0));
        c.add(start(Arm::With, 34000.0));
        c.stop("player");
        const SeriesResult q = series_result(c);
        CHECK(!q.difference && !q.warm[0].median_ms && !q.warm[0].min_ms && q.warm[1].times_ms.size() == 1);
        CHECK(q.cold.size() == 2 && q.cold[1].kind == "open_cost" && q.cold_difference && q.cold_difference->ms == -10000.0);
        CHECK(q.cold_difference->percent == -20.0);
    });
}
