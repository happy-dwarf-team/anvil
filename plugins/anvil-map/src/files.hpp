// anvil-map: map modes a mod defines in files, without C++
// (docs/spec/map-mode-files-v1.md). A mod's anvil/map_modes/*.txt, in the
// game's script syntax, names modes and the rules that colour a province;
// anvil-map registers each as its own plugin mode and computes the colours
// while the mode is shown.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil_map::files {

inline constexpr std::size_t kRulesMax = 64;
inline constexpr std::size_t kFileMax = 1u << 20;
// The most faults of one file given one line each; the rest are counted in one more line.
inline constexpr std::size_t kFaultsShown = 20;

// What a rule reads of a province.
enum class Test { always, flag, variable, modifier, value, owned_by, core, occupied, subject, controlled_by, core_of,
                  subject_of, religion, culture, trade_goods,
                  religion_group, culture_group, area, region, superregion, continent, capital,
                  hre, city, trade_node, coastal, state };
// Tests that pass or not, with no number: they take no bounds and no scale.
bool is_yes_no(Test test);

// The game's own values a `value` rule reads, x 1000: the province's, then
// from treasury on its owner's (owner = yes).
enum class Value { base_tax, base_production, base_manpower, development, local_autonomy, devastation, trade_power,
                   treasury, stability, prestige, manpower, monthly_income, provinces };
bool is_country_value(Value value);

struct Rule {
    Test test = Test::always;
    std::string name;    // the flag, variable or modifier key, the value's name, a country tag, or the
                         // religion's, culture's or trade goods' script key
    Value value = Value::development;  // for Test::value
    bool owner = false;  // read from the province's owner, not the province
    bool yes = true;     // core, occupied, subject: = yes or = no
    // Bounds x 1000, as the game keeps values; none set: the value is not 0.
    std::optional<std::int64_t> at_least, at_most, above, below;
    std::uint32_t color = 0;  // 0x00RRGGBB; with a scale, the colour at its low end
    // A colour scale (x 1000): color at low and below, high_color at high and
    // above, each channel linear between them.
    std::optional<std::int64_t> low, high;
    std::uint32_t high_color = 0;
    std::string label;  // UTF-8
    int line = 0;

    bool scaled() const { return low.has_value(); }
};

struct FileMode {
    std::string name;         // [a-z0-9_-]{1,32}; the id is anvil-map.<name>
    std::string label;        // UTF-8, 1 to 64 bytes
    std::string description;  // UTF-8, at most 512 bytes
    std::string sprite;       // empty: the generic one
    std::uint32_t tab = 0;    // 0 to 4
    bool legend = true;
    std::vector<Rule> rules;
    std::string source;  // "<mod>/anvil/map_modes/<file>", for messages
    int line = 0;
};

// Parses one file. Appends the modes it defines without a fault to `out` and
// a line "<source>:<line>: <what>" for each fault to `problems`, at most
// kFaultsShown of them and then "<source>: N more faults" for the rest; a mode
// with a fault is left out whole. Text that is not UTF-8 is read as Windows-1252,
// the encoding of the game's own script files.
void parse(std::string_view text, std::string_view source, std::vector<FileMode>& out,
           std::vector<std::string>& problems);

// Whether `value` (x 1000) passes the rule's bounds.
bool in_bounds(const Rule& rule, std::int64_t value);

// The colour of a province the rule passes with `value` (x 1000): the rule's
// colour, or for a scale the colour at that point of it.
std::uint32_t color_at(const Rule& rule, std::int64_t value);

// "7.5" for 7500.
std::string decimal(std::int64_t value);

// The world as the rules read it; the plugin's answers come from
// anvil-game.world, the tests' from a table.
class World {
public:
    virtual ~World() = default;
    virtual std::int32_t province_limit() = 0;
    // The ids the game gives a flag name or a modifier key; nullopt while the
    // game has none (a flag no effect has set yet, an unknown modifier).
    virtual std::optional<std::int32_t> flag_id(const std::string& name) = 0;
    virtual std::optional<std::int32_t> modifier_id(const std::string& key) = 0;
    virtual bool has_flag(std::int32_t province, std::int32_t flag) = 0;
    // x 1000; a variable the province lacks is 0, as for the game's triggers.
    virtual std::int64_t variable(std::int32_t province, const std::string& name) = 0;
    virtual std::int64_t modifier(std::int32_t province, std::int32_t id) = 0;
    // The province's owner's tag; empty for none.
    virtual std::string owner(std::int32_t province) = 0;
    virtual bool country_has_flag(const std::string& tag, std::int32_t flag) = 0;
    virtual std::int64_t country_variable(const std::string& tag, const std::string& name) = 0;
    virtual std::int64_t country_modifier(const std::string& tag, std::int32_t id) = 0;
    // x 1000; nullopt when the game does not give it.
    virtual std::optional<std::int64_t> province_value(std::int32_t province, Value value) = 0;
    virtual std::optional<std::int64_t> country_value(const std::string& tag, Value value) = 0;
    // The province's controller's tag; empty for none.
    virtual std::string controller(std::int32_t province) = 0;
    // Whether `tag` has a core on the province.
    virtual bool has_core(std::int32_t province, const std::string& tag) = 0;
    // The country's overlord's tag; empty for none.
    virtual std::string overlord(const std::string& tag) = 0;
    // The country's capital, a province ID; 0 for none.
    virtual std::int32_t capital(const std::string& tag) = 0;
    // Whether the province is part of the Holy Roman Empire (Test::hre), a
    // city (Test::city) or coastal land (Test::coastal); nullopt when the game
    // does not say, and for coastal when it is not land or is wasteland.
    virtual std::optional<bool> province_is(std::int32_t province, Test test) = 0;
    // The script key of the province's religion, culture or trade goods
    // (`test` is one of the three), or of the country's state religion or
    // primary culture; empty for none.
    virtual std::string province_key(std::int32_t province, Test test) = 0;
    virtual std::string country_key(const std::string& tag, Test test) = 0;
};

// A mode's rules with the game's ids for this computation. A country's
// answers are read once per evaluator (cached by tag), so a computation reads
// each owner once per rule however many provinces it has.
class Evaluator {
public:
    Evaluator(const FileMode& mode, World& world);
    // The first rule the province passes, or null; `value` gets what the rule
    // read (x 1000; 1 for always and a test that passes or not).
    const Rule* match(std::int32_t province, std::int64_t* value = nullptr);
    // colors[i] for province i: the first matching rule's colour, else
    // ANVIL_MAP_NO_COLOR. Sized to the province limit.
    std::vector<std::uint32_t> colors();
    // Modifier keys the game does not know (each rule with one never passes).
    const std::vector<std::string>& unknown() const { return unknown_; }

private:
    // A country's answers, per rule, read when a rule first needs them.
    struct CountryReads {
        std::vector<std::optional<std::int64_t>> value;
        std::vector<bool> done;
    };

    // What rule i reads of a province, or nullopt when it cannot pass (an id
    // the game lacks, no owner for an owner rule). `owner` caches the
    // province's owner for the rules after the first that asks.
    std::optional<std::int64_t> read(std::size_t i, std::int32_t province, std::optional<std::string>& owner);
    std::optional<std::int64_t> read_country(std::size_t i, const std::string& tag);

    const FileMode& mode_;
    World& world_;
    std::vector<std::optional<std::int32_t>> ids_;  // per rule
    std::vector<std::string> unknown_;
    std::map<std::string, CountryReads> countries_;  // by tag
};

// The legend of a mode: each rule's colour and label once, in rule order, at
// most ANVIL_MAP_LEGEND_MAX; a scale gives "<label> <low>" in its colour and
// "<label> <high>" in its high colour.
struct LegendItem {
    std::uint32_t color;
    std::string label;
};
std::vector<LegendItem> legend_of(const FileMode& mode);

// ---- the playset ---------------------------------------------------------------

// The entries of "enabled_mods" in dlc_load.json, in order ("mod/ugc_1.mod").
std::vector<std::string> enabled_mods(std::string_view dlc_load_json);
// The first key="value" of `key` in a .mod file; empty when it has none.
std::string mod_value(std::string_view mod_text, std::string_view key);

// One mode file found in the playset.
struct Found {
    std::filesystem::path path;
    std::string source;  // "<mod name>/anvil/map_modes/<file>"
};

// The mode files of the playset in the user folder `user`: each enabled
// mod's anvil/map_modes/*.txt, mods in load order and each mod's files by
// name. A file of the same name in a later mod replaces an earlier one, as
// the game's files do. A mod without a folder `path` (a zip archive) is
// skipped with a line in `problems`.
std::vector<Found> playset_files(const std::filesystem::path& user, std::vector<std::string>& problems);

// Reads and parses every file of `found`. A mode whose name an earlier file
// already took is left out with a line in `problems`.
std::vector<FileMode> load(const std::vector<Found>& found, std::vector<std::string>& problems);

// ---- reading the files again ---------------------------------------------------

// What a reload compares: each file's path, size and last write time, in the
// order playset_files gives them. Equal stamps: nothing to read again.
struct Stamp {
    std::filesystem::path path;
    std::uintmax_t size = 0;
    std::filesystem::file_time_type time{};
    bool operator==(const Stamp&) const = default;
};
std::vector<Stamp> stamps(const std::vector<Found>& found);

// How the modes read again map onto the registered ones. The game keeps a
// mode's button (label, description, tab, sprite) from the start, so a reload
// changes only its rules and legend.
struct Reload {
    // For each registered mode, the index of its new definition in the modes
    // read again, or -1 when it is gone or has a fault now (it keeps its rules).
    std::vector<int> fresh;
    std::vector<std::string> restart;  // registered modes whose button changed
    std::vector<std::string> added;    // new modes: they need a restart
    std::vector<std::string> missing;  // registered modes not read again
};
Reload plan_reload(const std::vector<const FileMode*>& registered, const std::vector<FileMode>& fresh);

}  // namespace anvil_map::files
