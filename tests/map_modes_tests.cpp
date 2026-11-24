// anvil-map (plugins/anvil-map): the registry of plugin modes, their borrowed
// numbers, colour buffers and the text rules, without the game. The hooks
// are tried in the game (docs/spec/map-modes-v1.md §9).
#include "test_support.hpp"

#include "../plugins/anvil-map/src/files.hpp"
#include "../plugins/anvil-map/src/modes.hpp"
#include "../plugins/anvil-map/src/text.hpp"

#include <anvil/anvil_map.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace anvil_map;

AnvilMapModeInfo info(const char* id, const char* label = "Cores", std::uint32_t tab = 0) {
    AnvilMapModeInfo i{};
    i.size = sizeof(i);
    i.tab = tab;
    i.id = id;
    i.label = label;
    return i;
}

void show_tests() {
    run("map modes: a show request waits for the step, the last one wins", [] {
        Registry r;
        const AnvilMapMode* a = nullptr;
        const AnvilMapMode* b = nullptr;
        std::string why;
        AnvilMapModeInfo ia = info("p.a"), ib = info("p.b");
        CHECK(r.add(&ia, false, &a, why) == ANVIL_OK && r.add(&ib, false, &b, why) == ANVIL_OK);
        CHECK(r.take_show() == nullptr);
        CHECK(r.owns(a) && r.owns(b) && !r.owns(nullptr));
        CHECK(r.request_show(a, r.epoch()) && r.request_show(b, r.epoch()));
        CHECK(r.take_show() == b && r.take_show() == nullptr);
        // A mode of another registry, a forged handle and NULL are refused.
        Registry other;
        const AnvilMapMode* c = nullptr;
        AnvilMapModeInfo ic = info("p.c");
        CHECK(other.add(&ic, false, &c, why) == ANVIL_OK);
        CHECK(!r.owns(c) && !r.request_show(c, r.epoch()) && !r.request_show(nullptr, r.epoch()));
        AnvilMapMode forged;
        forged.number = a->number;
        CHECK(!r.request_show(&forged, r.epoch()) && r.take_show() == nullptr);
        // The end of a session drops a waiting request.
        CHECK(r.request_show(a, r.epoch()));
        r.clear_session();
        CHECK(r.take_show() == nullptr);
        // A request checked in a session that ended before it was kept is
        // dropped too: it never reaches the next session's step.
        const std::uint64_t before = r.epoch();
        r.clear_session();
        CHECK(r.request_show(a, before) && r.take_show() == nullptr);
        CHECK(r.request_show(b, r.epoch()) && r.take_show() == b);
        // A handle that is not a mode is not read: an address of no AnvilMapMode.
        alignas(AnvilMapMode) static const unsigned char junk[sizeof(AnvilMapMode)] = {0xFF, 0xFF, 0xFF, 0xFF};
        CHECK(!r.owns(reinterpret_cast<const AnvilMapMode*>(junk)));
    });

    run("map modes: show() answers UNSUPPORTED, INVALID_ARGUMENT and WRONG_PHASE before it asks", [] {
        Registry r;
        const AnvilMapMode* a = nullptr;
        std::string why;
        AnvilMapModeInfo ia = info("p.a");
        CHECK(r.add(&ia, false, &a, why) == ANVIL_OK);
        std::atomic<bool> in_session{false};
        // Another loader of map modes: refused before the handle is looked at.
        CHECK(ask_show(r, a, true, in_session) == ANVIL_ERROR_UNSUPPORTED);
        CHECK(ask_show(r, nullptr, true, in_session) == ANVIL_ERROR_UNSUPPORTED);
        CHECK(ask_show(r, nullptr, false, in_session) == ANVIL_ERROR_INVALID_ARGUMENT);
        AnvilMapMode forged;
        forged.number = a->number;
        CHECK(ask_show(r, &forged, false, in_session) == ANVIL_ERROR_INVALID_ARGUMENT);
        // Outside a session (the menus, between two sessions).
        CHECK(ask_show(r, a, false, in_session) == ANVIL_ERROR_WRONG_PHASE);
        CHECK(r.take_show() == nullptr);
        in_session = true;
        CHECK(ask_show(r, &forged, false, in_session) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(ask_show(r, a, true, in_session) == ANVIL_ERROR_UNSUPPORTED && r.take_show() == nullptr);
        CHECK(ask_show(r, a, false, in_session) == ANVIL_OK && r.take_show() == a);
    });

    run("map modes: the step switches to the asked mode through SetCurrentMapMode, once", [] {
        Registry r;
        const AnvilMapMode* a = nullptr;
        std::string why;
        AnvilMapModeInfo ia = info("p.a");
        CHECK(r.add(&ia, false, &a, why) == ANVIL_OK);
        std::atomic<bool> in_session{true};
        std::vector<std::pair<std::uintptr_t, std::int32_t>> calls;
        const auto set = [&](std::uintptr_t session, std::int32_t number) { calls.emplace_back(session, number); };
        constexpr std::uintptr_t kSession = 0x5E55;
        // Nothing asked: nothing called.
        CHECK(switch_to_asked(r, true, kSession, 1, set) == nullptr && calls.empty());
        // Asked while the political mode shows: one call with the session and the borrowed number.
        CHECK(ask_show(r, a, false, in_session) == ANVIL_OK);
        CHECK(switch_to_asked(r, true, kSession, 1, set) == a);
        CHECK(calls.size() == 1 && calls[0].first == kSession && calls[0].second == a->number);
        // The request was taken: the next step calls nothing.
        CHECK(switch_to_asked(r, true, kSession, a->number, set) == nullptr && calls.size() == 1);
        // Already shown: taken, not called again.
        CHECK(ask_show(r, a, false, in_session) == ANVIL_OK);
        CHECK(switch_to_asked(r, true, kSession, a->number, set) == nullptr && calls.size() == 1);
        CHECK(r.take_show() == nullptr);
        // The current mode could not be read: taken and dropped, not called.
        CHECK(ask_show(r, a, false, in_session) == ANVIL_OK);
        CHECK(switch_to_asked(r, false, 0, -1, set) == nullptr && calls.size() == 1);
        CHECK(r.take_show() == nullptr);
        // Asked, then the session ended before the step: the next session's step calls nothing.
        CHECK(ask_show(r, a, false, in_session) == ANVIL_OK);
        r.clear_session();
        CHECK(switch_to_asked(r, true, kSession, 1, set) == nullptr && calls.size() == 1);
    });
}

void id_tests() {
    run("map modes: ids are <plugin-id>.<name>", [] {
        CHECK(valid_id("player-cores.cores"));
        CHECK(valid_id("a.b_c-1"));
        CHECK(!valid_id("cores"));
        CHECK(!valid_id(".cores"));
        CHECK(!valid_id("player-cores."));
        CHECK(!valid_id("Player.cores"));
        CHECK(!valid_id("player.co.res"));
        CHECK(!valid_id("player.cores!"));
        CHECK(valid_id("p." + std::string(32, 'x')));
        CHECK(!valid_id("p." + std::string(33, 'x')));
    });
}

void registry_tests() {
    run("map modes: numbers are lent from the pool in order, 80-83 first", [] {
        Registry r;
        std::vector<std::int32_t> numbers;
        for (int k = 0; k < 13; ++k) {
            const std::string id = "p.m" + std::to_string(k);
            const AnvilMapModeInfo i = info(id.c_str());
            const AnvilMapMode* m = nullptr;
            std::string why;
            CHECK(r.add(&i, false, &m, why) == ANVIL_OK);
            numbers.push_back(m->number);
            CHECK(r.by_number(m->number) == m);
        }
        CHECK((numbers == std::vector<std::int32_t>{80, 81, 82, 83, 50, 57, 58, 59, 60, 66, 68, 71, 84}));
        const AnvilMapModeInfo extra = info("p.fourteenth");
        const AnvilMapMode* m = nullptr;
        std::string why;
        CHECK(r.add(&extra, false, &m, why) == ANVIL_ERROR_UNSUPPORTED && m == nullptr);
        CHECK(r.by_number(1) == nullptr && r.by_number(-1) == nullptr && r.by_number(85) == nullptr);
    });

    run("map modes: a bad registration is refused and lends nothing", [] {
        Registry r;
        const AnvilMapMode* m = nullptr;
        std::string why;
        CHECK(r.add(nullptr, false, &m, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        AnvilMapModeInfo small = info("p.a");
        small.size = 8;
        CHECK(r.add(&small, false, &m, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        AnvilMapModeInfo empty = info("p.a", "");
        CHECK(r.add(&empty, false, &m, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        const std::string long_label(65, 'x');
        AnvilMapModeInfo too_long = info("p.a", long_label.c_str());
        CHECK(r.add(&too_long, false, &m, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        AnvilMapModeInfo bad_utf8 = info("p.a", "\xC3(");
        CHECK(r.add(&bad_utf8, false, &m, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        AnvilMapModeInfo tab = info("p.a", "A", 5);
        CHECK(r.add(&tab, false, &m, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        AnvilMapModeInfo sprite = info("p.a");
        sprite.sprite = "GFX bad";
        CHECK(r.add(&sprite, false, &m, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(r.size() == 0);
        const AnvilMapModeInfo good = info("p.a");
        CHECK(r.add(&good, false, &m, why) == ANVIL_OK && m->number == 80);
        CHECK(r.add(&good, false, &m, why) == ANVIL_ERROR_ALREADY_EXISTS);
        CHECK(r.size() == 1);
    });

    run("map modes: tabs keep the order of registration; the default is the last tab", [] {
        Registry r;
        const AnvilMapMode* m = nullptr;
        std::string why;
        const AnvilMapModeInfo a = info("p.a"), b = info("p.b", "B", 2), c = info("p.c", "C", 4);
        CHECK(r.add(&a, false, &m, why) == ANVIL_OK && m->tab == 4);
        CHECK(r.add(&b, false, &m, why) == ANVIL_OK && m->tab == 2);
        CHECK(r.add(&c, false, &m, why) == ANVIL_OK);
        std::int32_t out[13] = {};
        CHECK(r.tab_numbers(4, out, 13) == 2 && out[0] == 80 && out[1] == 82);
        CHECK(r.tab_numbers(2, out, 13) == 1 && out[0] == 81);
        CHECK(r.tab_numbers(1, out, 13) == 0);
        CHECK(r.tab_numbers(4, out, 1) == 1);
    });

    run("map modes: labels are written as the game draws text", [] {
        Registry r;
        const AnvilMapMode* m = nullptr;
        std::string why;
        AnvilMapModeInfo i = info("p.a", "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87");  // "Zażółć"
        i.description = "Cores \xE2\x80\x93 yours";                                   // en dash
        CHECK(r.add(&i, false, &m, why) == ANVIL_OK);
        CHECK(m->label == "Za?\xF3??");  // of "żółć" only ó is in Windows-1252
        CHECK(m->description == "Cores \x96 yours");
        AnvilMapModeInfo j = info("p.b", "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87");
        CHECK(r.add(&j, true, &m, why) == ANVIL_OK && m->label == "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87");
    });
}

void color_tests() {
    run("map modes: a colour buffer is checked and copied", [] {
        Colors c;
        const std::vector<std::uint32_t> values = {ANVIL_MAP_RGB(1, 2, 3), ANVIL_MAP_RGB(255, 0, 0),
                                                   ANVIL_MAP_NO_COLOR, ANVIL_MAP_RGB(0, 0, 255)};
        CHECK(make_colors(values.data(), 4, 10, c) == ANVIL_OK);
        CHECK(c.values.size() == 4 && c.values[0] == ANVIL_MAP_NO_COLOR && c.values[1] == 0xFF0000u &&
              c.values[2] == ANVIL_MAP_NO_COLOR && !c.complete);
        CHECK(make_colors(values.data(), 11, 10, c) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(make_colors(nullptr, 1, 10, c) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(make_colors(nullptr, 0, 10, c) == ANVIL_OK && c.values.empty() && !c.complete);
        const std::uint32_t bad[] = {0, 0x01000000u};
        CHECK(make_colors(bad, 2, 10, c) == ANVIL_ERROR_INVALID_ARGUMENT);
        const std::uint32_t full[] = {0x123456, 1, 2};  // province 0 counts for nothing
        CHECK(make_colors(full, 3, 3, c) == ANVIL_OK && c.complete && c.values[0] == ANVIL_MAP_NO_COLOR);
        const std::uint32_t none[] = {0, 1, 0xFFABCDEFu};
        CHECK(make_colors(none, 3, 3, c) == ANVIL_OK && !c.complete && c.values[2] == ANVIL_MAP_NO_COLOR);
    });

    run("map modes: a new buffer is a new generation; the end of a session drops it", [] {
        Registry r;
        const AnvilMapMode* m = nullptr;
        std::string why;
        const AnvilMapModeInfo i = info("p.a");
        CHECK(r.add(&i, false, &m, why) == ANVIL_OK);
        auto* mode = const_cast<AnvilMapMode*>(m);
        const std::uint64_t g = mode->generation.load();
        set_colors(*mode, std::make_shared<Colors>());
        CHECK(mode->generation.load() == g + 1 && mode->snapshot() != nullptr);
        keep_text(*mode, 7, "Yours");
        CHECK(kept_text(*mode, 7) == "Yours" && kept_text(*mode, 8).empty());
        r.clear_session();
        CHECK(mode->snapshot() == nullptr && mode->generation.load() == g + 2 && kept_text(*mode, 7).empty());

        // A buffer of a session that ended between the caller's check and the
        // store is dropped; one of this epoch is kept; a forged handle is refused.
        const std::uint64_t before = r.epoch();
        r.clear_session();
        CHECK(!r.store_colors(mode, std::make_shared<Colors>(), before) && mode->snapshot() == nullptr);
        CHECK(r.store_colors(mode, std::make_shared<Colors>(), r.epoch()) && mode->snapshot() != nullptr);
        AnvilMapMode forged;
        CHECK(!r.store_colors(&forged, std::make_shared<Colors>(), r.epoch()) && forged.snapshot() == nullptr);
    });
}

void text_tests() {
    run("map modes: a tooltip's province goes to describe only within the province limit", [] {
        // The ID is read from the game's memory (anvil#545, the anvil#426 class).
        CHECK(describable(1, 4942) && describable(4941, 4942));
        CHECK(!describable(0, 4942) && !describable(-1, 4942) && !describable(4942, 4942));
        CHECK(!describable(0x7FFFFFFF, 4942) && !describable(INT32_MIN, 4942));
        // After "Name (Owner) - (@TAG)" the lines follow; after "Occupied by X" they start a line.
        CHECK(!needs_line_break(')') && !needs_line_break(' ') && !needs_line_break('\n') && !needs_line_break(0));
        CHECK(needs_line_break('a') && needs_line_break('!'));
        CHECK(!describable(1, 0));  // no limit known: none
    });

    run("map modes: UTF-8 is cut at a whole character and at the first bad byte", [] {
        CHECK(utf8_prefix("abc") == "abc");
        CHECK(utf8_prefix("a\xC5\x82", 2) == "a");
        CHECK(utf8_prefix("a\xC5\x82", 3) == "a\xC5\x82");
        CHECK(utf8_prefix("ok\xFFno") == "ok");
        CHECK(utf8_prefix("\xED\xA0\x80") == "");          // a surrogate
        CHECK(utf8_prefix("\xC0\xAF") == "");              // overlong
        CHECK(utf8_prefix("\xF0\x9F\x98\x80!") == "\xF0\x9F\x98\x80!");
        CHECK(valid_utf8("Kraków") && !valid_utf8("\x80"));
    });

    run("map modes: text for the game is literal, in UTF-8 or Windows-1252", [] {
        CHECK(game_text("Line 1\nLine 2", false) == "Line 1\nLine 2");
        CHECK(game_text("tab\there\r", false) == "tabhere");
        CHECK(game_text("\xE2\x82\xAC 5", false) == "\x80 5");
        CHECK(game_text("\xE2\x82\xAC 5", true) == "\xE2\x82\xAC 5");
        CHECK(game_text("\xF0\x9F\x98\x80", false) == "?");
        CHECK(game_text("\xC2\xA7Rred", false) == "?Rred");
        CHECK(game_text("\xC2\xA7Rred", true) == "?Rred");
        CHECK(game_text("$PROV$", false) == "$PROV$");
        CHECK(game_text("ok\xFFtail", true) == "ok");
        CHECK(windows_1252(0x153) == 0x9C && windows_1252(0x81) == 0 && windows_1252(0xE9) == 0xE9);
    });
}

void legend_tests() {
    run("map modes: a legend is checked and copied (set_legend)", [] {
        std::vector<LegendEntry> out;
        std::string why;
        std::string label = "Holy site";
        const AnvilMapLegendEntry two[] = {{ANVIL_MAP_RGB(224, 176, 0), 0, label.c_str()},
                                           {ANVIL_MAP_RGB(128, 64, 192), 0, "Faith 5 or more"}};
        CHECK(make_legend(two, 2, sizeof(AnvilMapLegendEntry), out, why) == ANVIL_OK);
        label = "changed";
        CHECK(out.size() == 2 && out[0].label == "Holy site" && out[0].color == 0xE0B000 && out[1].color == 0x8040C0);
        CHECK(make_legend(nullptr, 0, 0, out, why) == ANVIL_OK && out.empty());  // removes it

        // Bad legends change nothing.
        CHECK(make_legend(two, 2, sizeof(AnvilMapLegendEntry), out, why) == ANVIL_OK);
        const AnvilMapLegendEntry no_color[] = {{ANVIL_MAP_NO_COLOR, 0, "x"}};
        const AnvilMapLegendEntry no_label[] = {{0, 0, nullptr}};
        const AnvilMapLegendEntry empty[] = {{0, 0, ""}};
        const AnvilMapLegendEntry bad_utf8[] = {{0, 0, "\xFF"}};
        const std::string long_label(kLabelMax + 1, 'x');
        const AnvilMapLegendEntry too_long[] = {{0, 0, long_label.c_str()}};
        CHECK(make_legend(no_color, 1, sizeof(AnvilMapLegendEntry), out, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(make_legend(no_label, 1, sizeof(AnvilMapLegendEntry), out, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(make_legend(empty, 1, sizeof(AnvilMapLegendEntry), out, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(make_legend(bad_utf8, 1, sizeof(AnvilMapLegendEntry), out, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(make_legend(too_long, 1, sizeof(AnvilMapLegendEntry), out, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(make_legend(nullptr, 1, sizeof(AnvilMapLegendEntry), out, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(make_legend(two, 2, 8, out, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        std::vector<AnvilMapLegendEntry> many(ANVIL_MAP_LEGEND_MAX + 1, AnvilMapLegendEntry{0, 0, "x"});
        CHECK(make_legend(many.data(), ANVIL_MAP_LEGEND_MAX + 1, sizeof(AnvilMapLegendEntry), out, why) ==
              ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(make_legend(many.data(), ANVIL_MAP_LEGEND_MAX, sizeof(AnvilMapLegendEntry), out, why) == ANVIL_OK &&
              out.size() == ANVIL_MAP_LEGEND_MAX);

        // A newer entry: a longer stride reads the fields it knows.
        struct Newer {
            AnvilMapLegendEntry entry;
            std::uint64_t extra;
        } newer[2] = {{{1, 0, "a"}, 9}, {{2, 0, "b"}, 9}};
        CHECK(make_legend(&newer[0].entry, 2, sizeof(Newer), out, why) == ANVIL_OK && out[1].label == "b" &&
              out[1].color == 2);
    });

    run("map modes: a mode keeps its legend and counts each change", [] {
        Registry registry;
        AnvilMapModeInfo i = info("p.legend", "Holy sites");
        const AnvilMapMode* mode = nullptr;
        std::string why;
        CHECK(registry.add(&i, false, &mode, why) == ANVIL_OK && mode->label_utf8 == "Holy sites");
        auto* m = const_cast<AnvilMapMode*>(mode);
        CHECK(m->legend_generation.load() == 0 && legend_of(*m).empty());
        set_legend(*m, {{0xE0B000, "Holy site"}});
        CHECK(m->legend_generation.load() == 1 && legend_of(*m).size() == 1 && legend_of(*m)[0].label == "Holy site");
        registry.clear_session();  // a legend lasts across sessions
        CHECK(legend_of(*m).size() == 1);
        set_legend(*m, {});
        CHECK(m->legend_generation.load() == 2 && legend_of(*m).empty());
    });
}

// A world of a few provinces for the rules.
class TableWorld final : public files::World {
public:
    std::int32_t limit = 6;
    std::map<std::string, std::int32_t> flag_ids{{"my_holy_site", 3}};
    std::map<std::string, std::int32_t> modifier_ids{{"local_missionary_strength", 40}};
    std::set<std::pair<std::int32_t, std::int32_t>> flags{{2, 3}};
    std::map<std::pair<std::int32_t, std::string>, std::int64_t> variables{{{3, "my_faith"}, 5000}, {{4, "my_faith"}, 4999}};
    std::map<std::pair<std::int32_t, std::int32_t>, std::int64_t> modifiers{{{5, 40}, 20}};
    int flag_lookups = 0;

    std::int32_t province_limit() override { return limit; }
    std::optional<std::int32_t> flag_id(const std::string& name) override {
        ++flag_lookups;
        const auto it = flag_ids.find(name);
        return it == flag_ids.end() ? std::nullopt : std::optional<std::int32_t>(it->second);
    }
    std::optional<std::int32_t> modifier_id(const std::string& key) override {
        const auto it = modifier_ids.find(key);
        return it == modifier_ids.end() ? std::nullopt : std::optional<std::int32_t>(it->second);
    }
    bool has_flag(std::int32_t province, std::int32_t flag) override { return flags.contains({province, flag}); }
    std::int64_t variable(std::int32_t province, const std::string& name) override {
        const auto it = variables.find({province, name});
        return it == variables.end() ? 0 : it->second;
    }
    std::int64_t modifier(std::int32_t province, std::int32_t id) override {
        const auto it = modifiers.find({province, id});
        return it == modifiers.end() ? 0 : it->second;
    }

    // Owners: 1 and 2 are RUS's, 3 is POL's, 4 and 5 have none.
    std::map<std::int32_t, std::string> owners{{1, "RUS"}, {2, "RUS"}, {3, "POL"}};
    std::set<std::pair<std::string, std::int32_t>> country_flags{{"POL", 3}};
    std::map<std::pair<std::string, std::string>, std::int64_t> country_variables{{{"RUS", "my_faith"}, 2500}};
    std::map<std::pair<std::string, std::int32_t>, std::int64_t> country_modifiers{{{"RUS", 40}, 500}};
    int country_reads = 0;

    std::string owner(std::int32_t province) override {
        const auto it = owners.find(province);
        return it == owners.end() ? std::string() : it->second;
    }
    bool country_has_flag(const std::string& tag, std::int32_t flag) override {
        ++country_reads;
        return country_flags.contains({tag, flag});
    }
    std::int64_t country_variable(const std::string& tag, const std::string& name) override {
        ++country_reads;
        const auto it = country_variables.find({tag, name});
        return it == country_variables.end() ? 0 : it->second;
    }
    std::int64_t country_modifier(const std::string& tag, std::int32_t id) override {
        ++country_reads;
        const auto it = country_modifiers.find({tag, id});
        return it == country_modifiers.end() ? 0 : it->second;
    }

    // Development x 1000 by province; treasury x 1000 by tag (POL's not given).
    std::map<std::int32_t, std::int64_t> development{{1, 3000}, {2, 12000}, {3, 30000}};
    std::map<std::string, std::int64_t> treasury{{"RUS", 150000}};
    // Local autonomy x 1000 by province; this build has no devastation.
    std::map<std::int32_t, std::int64_t> autonomy{{1, 0}, {2, 25000}, {3, 75000}};
    std::optional<std::int64_t> province_value(std::int32_t province, files::Value value) override {
        if (value == files::Value::local_autonomy) {
            const auto it = autonomy.find(province);
            return it == autonomy.end() ? std::optional<std::int64_t>(0) : it->second;
        }
        if (value != files::Value::development) return std::nullopt;
        const auto it = development.find(province);
        return it == development.end() ? std::optional<std::int64_t>(0) : it->second;
    }
    std::optional<std::int64_t> country_value(const std::string& tag, files::Value value) override {
        ++country_reads;
        if (value != files::Value::treasury) return std::nullopt;
        const auto it = treasury.find(tag);
        return it == treasury.end() ? std::nullopt : std::optional<std::int64_t>(it->second);
    }

    // Controllers: SWE holds RUS's province 2; cores: RUS on 1, POL and RUS on 3.
    // POL is RUS's subject.
    std::map<std::int32_t, std::string> controllers{{1, "RUS"}, {2, "SWE"}, {3, "POL"}};
    std::set<std::pair<std::int32_t, std::string>> cores{{1, "RUS"}, {3, "POL"}, {3, "RUS"}};
    std::map<std::string, std::string> overlords{{"POL", "RUS"}};
    std::string controller(std::int32_t province) override {
        const auto it = controllers.find(province);
        return it == controllers.end() ? std::string() : it->second;
    }
    bool has_core(std::int32_t province, const std::string& tag) override { return cores.contains({province, tag}); }
    bool overlords_known = true;  // false: an anvil-game that cannot say
    std::optional<std::string> overlord(const std::string& tag) override {
        ++country_reads;
        if (!overlords_known) return std::nullopt;
        const auto it = overlords.find(tag);
        return it == overlords.end() ? std::string() : it->second;
    }
    // Capitals: RUS province 1, POL none.
    std::map<std::string, std::int32_t> capitals{{"RUS", 1}};
    std::int32_t capital(const std::string& tag) override {
        ++country_reads;
        const auto it = capitals.find(tag);
        return it == capitals.end() ? 0 : it->second;
    }
    // 1 and 3 in the Empire, 1 and 2 cities, 2 and 4 coastal land, 1 and 3 in a state; 5 not land;
    // 6 unreadable.
    std::set<std::int32_t> empire{1, 3}, cities{1, 2}, coasts{2, 4}, states{1, 3};
    std::optional<bool> province_is(std::int32_t province, files::Test test) override {
        if (province == 6) return std::nullopt;
        if (test == files::Test::coastal) {
            if (province == 5) return std::nullopt;
            return coasts.contains(province);
        }
        return (test == files::Test::hre ? empire : test == files::Test::state ? states : cities).contains(province);
    }

    // Religion and culture: 1 and 2 orthodox (2 Swedish), 3 catholic and
    // polish, 4 and 5 none; RUS orthodox and russian, POL catholic and polish.
    // Trade goods: 1 grain, 3 fur.
    std::map<std::pair<std::int32_t, files::Test>, std::string> province_keys{
        {{1, files::Test::religion}, "orthodox"}, {{2, files::Test::religion}, "orthodox"},
        {{3, files::Test::religion}, "catholic"}, {{1, files::Test::culture}, "russian"},
        {{2, files::Test::culture}, "swedish"},   {{3, files::Test::culture}, "polish"},
        {{1, files::Test::trade_goods}, "grain"}, {{3, files::Test::trade_goods}, "fur"},
        {{1, files::Test::religion_group}, "christian"}, {{2, files::Test::religion_group}, "christian"},
        {{3, files::Test::religion_group}, "christian"}, {{1, files::Test::culture_group}, "east_slavic"},
        {{2, files::Test::culture_group}, "scandinavian"}, {{3, files::Test::culture_group}, "west_slavic"},
        {{1, files::Test::area}, "moskva_area"}, {{1, files::Test::region}, "russia_region"},
        {{2, files::Test::region}, "scandinavia_region"}, {{3, files::Test::region}, "poland_region"},
        {{1, files::Test::continent}, "europe"}, {{2, files::Test::continent}, "europe"},
        {{3, files::Test::continent}, "europe"}, {{1, files::Test::superregion}, "eastern_europe_superregion"},
        {{1, files::Test::trade_node}, "novgorod"}, {{3, files::Test::trade_node}, "krakow"}};
    std::map<std::pair<std::string, files::Test>, std::string> country_keys{
        {{"RUS", files::Test::religion}, "orthodox"}, {{"RUS", files::Test::culture}, "russian"},
        {{"POL", files::Test::religion}, "catholic"}, {{"POL", files::Test::culture}, "polish"},
        {{"RUS", files::Test::culture_group}, "east_slavic"}, {{"POL", files::Test::culture_group}, "west_slavic"},
        {{"RUS", files::Test::religion_group}, "christian"}, {{"POL", files::Test::religion_group}, "christian"}};
    std::string province_key(std::int32_t province, files::Test test) override {
        const auto it = province_keys.find({province, test});
        return it == province_keys.end() ? std::string() : it->second;
    }
    std::string country_key(const std::string& tag, files::Test test) override {
        ++country_reads;
        const auto it = country_keys.find({tag, test});
        return it == country_keys.end() ? std::string() : it->second;
    }
};

constexpr const char* kHolySites = R"(# The example of docs/spec/map-mode-files-v1.md
holy_sites = {
    label = "Holy sites"
    description = "Where the faith is strong"
    tab = 4
    sprite = GFX_mapmode_religion
    rule = { flag = my_holy_site color = { 224 176 0 } label = "Holy site" }
    rule = { variable = my_faith at_least = 5 color = { 128 64 192 } label = "Faith 5 or more" }
    rule = { modifier = local_missionary_strength above = 0.01 color = { 64 128 255 } label = "Missionaries" }
}
)";

std::vector<std::string> parse_problems(std::string_view text, std::vector<files::FileMode>* modes = nullptr) {
    std::vector<files::FileMode> out;
    std::vector<std::string> problems;
    files::parse(text, "mod/anvil/map_modes/t.txt", out, problems);
    if (modes != nullptr) *modes = std::move(out);
    return problems;
}

void file_tests() {
    run("map mode files: a mode and its rules are read from the game's script syntax", [] {
        std::vector<files::FileMode> modes;
        CHECK(parse_problems(kHolySites, &modes).empty() && modes.size() == 1);
        const files::FileMode& m = modes[0];
        CHECK(m.name == "holy_sites" && m.label == "Holy sites" && m.description == "Where the faith is strong");
        CHECK(m.tab == 4 && m.sprite == "GFX_mapmode_religion" && m.legend && m.line == 2);
        CHECK(m.source == "mod/anvil/map_modes/t.txt" && m.rules.size() == 3);
        CHECK(m.rules[0].test == files::Test::flag && m.rules[0].name == "my_holy_site" && m.rules[0].color == 0xE0B000);
        CHECK(m.rules[1].test == files::Test::variable && m.rules[1].at_least == 5000 && !m.rules[1].above);
        CHECK(m.rules[2].test == files::Test::modifier && m.rules[2].above == 10 && m.rules[2].line == 9);

        // Words for texts, a mode without a legend, a catch-all rule.
        CHECK(parse_problems("plain={label=Plain legend=no rule={always=yes color={1 2 3} label=All}}", &modes).empty());
        CHECK(modes.size() == 1 && modes[0].label == "Plain" && !modes[0].legend && modes[0].tab == 0);
        CHECK(modes[0].rules[0].test == files::Test::always && modes[0].rules[0].color == 0x010203);
    });

    run("map mode files: Windows-1252 text is read as the game's script files are", [] {
        std::vector<files::FileMode> modes;
        CHECK(parse_problems("k={label=\"Krak\xF3w \x80\" rule={always=yes color={0 0 0} label=x}}", &modes).empty());
        CHECK(modes.size() == 1 && modes[0].label == "Krak\xC3\xB3w \xE2\x82\xAC");
        CHECK(parse_problems("\xEF\xBB\xBFk={label=\"Krak\xC3\xB3w\" rule={always=yes color={0 0 0} label=x}}", &modes).empty());
        CHECK(modes.size() == 1 && modes[0].label == "Krak\xC3\xB3w");
    });

    run("map mode files: a fault leaves out its mode and names the line; the next mode is read", [] {
        std::vector<files::FileMode> modes;
        const auto one = [&](std::string_view text) {
            const std::vector<std::string> p = parse_problems(text, &modes);
            return p.size() == 1 && modes.empty() ? p[0] : std::string("(") + std::to_string(p.size()) + ")";
        };
        CHECK(one("m={label=M}") == "mod/anvil/map_modes/t.txt:1: m has no rule");
        CHECK(one("m={rule={always=yes color={0 0 0} label=x}}").ends_with("m needs a label of 1 to 64 bytes"));
        CHECK(one("Mode={label=M rule={always=yes color={0 0 0} label=x}}").find("[a-z0-9_-]") != std::string::npos);
        CHECK(one("m={label=M\n tab=5 rule={always=yes color={0 0 0} label=x}}") == "mod/anvil/map_modes/t.txt:2: tab is 1, 2, 3 or 4");
        CHECK(one("m={label=M colour=1 rule={always=yes color={0 0 0} label=x}}").ends_with("a mode has no key colour"));
        CHECK(one("m={label=M label=N rule={always=yes color={0 0 0} label=x}}").ends_with("label is given twice"));
        CHECK(one("m={label=M sprite=\"GFX x\" rule={always=yes color={0 0 0} label=x}}").find("sprite") != std::string::npos);
        CHECK(one("m={label=M rule={color={0 0 0} label=x}}").ends_with("one of always, flag, variable, modifier, value, owned_by, controlled_by, core, occupied, subject, "
                                                                                          "subject_of, religion, religion_group, culture, culture_group, trade_goods, area, region, superregion, continent, trade_node, capital, hre, city, coastal or state"));
        CHECK(one("m={label=M rule={flag=a variable=b color={0 0 0} label=x}}").ends_with("one of always, flag, variable, modifier, value, owned_by, controlled_by, core, occupied, subject, "
                                                                                          "subject_of, religion, religion_group, culture, culture_group, trade_goods, area, region, superregion, continent, trade_node, capital, hre, city, coastal or state"));
        CHECK(one("m={label=M rule={always=yes label=x}}").ends_with("a rule needs color = { r g b }"));
        CHECK(one("m={label=M rule={always=yes color={0 0 0}}}").ends_with("a rule needs a label of 1 to 64 bytes"));
        CHECK(one("m={label=M rule={always=yes color={0 0 256} label=x}}").find("0 to 255") != std::string::npos);
        CHECK(one("m={label=M rule={always=yes color={0 0} label=x}}").find("0 to 255") != std::string::npos);
        CHECK(one("m={label=M rule={flag=f at_least=1 color={0 0 0} label=x}}").ends_with("go with a variable, a modifier or a value"));
        CHECK(one("m={label=M rule={variable=v at_least=1.2345 color={0 0 0} label=x}}").find("three decimals") != std::string::npos);
        CHECK(one("m={label=M rule={variable=v at_least=x color={0 0 0} label=x}}").find("three decimals") != std::string::npos);
        CHECK(one("m={label=M rule={variable=v at_least=1 at_least=2 color={0 0 0} label=x}}").ends_with("at_least is given twice"));
        CHECK(one("m={label=M rule={variable >= 5 color={0 0 0} label=x}}").find("at_least") != std::string::npos);
        CHECK(one("m={label=\"M rule={always=yes color={0 0 0} label=x}}").ends_with("a quoted text has no end"));
        CHECK(one("m={label=M rule={always=no color={0 0 0} label=x}}").find("never passes") != std::string::npos);

        // After a fault the parser goes on behind its block.
        const std::vector<std::string> p = parse_problems(
            "bad = { label = B rule = { nope = { deep = 1 } } }\n"
            "good = { label = G rule = { always = yes color = { 0 0 0 } label = x } }",
            &modes);
        CHECK(p.size() == 1 && p[0] == "mod/anvil/map_modes/t.txt:1: a rule has no key nope");
        CHECK(modes.size() == 1 && modes[0].name == "good");

        // A fault the lexer finds outside any block ends: the parser goes on behind it.
        modes.clear();
        const std::vector<std::string> outside = parse_problems(
            "a = { label = A rule = { always = yes color = { 0 0 0 } label = x } } >\n"
            "\"open\n"
            "b = { label = B rule = { always = yes color = { 0 0 0 } label = x } }",
            &modes);
        CHECK(outside.size() == 2 && outside[0].find("at_least") != std::string::npos &&
              outside[1].ends_with(":2: a quoted text has no end"));
        CHECK(modes.size() == 2 && modes[0].name == "a" && modes[1].name == "b");

        // An unclosed text met while recovering ends at its line, which still counts.
        const std::vector<std::string> lines = parse_problems("a = { nope = 1 label = \"open\n}\nb = { tab = 9 }");
        CHECK(lines.size() == 2 && lines[0].starts_with("mod/anvil/map_modes/t.txt:1: ") &&
              lines[1].starts_with("mod/anvil/map_modes/t.txt:3: "));

        // A quoted text over two lines: one fault, and its closing quote on the
        // next line does not hide the } behind it, so the next mode is read.
        modes.clear();
        const std::vector<std::string> wrapped = parse_problems(
            "a = { label = A description = \"two\nlines\" rule = { always = yes color = { 0 0 0 } label = x } }\n"
            "b = { label = B rule = { always = yes color = { 0 0 0 } label = x } }",
            &modes);
        CHECK(wrapped.size() == 1 && wrapped[0] == "mod/anvil/map_modes/t.txt:1: a quoted text has no end");
        CHECK(modes.size() == 1 && modes[0].name == "b");

        // A text whose closing quote was forgotten, with whole texts on the next
        // line: the first quote met there opens one, so the } behind it still
        // closes the block and the next mode is read.
        modes.clear();
        const std::vector<std::string> forgotten = parse_problems(
            "a = { label = \"A\n"
            "  rule = { always = yes color = { 0 0 0 } label = \"x\" } }\n"
            "b = { label = B rule = { always = yes color = { 0 0 0 } label = x } }",
            &modes);
        CHECK(forgotten.size() == 1 && forgotten[0] == "mod/anvil/map_modes/t.txt:1: a quoted text has no end");
        CHECK(modes.size() == 1 && modes[0].name == "b");

        // A file of stray braces: twenty lines and a count, not a line per byte.
        const std::vector<std::string> flood = parse_problems(std::string(1000, '}'));
        CHECK(flood.size() == files::kFaultsShown + 1 &&
              flood[0] == "mod/anvil/map_modes/t.txt:1: expected the name of a mode" &&
              flood.back() == "mod/anvil/map_modes/t.txt: 980 more faults");
    });

    run("map mode files: numbers are x 1000 and the bounds hold together", [] {
        std::vector<files::FileMode> modes;
        CHECK(parse_problems("m={label=M rule={variable=v at_least=-1.5 below=2 color={0 0 0} label=x}}", &modes).empty());
        const files::Rule& r = modes[0].rules[0];
        CHECK(r.at_least == -1500 && r.below == 2000);
        CHECK(files::in_bounds(r, -1500) && files::in_bounds(r, 1999) && !files::in_bounds(r, 2000) && !files::in_bounds(r, -1501));
        files::Rule plain;
        CHECK(files::in_bounds(plain, 1) && files::in_bounds(plain, -1) && !files::in_bounds(plain, 0));  // not 0
        files::Rule at_most;
        at_most.at_most = 0;
        at_most.above = -1000;
        CHECK(files::in_bounds(at_most, 0) && files::in_bounds(at_most, -999) && !files::in_bounds(at_most, -1000));
    });

    run("map mode files: the first rule a province passes gives its colour", [] {
        std::vector<files::FileMode> modes;
        CHECK(parse_problems(kHolySites, &modes).empty());
        TableWorld world;
        files::Evaluator e(modes[0], world);
        CHECK(e.unknown().empty());
        const std::vector<std::uint32_t> colors = e.colors();
        CHECK(colors.size() == 6 && colors[0] == ANVIL_MAP_NO_COLOR);
        CHECK(colors[1] == ANVIL_MAP_NO_COLOR);       // nothing passes: the political colour
        CHECK(colors[2] == 0xE0B000);                 // the flag
        CHECK(colors[3] == 0x8040C0);                 // my_faith 5
        CHECK(colors[4] == ANVIL_MAP_NO_COLOR);       // my_faith 4.999
        CHECK(colors[5] == 0x4080FF);                 // the modifier above 0.01
        world.variables[{2, "my_faith"}] = 9000;      // the flag's rule comes first
        CHECK(e.colors()[2] == 0xE0B000 && e.match(2) == &modes[0].rules[0] && e.match(1) == nullptr);
        CHECK(world.flag_lookups == 1);               // ids once per computation

        // A flag no effect has set yet, and a modifier the game lacks.
        world.flag_ids.clear();
        world.modifier_ids.clear();
        files::Evaluator later(modes[0], world);
        CHECK(later.unknown().size() == 1 && later.unknown()[0] == "local_missionary_strength");
        // Province 2's flag is not seen; its my_faith 9 passes the next rule.
        CHECK(later.match(2) == &modes[0].rules[1] && later.match(5) == nullptr && later.match(3) == &modes[0].rules[1]);
    });

    run("map mode files: owner = yes reads the province's owner, once per country (anvil#580)", [] {
        std::vector<files::FileMode> modes;
        CHECK(parse_problems("m={label=M"
                             " rule={flag=my_holy_site owner=yes color={1 0 0} label=Flag}"
                             " rule={variable=my_faith owner=yes at_least=2 color={2 0 0} label=Faith}"
                             " rule={always=yes owner=yes color={3 0 0} label=Owned}}",
                             &modes)
                  .empty());
        CHECK(modes.size() == 1 && modes[0].rules[0].owner && modes[0].rules[2].owner);
        TableWorld world;
        files::Evaluator e(modes[0], world);
        const std::vector<std::uint32_t> colors = e.colors();
        CHECK(colors[1] == 0x020000 && colors[2] == 0x020000);  // RUS's my_faith 2.5, not the province's flag
        CHECK(colors[3] == 0x010000);                           // POL's flag
        CHECK(colors[4] == ANVIL_MAP_NO_COLOR && colors[5] == ANVIL_MAP_NO_COLOR);  // no owner
        // RUS: flag and variable; POL: flag. Province 2 read nothing of RUS again.
        CHECK(world.country_reads == 3);
        std::int64_t value = 0;
        CHECK(e.match(1, &value) == &modes[0].rules[1] && value == 2500);
        CHECK(world.country_reads == 3);  // the tooltip reads what the colours read

        // owner = no is the province itself; owner is given once.
        CHECK(parse_problems("m={label=M rule={flag=my_holy_site owner=no color={1 0 0} label=F}}", &modes).empty());
        files::Evaluator own(modes[0], world);
        CHECK(own.match(2) == &modes[0].rules[0] && own.match(3) == nullptr);
        CHECK(parse_problems("m={label=M rule={always=yes owner=yes owner=no color={0 0 0} label=x}}").size() == 1);
    });

    run("map mode files: owned_by, core, occupied and subject", [] {
        std::vector<files::FileMode> modes;
        CHECK(parse_problems("m={label=M"
                             " rule={occupied=yes color={1 0 0} label=Occupied}"
                             " rule={subject=yes owner=yes color={2 0 0} label=Subject}"
                             " rule={owned_by=rus color={3 0 0} label=Russia}}",
                             &modes)
                  .empty());
        CHECK(modes.size() == 1 && modes[0].rules[2].name == "RUS" && modes[0].rules[1].owner);
        TableWorld world;
        files::Evaluator e(modes[0], world);
        std::vector<std::uint32_t> colors = e.colors();
        CHECK(colors[1] == 0x030000);  // RUS's, held by RUS
        CHECK(colors[2] == 0x010000);  // RUS's, held by SWE
        CHECK(colors[3] == 0x020000);  // POL's, RUS's subject
        CHECK(colors[4] == ANVIL_MAP_NO_COLOR && colors[5] == ANVIL_MAP_NO_COLOR);  // no owner
        std::int64_t value = 0;
        CHECK(e.match(2, &value) == &modes[0].rules[0] && value == 1);

        // core = no: owned without the owner's core; = yes the other way.
        CHECK(parse_problems("m={label=M rule={core=no color={1 0 0} label=Uncored}"
                             " rule={core=yes color={2 0 0} label=Core}}",
                             &modes)
                  .empty());
        files::Evaluator c(modes[0], world);
        colors = c.colors();
        CHECK(colors[1] == 0x020000 && colors[2] == 0x010000 && colors[3] == 0x020000);
        CHECK(colors[4] == ANVIL_MAP_NO_COLOR);  // unowned: neither
        // occupied = no and subject = no.
        CHECK(parse_problems("m={label=M rule={occupied=no color={1 0 0} label=Held}}", &modes).empty());
        files::Evaluator held(modes[0], world);
        CHECK(held.match(1) != nullptr && held.match(2) == nullptr && held.match(4) == nullptr);
        CHECK(parse_problems("m={label=M rule={subject=no owner=yes color={1 0 0} label=Free}}", &modes).empty());
        files::Evaluator independent(modes[0], world);
        CHECK(independent.match(1) != nullptr && independent.match(3) == nullptr && independent.match(5) == nullptr);
        // An anvil-game that cannot say whose subject a country is: neither yes nor no passes.
        TableWorld older;
        older.overlords_known = false;
        CHECK(parse_problems("m={label=M rule={subject=no owner=yes color={1 0 0} label=Free}"
                             " rule={subject=yes owner=yes color={2 0 0} label=Subject}"
                             " rule={subject_of=RUS owner=yes color={3 0 0} label=Vassal}}",
                             &modes)
                  .empty());
        files::Evaluator unknown(modes[0], older);
        CHECK(unknown.match(1) == nullptr && unknown.match(3) == nullptr);

        // Faults.
        CHECK(parse_problems("m={label=M rule={owned_by=RUSS color={0 0 0} label=x}}")[0].ends_with(
            "owned_by needs a country tag of three letters or digits: RUSS"));
        CHECK(parse_problems("m={label=M rule={subject=yes color={0 0 0} label=x}}")[0].ends_with(
            "subject is the owner's: write owner = yes"));
        CHECK(parse_problems("m={label=M rule={core=yes owner=yes color={0 0 0} label=x}}")[0].ends_with(
            "core is the province's: leave out owner = yes"));
        CHECK(parse_problems("m={label=M rule={occupied=yes at_least=1 color={0 0 0} label=x}}")[0].ends_with(
            "at_least, at_most, above and below go with a variable, a modifier or a value"));
        CHECK(parse_problems("m={label=M rule={owned_by=RUS scale={0 1} high_color={0 0 0} color={0 0 0} "
                             "label=x}}")[0]
                  .ends_with("a scale goes with a variable, a modifier or a value"));
        CHECK(parse_problems("m={label=M rule={core=maybe color={0 0 0} label=x}}").size() == 1);
    });

    run("map mode files: controlled_by, core = TAG and subject_of", [] {
        std::vector<files::FileMode> modes;
        CHECK(parse_problems("m={label=M"
                             " rule={controlled_by=swe color={1 0 0} label=Swedish}"
                             " rule={subject_of=RUS owner=yes color={2 0 0} label=Vassal}"
                             " rule={core=RUS color={3 0 0} label=Claimed}}",
                             &modes)
                  .empty());
        CHECK(modes.size() == 1 && modes[0].rules[0].name == "SWE" && modes[0].rules[2].test == files::Test::core_of);
        TableWorld world;
        files::Evaluator e(modes[0], world);
        const std::vector<std::uint32_t> colors = e.colors();
        CHECK(colors[1] == 0x030000);  // RUS's core
        CHECK(colors[2] == 0x010000);  // held by SWE
        CHECK(colors[3] == 0x020000);  // POL's, RUS's subject (RUS's core too: the first rule wins)
        CHECK(colors[4] == ANVIL_MAP_NO_COLOR && colors[5] == ANVIL_MAP_NO_COLOR);

        // core = TAG needs no owner and names any country; subject_of another overlord passes nothing.
        CHECK(parse_problems("m={label=M rule={core=pol color={1 0 0} label=P}}"
                             " n={label=N rule={subject_of=SWE owner=yes color={1 0 0} label=S}}",
                             &modes)
                  .empty());
        files::Evaluator pol(modes[0], world);
        CHECK(pol.match(3) != nullptr && pol.match(1) == nullptr && pol.match(4) == nullptr);
        files::Evaluator swe(modes[1], world);
        CHECK(swe.match(1) == nullptr && swe.match(3) == nullptr);

        // Faults.
        CHECK(parse_problems("m={label=M rule={controlled_by=SWED color={0 0 0} label=x}}")[0].ends_with(
            "controlled_by needs a country tag of three letters or digits: SWED"));
        CHECK(parse_problems("m={label=M rule={core=maybe color={0 0 0} label=x}}")[0].ends_with(
            "core is yes, no or a country tag of three letters or digits: maybe"));
        CHECK(parse_problems("m={label=M rule={subject_of=RUS color={0 0 0} label=x}}")[0].ends_with(
            "subject_of is the owner's: write owner = yes"));
        CHECK(parse_problems("m={label=M rule={core=RUS owner=yes color={0 0 0} label=x}}")[0].ends_with(
            "core is the province's: leave out owner = yes"));
        CHECK(parse_problems("m={label=M rule={owned_by=RUS owner=yes color={0 0 0} label=x}}")[0].ends_with(
            "owned_by is the province's: leave out owner = yes"));
        CHECK(parse_problems("m={label=M rule={controlled_by=SWE above=1 color={0 0 0} label=x}}")[0].ends_with(
            "at_least, at_most, above and below go with a variable, a modifier or a value"));
    });

    run("map mode files: religion, culture and trade_goods", [] {
        std::vector<files::FileMode> modes;
        CHECK(parse_problems("m={label=M"
                             " rule={culture=swedish color={1 0 0} label=Swedish}"
                             " rule={religion=orthodox color={2 0 0} label=Orthodox}"
                             " rule={trade_goods=fur color={3 0 0} label=Fur}}"
                             " n={label=N"
                             " rule={culture=russian owner=yes color={4 0 0} label=Russian}"
                             " rule={religion=catholic owner=yes color={5 0 0} label=Catholic}}",
                             &modes)
                  .empty());
        CHECK(modes.size() == 2 && modes[0].rules[0].test == files::Test::culture &&
              modes[0].rules[0].name == "swedish" && modes[0].rules[2].test == files::Test::trade_goods);
        TableWorld world;
        files::Evaluator e(modes[0], world);
        std::vector<std::uint32_t> colors = e.colors();
        CHECK(colors[1] == 0x020000);  // orthodox
        CHECK(colors[2] == 0x010000);  // Swedish culture (orthodox too: the first rule wins)
        CHECK(colors[3] == 0x030000);  // fur
        CHECK(colors[4] == ANVIL_MAP_NO_COLOR && colors[5] == ANVIL_MAP_NO_COLOR);
        // owner = yes: the owner's primary culture and state religion, read once per owner.
        world.country_reads = 0;
        files::Evaluator owners(modes[1], world);
        colors = owners.colors();
        CHECK(colors[1] == 0x040000 && colors[2] == 0x040000);  // RUS's: russian, whatever the province's culture
        CHECK(colors[3] == 0x050000);                           // POL's: catholic
        CHECK(colors[4] == ANVIL_MAP_NO_COLOR && colors[5] == ANVIL_MAP_NO_COLOR);
        CHECK(world.country_reads == 3);
        // A key is compared as written.
        CHECK(parse_problems("m={label=M rule={religion=Orthodox color={1 0 0} label=O}}", &modes).empty());
        files::Evaluator upper(modes[0], world);
        CHECK(upper.match(1) == nullptr);

        // Faults.
        CHECK(parse_problems("m={label=M rule={trade_goods=grain owner=yes color={0 0 0} label=x}}")[0].ends_with(
            "trade_goods is the province's: leave out owner = yes"));
        CHECK(parse_problems("m={label=M rule={religion=\"two words\" color={0 0 0} label=x}}")[0].ends_with(
            "religion needs a script key as the game's files write it (orthodox, russian, grain, "
            "scandinavia_region): two words"));
        CHECK(parse_problems("m={label=M rule={culture=russian above=1 color={0 0 0} label=x}}")[0].ends_with(
            "at_least, at_most, above and below go with a variable, a modifier or a value"));
        CHECK(parse_problems("m={label=M rule={culture=russian religion=orthodox color={0 0 0} label=x}}").size() ==
              1);

        // Their groups, the province's and with owner = yes the owner's.
        CHECK(parse_problems("m={label=M"
                             " rule={culture_group=scandinavian color={1 0 0} label=Scandinavian}"
                             " rule={culture_group=west_slavic owner=yes color={2 0 0} label=West}"
                             " rule={religion_group=christian color={3 0 0} label=Christian}}",
                             &modes)
                  .empty());
        CHECK(modes[0].rules[0].test == files::Test::culture_group &&
              modes[0].rules[2].test == files::Test::religion_group);
        files::Evaluator groups(modes[0], world);
        colors = groups.colors();
        CHECK(colors[2] == 0x010000);                           // scandinavian
        CHECK(colors[3] == 0x020000);                           // POL's west_slavic
        CHECK(colors[1] == 0x030000);                           // christian, east_slavic under RUS
        CHECK(colors[4] == ANVIL_MAP_NO_COLOR && colors[5] == ANVIL_MAP_NO_COLOR);
        CHECK(parse_problems("m={label=M rule={religion_group=\"\" color={0 0 0} label=x}}")[0].ends_with(
            "religion_group needs a script key as the game's files write it (orthodox, russian, grain, "
            "scandinavia_region): "));

        // Places: the province's area, region, superregion and continent.
        CHECK(parse_problems("m={label=M"
                             " rule={area=moskva_area color={1 0 0} label=Moskva}"
                             " rule={region=scandinavia_region color={2 0 0} label=Scandinavia}"
                             " rule={superregion=eastern_europe_superregion color={3 0 0} label=East}"
                             " rule={continent=europe color={4 0 0} label=Europe}}",
                             &modes)
                  .empty());
        CHECK(modes[0].rules[0].test == files::Test::area && modes[0].rules[1].test == files::Test::region &&
              modes[0].rules[2].test == files::Test::superregion && modes[0].rules[3].test == files::Test::continent);
        files::Evaluator places(modes[0], world);
        colors = places.colors();
        CHECK(colors[1] == 0x010000 && colors[2] == 0x020000 && colors[3] == 0x040000);
        CHECK(colors[4] == ANVIL_MAP_NO_COLOR);
        CHECK(parse_problems("m={label=M rule={region=x owner=yes color={0 0 0} label=x}}")[0].ends_with(
            "region is the province's: leave out owner = yes"));
        CHECK(parse_problems("m={label=M rule={continent=europe owner=yes color={0 0 0} label=x}}")[0].ends_with(
            "continent is the province's: leave out owner = yes"));
    });

    run("map mode files: capital = yes and no", [] {
        std::vector<files::FileMode> modes;
        CHECK(parse_problems("m={label=M rule={capital=yes color={1 0 0} label=Capital}"
                             " rule={capital=no color={2 0 0} label=Other}}",
                             &modes)
                  .empty());
        CHECK(modes[0].rules[0].test == files::Test::capital && modes[0].rules[0].yes &&
              !modes[0].rules[1].yes);
        TableWorld world;
        world.country_reads = 0;
        files::Evaluator e(modes[0], world);
        const std::vector<std::uint32_t> colors = e.colors();
        CHECK(colors[1] == 0x010000);  // RUS's capital
        CHECK(colors[2] == 0x020000);  // RUS's, not its capital
        CHECK(colors[3] == 0x020000);  // POL has none: every province of it is another
        CHECK(colors[4] == ANVIL_MAP_NO_COLOR && colors[5] == ANVIL_MAP_NO_COLOR);  // no owner
        CHECK(world.country_reads == 4);  // once per owner and rule
        CHECK(parse_problems("m={label=M rule={capital=yes owner=yes color={0 0 0} label=x}}")[0].ends_with(
            "capital is the province's: leave out owner = yes"));
        CHECK(parse_problems("m={label=M rule={capital=RUS color={0 0 0} label=x}}").size() == 1);
    });

    run("map mode files: hre, city and trade_node", [] {
        std::vector<files::FileMode> modes;
        CHECK(parse_problems("m={label=M rule={hre=yes color={1 0 0} label=Empire}"
                             " rule={city=no color={2 0 0} label=Colony}"
                             " rule={trade_node=novgorod color={3 0 0} label=Novgorod}}",
                             &modes)
                  .empty());
        CHECK(modes[0].rules[0].test == files::Test::hre && modes[0].rules[1].test == files::Test::city &&
              !modes[0].rules[1].yes && modes[0].rules[2].test == files::Test::trade_node &&
              modes[0].rules[2].name == "novgorod");
        TableWorld world;
        world.limit = 7;
        world.owners[6] = "RUS";
        files::Evaluator e(modes[0], world);
        const std::vector<std::uint32_t> colors = e.colors();
        CHECK(colors[1] == 0x010000 && colors[3] == 0x010000);  // in the Empire
        CHECK(colors[2] == ANVIL_MAP_NO_COLOR);  // RUS's city, outside the Empire and Novgorod's node
        CHECK(colors[4] == ANVIL_MAP_NO_COLOR && colors[5] == ANVIL_MAP_NO_COLOR);  // no owner
        CHECK(colors[6] == ANVIL_MAP_NO_COLOR);  // the game does not say, no node
        std::vector<files::FileMode> node;
        CHECK(parse_problems("m={label=M rule={trade_node=novgorod color={3 0 0} label=Novgorod}"
                             " rule={city=yes color={4 0 0} label=City}}",
                             &node)
                  .empty());
        files::Evaluator n(node[0], world);
        const std::vector<std::uint32_t> by_node = n.colors();
        CHECK(by_node[1] == 0x030000 && by_node[2] == 0x040000 && by_node[3] == ANVIL_MAP_NO_COLOR);
        CHECK(parse_problems("m={label=M rule={hre=yes owner=yes color={0 0 0} label=x}}")[0].ends_with(
            "hre is the province's: leave out owner = yes"));
        CHECK(parse_problems("m={label=M rule={trade_node=baltic_sea owner=yes color={0 0 0} label=x}}")[0].ends_with(
            "trade_node is the province's: leave out owner = yes"));
        CHECK(parse_problems("m={label=M rule={city=maybe color={0 0 0} label=x}}").size() == 1);
    });

    run("map mode files: coastal = yes and no", [] {
        std::vector<files::FileMode> modes;
        CHECK(parse_problems("m={label=M rule={coastal=yes color={1 0 0} label=Coast}"
                             " rule={coastal=no color={2 0 0} label=Inland}}",
                             &modes)
                  .empty());
        CHECK(modes[0].rules[0].test == files::Test::coastal && modes[0].rules[0].yes && !modes[0].rules[1].yes);
        TableWorld world;
        world.limit = 7;
        files::Evaluator e(modes[0], world);
        const std::vector<std::uint32_t> colors = e.colors();
        CHECK(colors[2] == 0x010000 && colors[4] == 0x010000);  // coastal, owned or not
        CHECK(colors[1] == 0x020000 && colors[3] == 0x020000);  // inland
        CHECK(colors[5] == ANVIL_MAP_NO_COLOR && colors[6] == ANVIL_MAP_NO_COLOR);  // not land; the game does not say
        CHECK(parse_problems("m={label=M rule={coastal=yes owner=yes color={0 0 0} label=x}}")[0].ends_with(
            "coastal is the province's: leave out owner = yes"));
    });

    run("map mode files: state = yes and no", [] {
        std::vector<files::FileMode> modes;
        CHECK(parse_problems("m={label=M rule={state=yes color={1 0 0} label=State}"
                             " rule={state=no color={2 0 0} label=Territory}}",
                             &modes)
                  .empty());
        CHECK(modes[0].rules[0].test == files::Test::state && modes[0].rules[0].yes && !modes[0].rules[1].yes);
        TableWorld world;
        world.limit = 7;
        world.owners[6] = "RUS";
        files::Evaluator e(modes[0], world);
        const std::vector<std::uint32_t> colors = e.colors();
        CHECK(colors[1] == 0x010000 && colors[3] == 0x010000);  // in a state
        CHECK(colors[2] == 0x020000);                           // RUS's territory
        CHECK(colors[4] == ANVIL_MAP_NO_COLOR && colors[5] == ANVIL_MAP_NO_COLOR);  // no owner
        CHECK(colors[6] == ANVIL_MAP_NO_COLOR);                 // the game does not say
        CHECK(parse_problems("m={label=M rule={state=yes owner=yes color={0 0 0} label=x}}")[0].ends_with(
            "state is the province's: leave out owner = yes"));
        CHECK(parse_problems("m={label=M rule={state=yes at_least=1 color={0 0 0} label=x}}").size() == 1);
    });

    run("map mode files: local_autonomy, devastation and trade_power values", [] {
        std::vector<files::FileMode> modes;
        CHECK(parse_problems("m={label=M rule={value=local_autonomy at_least=0 scale={0 100} color={0 200 0} "
                             "high_color={200 0 0} label=Autonomy}}",
                             &modes)
                  .empty());
        CHECK(modes[0].rules[0].value == files::Value::local_autonomy);
        TableWorld world;
        files::Evaluator e(modes[0], world);
        const std::vector<std::uint32_t> colors = e.colors();
        CHECK(colors[1] == 0x00C800 && colors[2] == 0x329600 && colors[3] == 0x963200);  // 0, 25 and 75 of 100
        std::int64_t value = 0;
        CHECK(e.match(3, &value) != nullptr && value == 75000);
        // A value the build does not have: the rule never passes.
        CHECK(parse_problems("m={label=M rule={value=devastation at_least=0 color={9 9 9} label=Devastated}}", &modes)
                  .empty());
        files::Evaluator none(modes[0], world);
        for (const std::uint32_t c : none.colors()) CHECK(c == ANVIL_MAP_NO_COLOR);
        CHECK(parse_problems("m={label=M rule={value=trade_power owner=yes color={0 0 0} label=x}}")[0].ends_with(
            "trade_power is the province's: leave out owner = yes"));
    });

    run("map mode files: a scale colours between its two ends (anvil#580)", [] {
        std::vector<files::FileMode> modes;
        CHECK(parse_problems("m={label=M rule={variable=v at_least=0 scale={0 10} color={0 100 200} "
                             "high_color={200 100 0} label=Faith}}",
                             &modes)
                  .empty());
        const files::Rule& r = modes[0].rules[0];
        CHECK(r.scaled() && r.low == 0 && r.high == 10000 && r.high_color == 0xC86400);
        CHECK(files::color_at(r, -5000) == 0x0064C8);  // below the low end
        CHECK(files::color_at(r, 0) == 0x0064C8);
        CHECK(files::color_at(r, 5000) == 0x646464);   // the middle
        CHECK(files::color_at(r, 2500) == 0x326496);   // 50, 100, 150
        CHECK(files::color_at(r, 10000) == 0xC86400 && files::color_at(r, 99000) == 0xC86400);
        files::Rule plain;
        plain.color = 0x123456;
        CHECK(files::color_at(plain, 7000) == 0x123456);

        TableWorld world;
        world.variables[{1, "v"}] = 5000;
        world.variables[{2, "v"}] = 0;
        world.variables[{3, "v"}] = -1;
        files::Evaluator e(modes[0], world);
        const std::vector<std::uint32_t> colors = e.colors();
        CHECK(colors[1] == 0x646464 && colors[2] == 0x0064C8);  // at_least = 0 lets 0 in
        CHECK(colors[3] == ANVIL_MAP_NO_COLOR);                 // -0.001 fails the bound

        // A scale on the owner's modifier: RUS has local_missionary_strength 0.5.
        CHECK(parse_problems("m={label=M rule={modifier=local_missionary_strength owner=yes scale={0 1} "
                             "color={0 0 0} high_color={100 200 250} label=Missions}}",
                             &modes)
                  .empty());
        files::Evaluator owned(modes[0], world);
        CHECK(owned.colors()[1] == 0x32647D);  // 50, 100, 125

        const auto one = [&](std::string_view text) {
            const std::vector<std::string> p = parse_problems(text);
            return p.size() == 1 ? p[0] : std::string("(") + std::to_string(p.size()) + ")";
        };
        CHECK(one("m={label=M rule={variable=v scale={0 1} color={0 0 0} label=x}}").ends_with("go together"));
        CHECK(one("m={label=M rule={variable=v high_color={0 0 0} color={0 0 0} label=x}}").ends_with("go together"));
        CHECK(one("m={label=M rule={variable=v scale={1 1} color={0 0 0} high_color={0 0 0} label=x}}").ends_with("low below high"));
        CHECK(one("m={label=M rule={variable=v scale={1} color={0 0 0} high_color={0 0 0} label=x}}").find("three decimals") != std::string::npos);
        CHECK(one("m={label=M rule={flag=f scale={0 1} color={0 0 0} high_color={0 0 0} label=x}}").ends_with("a scale goes with a variable, a modifier or a value"));
    });

    run("map mode files: value reads the game's own numbers of a province or its owner (anvil#580)", [] {
        std::vector<files::FileMode> modes;
        CHECK(parse_problems("m={label=M rule={value=development at_least=0 scale={0 30} color={0 0 0} "
                             "high_color={0 0 240} label=Development}}",
                             &modes)
                  .empty());
        CHECK(modes[0].rules[0].test == files::Test::value && modes[0].rules[0].value == files::Value::development);
        TableWorld world;
        files::Evaluator e(modes[0], world);
        std::vector<std::uint32_t> colors = e.colors();
        CHECK(colors[1] == 0x000018 && colors[2] == 0x000060 && colors[3] == 0x0000F0);  // 3, 12, 30 of 30
        CHECK(colors[4] == 0x000000);  // 0 passes at_least = 0
        std::int64_t value = 0;
        CHECK(e.match(2, &value) != nullptr && value == 12000);

        // The owner's treasury: RUS has 150, POL's is not given (the rule cannot pass).
        CHECK(parse_problems("m={label=M rule={value=treasury owner=yes at_least=100 color={9 9 9} label=Rich}}", &modes).empty());
        files::Evaluator rich(modes[0], world);
        colors = rich.colors();
        CHECK(colors[1] == 0x090909 && colors[2] == 0x090909 && colors[3] == ANVIL_MAP_NO_COLOR && colors[4] == ANVIL_MAP_NO_COLOR);

        CHECK(parse_problems("m={label=M rule={value=treasury color={0 0 0} label=x}}").size() == 1);
        CHECK(parse_problems("m={label=M rule={value=treasury color={0 0 0} label=x}}")[0].ends_with("treasury is the owner's: write owner = yes"));
        CHECK(parse_problems("m={label=M rule={value=development owner=yes color={0 0 0} label=x}}")[0].ends_with(
            "development is the province's: leave out owner = yes"));
        CHECK(parse_problems("m={label=M rule={value=gold color={0 0 0} label=x}}")[0].ends_with(": gold"));
        for (const char* v : {"base_tax", "base_production", "base_manpower", "development", "local_autonomy",
                              "devastation", "trade_power"}) {
            CHECK(parse_problems(std::string("m={label=M rule={value=") + v + " color={0 0 0} label=x}}").empty());
        }
        for (const char* v : {"treasury", "stability", "prestige", "manpower", "monthly_income", "provinces"}) {
            CHECK(parse_problems(std::string("m={label=M rule={value=") + v + " owner=yes color={0 0 0} label=x}}").empty());
        }
    });

    run("map mode files: a scale's legend gives both ends, its label cut to fit (anvil#580)", [] {
        std::vector<files::FileMode> modes;
        CHECK(parse_problems("m={label=M rule={variable=v scale={0 12.5} color={1 1 1} high_color={2 2 2} label=Faith} "
                             "rule={always=yes color={3 3 3} label=Rest}}",
                             &modes)
                  .empty());
        std::vector<files::LegendItem> legend = files::legend_of(modes[0]);
        CHECK(legend.size() == 3);
        if (legend.size() == 3) {
            CHECK(legend[0].color == 0x010101 && legend[0].label == "Faith 0");
            CHECK(legend[1].color == 0x020202 && legend[1].label == "Faith 12.5");
            CHECK(legend[2].label == "Rest");
        }
        // 31 two-byte letters (62 bytes): " 12.5" leaves room for 29 of them, not half of one.
        std::string label;
        for (int i = 0; i < 31; ++i) label += "\xC5\x82";
        CHECK(parse_problems("m={label=M rule={variable=v scale={0 12.5} color={1 1 1} high_color={2 2 2} label=\"" +
                                 label + "\"}}",
                             &modes)
                  .empty());
        legend = files::legend_of(modes[0]);
        CHECK(legend.size() == 2 && legend[1].label.size() == 58 + 5 && legend[1].label.ends_with("\xC5\x82 12.5"));
        CHECK(files::decimal(-1500) == "-1.5" && files::decimal(12500) == "12.5" && files::decimal(3000) == "3");
    });

    run("map mode files: a legend lists each colour and label once", [] {
        std::vector<files::FileMode> modes;
        CHECK(parse_problems("m={label=M rule={flag=a color={1 1 1} label=A} rule={flag=b color={1 1 1} label=A} "
                             "rule={flag=c color={2 2 2} label=A}}",
                             &modes)
                  .empty());
        const std::vector<files::LegendItem> legend = files::legend_of(modes[0]);
        CHECK(legend.size() == 2 && legend[0].color == 0x010101 && legend[1].color == 0x020202);
    });

    run("map mode files: the playset's mods give their files in load order", [] {
        CHECK(files::enabled_mods(R"({"enabled_mods":["mod/a.mod", "mod/ugc_2.mod"],"disabled_dlcs":[]})") ==
              std::vector<std::string>({"mod/a.mod", "mod/ugc_2.mod"}));
        CHECK(files::enabled_mods("{}").empty());
        CHECK(files::mod_value("name=\"A mod\"\npath = \"C:/mods/a\"\n", "path") == "C:/mods/a");
        CHECK(files::mod_value("replace_path=\"x\"\n", "path").empty());
        // As Anvil's loader reads a descriptor: a BOM, comments and escapes.
        CHECK(files::mod_value("\xEF\xBB\xBFpath=\"mod/a\"\n", "path") == "mod/a");
        CHECK(files::mod_value("# path=\"mod/old\"\npath=\"mod/new\"\n", "path") == "mod/new");
        // A # in a quoted text is no comment, and a key in a quoted text is none.
        CHECK(files::mod_value("name=\"Mod #2\" path=\"mod/x\"\n", "path") == "mod/x");
        CHECK(files::mod_value("name=\"a path=\\\"y\\\"\" path=\"mod/x\"\n", "path") == "mod/x");
        CHECK(files::mod_value("name=\"a path = 1\"\n", "path").empty());
        CHECK(files::mod_value("name=\"A \\\"quoted\\\" mod\"\n", "name") == "A \"quoted\" mod");
        CHECK(files::mod_value("path=\"C:\\\\mods\\\\a\"\n", "path") == "C:\\mods\\a");
        CHECK(files::mod_value("path=\"C:\\mods\\a\\\"\n", "path").empty());  // \" does not close it, as in the game
        // As the loader: a bare value, a quote in a comment after it, a quoted
        // text over two lines, the last of two keys, no space before a key, and
        // only the top level.
        CHECK(files::mod_value("path=mod/x\n", "path") == "mod/x");
        CHECK(files::mod_value("path=mod/x # was \"mod/old\"\n", "path") == "mod/x");
        CHECK(files::mod_value("name=\"My\nmod\" path=\"mod/x\"\n", "path") == "mod/x");
        CHECK(files::mod_value("name=\"My\nmod\" path=\"mod/x\"\n", "name") == "My\nmod");
        CHECK(files::mod_value("path=\"mod/a\"\npath=\"mod/b\"\n", "path") == "mod/b");
        CHECK(files::mod_value("name=\"a\"path=\"mod/x\"\n", "path") == "mod/x");
        CHECK(files::mod_value("tags={ path=\"mod/x\" }\n", "path").empty());
        CHECK(files::mod_value("path={ \"mod/x\" }\n", "path").empty());

        const std::filesystem::path user = std::filesystem::temp_directory_path() /
                                           ("anvil-map-files-" + std::to_string(GetCurrentProcessId()));
        std::filesystem::remove_all(user);
        const auto write = [](const std::filesystem::path& p, std::string_view text) {
            std::filesystem::create_directories(p.parent_path());
            std::ofstream(p, std::ios::binary) << text;
        };
        // The last entry is not UTF-8 (a .mod named in Windows-1252 by hand).
        write(user / "dlc_load.json",
              "{\"enabled_mods\":[\"mod/a.mod\",\"mod/z.mod\",\"mod/ansi.mod\",\"mod/b.mod\",\"mod/Kr\xF3l.mod\"]}");
        write(user / "mod/a.mod", "name=\"First\"\npath=\"mod/first\"\n");
        write(user / "mod/z.mod", "name=\"Zipped\"\narchive=\"mod/z.zip\"\n");
        // Saved in Windows-1252 by hand: std::filesystem::path would throw on it.
        write(user / "mod/ansi.mod", "name=\"Ansi\"\npath=\"mod/Kr\xF3lestwo\"\n");
        write(user / "mod/b.mod", "name=\"Second\"\npath=\"" + (user / "second").generic_string() + "\"\n");
        write(user / "mod/first/anvil/map_modes/shared.txt", "old={label=Old rule={always=yes color={0 0 0} label=x}}");
        write(user / "mod/first/anvil/map_modes/own.txt", "mine={label=Mine rule={always=yes color={0 0 0} label=x}}");
        write(user / "mod/first/anvil/map_modes/notes.md", "not read");
        write(user / "second/anvil/map_modes/Shared.TXT", "new={label=New rule={always=yes color={0 0 0} label=x}}");
        write(user / "second/anvil/map_modes/z.txt", "mine={label=Again rule={always=yes color={0 0 0} label=x}}");

        std::vector<std::string> problems;
        const std::vector<files::Found> found = files::playset_files(user, problems);
        CHECK(problems.size() == 3 && problems[0] == "Zipped: a zipped mod; its anvil/map_modes files are not read" &&
              problems[1] == "Ansi: its path is not UTF-8; its anvil/map_modes files are not read" &&
              problems[2] == "dlc_load.json: mod 5 is not a UTF-8 file name; its anvil/map_modes files are not read");
        CHECK(found.size() == 3);
        if (found.size() == 3) {
            CHECK(found[0].source == "First/anvil/map_modes/own.txt");
            CHECK(found[1].source == "Second/anvil/map_modes/Shared.TXT");  // replaced the first mod's
            CHECK(found[2].source == "Second/anvil/map_modes/z.txt");
        }
        problems.clear();
        const std::vector<files::FileMode> modes = files::load(found, problems);
        CHECK(modes.size() == 2 && modes[0].name == "mine" && modes[1].name == "new");
        CHECK(problems.size() == 1 &&
              problems[0] == "Second/anvil/map_modes/z.txt:1: mine is defined earlier; this one is left out");

        // A file of one mode repeated: twenty lines and a count.
        write(user / "repeated.txt", [] {
            std::string text;
            for (int i = 0; i < 26; ++i) text += "r={label=R rule={always=yes color={0 0 0} label=x}}\n";
            return text;
        }());
        problems.clear();
        const std::vector<files::FileMode> repeated = files::load({{user / "repeated.txt", "R/repeated.txt"}}, problems);
        CHECK(repeated.size() == 1 && problems.size() == files::kFaultsShown + 1 &&
              problems[0] == "R/repeated.txt:2: r is defined earlier; this one is left out" &&
              problems.back() == "R/repeated.txt: 5 more modes defined earlier, left out");

        // Reading again: a stamp changes with the file, and the plan maps the
        // new definitions onto the registered modes.
        const std::vector<files::Stamp> before = files::stamps(found);
        CHECK(before.size() == 3 && before == files::stamps(found));
        write(user / "second/anvil/map_modes/Shared.TXT",
              "new={label=New rule={always=yes color={255 0 0} label=red}}\n"
              "extra={label=Extra rule={always=yes color={0 0 0} label=x}}");
        std::vector<files::Stamp> after = files::stamps(files::playset_files(user, problems));
        CHECK(after.size() == 3 && after != before && after[0] == before[0] && after[2] == before[2]);
        problems.clear();
        std::vector<files::FileMode> again = files::load(files::playset_files(user, problems), problems);
        std::vector<const files::FileMode*> registered{&modes[0], &modes[1]};
        files::Reload plan = files::plan_reload(registered, again);
        CHECK(plan.fresh.size() == 2 && plan.fresh[0] == 0 && plan.fresh[1] == 1);
        CHECK(again.size() == 3 && again[1].rules.size() == 1 && again[1].rules[0].color == 0xFF0000u);
        CHECK(plan.restart.empty() && plan.missing.empty());
        CHECK(plan.added.size() == 1 && plan.added[0] == "extra");
        // A changed label waits for a restart; a mode gone keeps its rules.
        write(user / "second/anvil/map_modes/Shared.TXT", "new={label=Renamed rule={always=yes color={0 0 0} label=x}}");
        write(user / "mod/first/anvil/map_modes/own.txt", "mine={label=Mine rule={always=yes}}");  // a fault now
        std::filesystem::remove(user / "second/anvil/map_modes/z.txt");  // else its mine would take over
        problems.clear();
        const std::vector<files::Found> fewer = files::playset_files(user, problems);
        CHECK(fewer.size() == 2 && files::stamps(fewer).size() == 2);  // a file gone changes the stamps too
        again = files::load(fewer, problems);
        plan = files::plan_reload(registered, again);
        CHECK(plan.fresh.size() == 2 && plan.fresh[0] == -1 && plan.fresh[1] >= 0);
        CHECK(plan.missing.size() == 1 && plan.missing[0] == "mine");
        CHECK(plan.restart.size() == 1 && plan.restart[0] == "new");
        CHECK(plan.added.empty() && !problems.empty());
        std::filesystem::remove_all(user);
    });
}

}  // namespace

int main() {
    id_tests();
    registry_tests();
    color_tests();
    show_tests();
    text_tests();
    legend_tests();
    file_tests();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
