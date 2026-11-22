// anvil-map: the standard plugin that lets plugins add map modes
// (docs/spec/map-modes-v1.md, ADR 0007; the C interface is
// include/anvil/anvil_map.h).
//
// A plugin mode borrows a number of the game's map mode enum, and the hooks
// answer for that number in place of the game:
//   - map_color paints the plugin's last colour buffer (over the political
//     map mode where it has no colour); it runs in any frame, nested ones
//     included, so it reads nothing but that buffer and the game's count;
//   - map_mode_label and map_mode_sprite give the button's name, tooltip and
//     icon; the call in CSettings::WriteMembers gets the sprite the writer
//     skips, so settings.txt never keeps a plugin mode under a debug mode's name;
//   - add_map_modes appends the tab's plugin modes to the array the game
//     builds the tab's buttons from;
//   - mapmode_tooltip writes the plugin's kept lines in place of the borrowed
//     debug mode's and notes which province was asked for.
// Every other number goes to the game unchanged, so without plugin modes
// nothing the player sees changes.
//
// The step runs after each frame of the main loop (anvil-game.frame) while a
// session runs: a mode show() asked for is set through the game's
// SetCurrentMapMode, then shown/hidden when the current map mode changes, the shown
// mode marked dirty after a new buffer, and describe() for the province the
// tooltip asked for. anvil-game.world may be read there.
#include <anvil/anvil.hpp>
#include <anvil/anvil_game.h>
#include <anvil/anvil_map.h>
#include <anvil/anvil_panel.h>

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "symbols.gen.hpp"

#include "files.hpp"
#include "modes.hpp"
#include "text.hpp"

namespace {

using anvil_map::Registry;

// The political map mode, whose colours a province without one keeps.
constexpr std::int32_t kPolitical = 1;
// The sprite of a plugin mode that names none, and the one settings.txt skips.
constexpr const char* kGenericSprite = "GFX_mapmode_notselected";
constexpr const char* kSkippedSprite = "GFX_mapmode_notselected";
// Room for a tab's array and every plugin mode.
constexpr std::size_t kTabCapacity = 128;
constexpr std::size_t kDescribeCapacity = 2048;

// Mei's text service (mei.text version 1): whether the game's text is UTF-8.
constexpr const char* kMeiTextService = "mei.text";
struct MeiTextService {
    std::uint32_t size;
    int (*game_text_utf8)(void);
};

struct Layout {
    std::uintptr_t* root = nullptr;       // the global that holds the game root
    std::uint32_t session = 0;            // root+: the session
    std::uint32_t map_mode = 0;           // session+: int32, the current map mode
    std::uintptr_t* map = nullptr;        // the global that holds the map's provinces
    std::uint32_t provinces = 0;          // map+: the province vector {begin, end}
    std::uint32_t shift = 0;
    std::uint64_t magic = 0;
    std::uint32_t province_id = 0;        // province+: int32, its own ID
    std::array<const std::int32_t*, anvil_map::kTabs> tabs{};
};

Layout g_layout;
Registry& g_registry = *new Registry;  // kept for the process
bool g_utf8_game = false;
// Another loader of map modes is in the process: every hook passes on.
std::atomic<bool> g_off{false};

anvil::HookOf<sym::map_color> g_color;
anvil::HookOf<sym::map_mode_label> g_label;
anvil::HookOf<sym::map_mode_sprite> g_sprite;
anvil::HookOf<sym::map_mode_sprite> g_settings_sprite;  // the redirected call in CSettings::WriteMembers
anvil::HookOf<sym::add_map_modes> g_add;
anvil::HookOf<sym::mapmode_tooltip> g_tooltip;
sym::Types::cstring_from_text::Pointer g_cstring = nullptr;
sym::Types::text_append::Pointer g_append = nullptr;
sym::Types::set_map_mode_dirty::Pointer g_dirty = nullptr;
sym::Types::set_current_map_mode::Pointer g_set_mode = nullptr;

const AnvilGameWorldService* g_world = nullptr;
// The overlay's windows, for legends; null without the overlay.
const AnvilPanelService* g_panels = nullptr;
// An overlay older than 1.26.0 refuses swatch rows: legends go as text.
bool g_text_legends = false;

std::atomic<bool> g_in_session{false};
std::atomic<std::int32_t> g_province_limit{0};
std::atomic<AnvilMapMode*> g_shown{nullptr};  // as of the last step
// What the tooltip last asked for; the step answers it.
std::atomic<AnvilMapMode*> g_asked_mode{nullptr};
std::atomic<std::int32_t> g_asked_province{0};

std::atomic<std::uint64_t> g_paints{0};
std::atomic<std::uint64_t> g_tooltips{0};

void publish(const char* event, AnvilMapMode* mode) noexcept {
    AnvilMapModeChange change{};
    change.size = sizeof(change);
    change.mode = mode;
    change.id = mode->id.c_str();
    const anvil::Host host;
    host.c_api().publish(host.self(), event, &change);
}

// ---- reads under a guard -------------------------------------------------------

// No objects with destructors here: the body runs under __try.
bool read_raw(std::uintptr_t address, void* out, std::size_t size) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T> bool read(std::uintptr_t address, T& out) noexcept { return read_raw(address, &out, sizeof(T)); }

// The session object and its current map mode.
bool current_mode(std::uintptr_t& session, std::int32_t& mode) noexcept {
    std::uintptr_t root = 0;
    return read(reinterpret_cast<std::uintptr_t>(g_layout.root), root) && root != 0 &&
           read(root + g_layout.session, session) && session != 0 && read(session + g_layout.map_mode, mode);
}

// How many provinces the paint fills, as the game counts them.
std::uint64_t paint_count() noexcept {
    std::uintptr_t map = 0;
    std::uintptr_t vector[2] = {};
    if (!read(reinterpret_cast<std::uintptr_t>(g_layout.map), map) || map == 0 ||
        !read_raw(map + g_layout.provinces, vector, sizeof(vector)) || vector[1] < vector[0]) {
        return 0;
    }
    const std::uint64_t count = ((vector[1] - vector[0]) >> g_layout.shift) * g_layout.magic;
    return count > 100000 ? 0 : count;
}

// ---- the hooks -----------------------------------------------------------------

AnvilMapMode* plugin_mode(std::int32_t number) noexcept {
    return g_off.load(std::memory_order_relaxed) ? nullptr : g_registry.by_number(number);
}

void on_color(std::uint8_t* colors, std::uint32_t kind, std::int32_t mode) noexcept {
    AnvilMapMode* m = plugin_mode(mode);
    if (m == nullptr) {
        g_color.next(colors, kind, mode);
        return;
    }
    g_paints.fetch_add(1, std::memory_order_relaxed);
    const std::shared_ptr<const anvil_map::Colors> buffer = m->snapshot();
    const std::uint64_t count = paint_count();
    const std::size_t given = buffer ? buffer->values.size() : 0;
    if (buffer == nullptr || !buffer->complete || given < count) g_color.next(colors, kind, kPolitical);
    if (buffer == nullptr || colors == nullptr) return;
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(count, given));
    for (std::size_t i = 1; i < n; ++i) {
        const std::uint32_t c = buffer->values[i];
        if ((c >> 24) != 0) continue;
        std::uint8_t* p = colors + i * 4;
        p[0] = static_cast<std::uint8_t>(c);        // B
        p[1] = static_cast<std::uint8_t>(c >> 8);   // G
        p[2] = static_cast<std::uint8_t>(c >> 16);  // R
        p[3] = 0xFF;
    }
}

void* on_label(void* out, std::int32_t mode, std::uint8_t description) noexcept {
    AnvilMapMode* m = plugin_mode(mode);
    if (m == nullptr) return g_label.next(out, mode, description);
    return g_cstring(out, description != 0 ? m->description.c_str() : m->label.c_str());
}

void* on_sprite(void* out, std::int32_t mode) noexcept {
    AnvilMapMode* m = plugin_mode(mode);
    if (m == nullptr) return g_sprite.next(out, mode);
    return g_cstring(out, m->sprite.empty() ? kGenericSprite : m->sprite.c_str());
}

void* on_settings_sprite(void* out, std::int32_t mode) noexcept {
    if (plugin_mode(mode) == nullptr) return g_settings_sprite.next(out, mode);
    return g_cstring(out, kSkippedSprite);
}

void on_add(void* panel, const std::int32_t* modes, std::int32_t count, std::uint8_t flag) noexcept {
    std::uint32_t tab = 0;
    for (std::uint32_t t = 0; t < anvil_map::kTabs; ++t) {
        if (modes == g_layout.tabs[t]) tab = t + 1;
    }
    if (tab == 0 || g_off.load(std::memory_order_relaxed) || count < 0 ||
        static_cast<std::size_t>(count) + anvil_map::kPool.size() > kTabCapacity) {
        g_add.next(panel, modes, count, flag);
        return;
    }
    // One array per tab, kept: the game reads it while it builds the buttons.
    static std::array<std::array<std::int32_t, kTabCapacity>, anvil_map::kTabs> arrays{};
    std::array<std::int32_t, kTabCapacity>& array = arrays[tab - 1];
    std::size_t added = 0;
    try {
        added = g_registry.tab_numbers(tab, array.data() + count, anvil_map::kPool.size());
    } catch (...) {
        added = 0;
    }
    if (added == 0) {
        g_add.next(panel, modes, count, flag);
        return;
    }
    std::memcpy(array.data(), modes, static_cast<std::size_t>(count) * sizeof(std::int32_t));
    g_add.next(panel, array.data(), count + static_cast<std::int32_t>(added), flag);
}

void on_tooltip(void* province, std::uint8_t flag, const void* other, void* tooltip) noexcept {
    std::uintptr_t session = 0;
    std::int32_t mode = -1;
    AnvilMapMode* m = nullptr;
    if (g_in_session.load(std::memory_order_acquire) && current_mode(session, mode)) m = plugin_mode(mode);
    if (m == nullptr) {
        g_tooltip.next(province, flag, other, tooltip);
        return;
    }
    // The plugin's lines replace the borrowed debug mode's.
    g_tooltips.fetch_add(1, std::memory_order_relaxed);
    std::int32_t id = 0;
    if (province == nullptr || !read(reinterpret_cast<std::uintptr_t>(province) + g_layout.province_id, id)) return;
    // Without anvil-game.world, the map's own count of provinces is the limit.
    const std::int32_t world_limit = g_province_limit.load(std::memory_order_relaxed);
    const std::uint64_t limit = world_limit > 0 ? static_cast<std::uint64_t>(world_limit) : paint_count();
    if (!anvil_map::describable(id, limit)) return;
    g_asked_province.store(id, std::memory_order_relaxed);
    g_asked_mode.store(m, std::memory_order_release);
    try {
        const std::string text = anvil_map::kept_text(*m, id);
        if (!text.empty() && tooltip != nullptr) g_append(tooltip, text.data(), text.size());
    } catch (...) {
        // Only a failed allocation: the tooltip goes without the lines.
    }
}

// ---- the step ------------------------------------------------------------------

// No objects with destructors here: the body runs under __try. Only an access
// violation costs the switch; any other exception in the game's code goes on
// to the crash handler.
bool call_set_mode(std::uintptr_t session, std::int32_t number, unsigned long* code) noexcept {
    __try {
        g_set_mode(reinterpret_cast<void*>(session), number, 0, 0);
        return true;
    } __except (*code = GetExceptionCode(),
                *code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

// A mode show() asked for: the game's own switch, as the minimap's button.
void show_asked() {
    std::uintptr_t session = 0;
    std::int32_t number = -1;
    const bool readable = current_mode(session, number);
    unsigned long code = 0;
    bool faulted = false;
    AnvilMapMode* asked = anvil_map::switch_to_asked(g_registry, readable, session, number,
                                                     [&](std::uintptr_t s, std::int32_t n) {
                                                         faulted = !call_set_mode(s, n, &code);
                                                     });
    if (asked != nullptr && faulted) {
        const anvil::Host host;
        host.warn("show {}: SetCurrentMapMode faulted ({:#x})", asked->id, code);
    }
}

void describe_asked(AnvilMapMode* shown) {
    AnvilMapMode* asked = g_asked_mode.exchange(nullptr, std::memory_order_acq_rel);
    if (asked == nullptr || asked != shown || asked->describe == nullptr) return;
    const std::int32_t province = g_asked_province.load(std::memory_order_relaxed);
    std::array<char, kDescribeCapacity> buffer{};
    std::size_t n = 0;
    try {
        n = asked->describe(asked->context, asked, province, buffer.data(), buffer.size());
    } catch (...) {
        n = 0;
    }
    n = std::min(n, buffer.size() - 1);
    anvil_map::keep_text(*asked, province,
                         anvil_map::game_text(anvil_map::utf8_prefix(std::string_view(buffer.data(), n)), g_utf8_game));
}

// ---- modes from files ----------------------------------------------------------

namespace files = anvil_map::files;

// anvil-game.world as the rules read it. Main thread, in a step or an
// anvil-game event's handler.
class GameWorld final : public files::World {
public:
    std::int32_t province_limit() override { return g_world->province_limit(); }
    std::optional<std::int32_t> flag_id(const std::string& name) override {
        std::int32_t id = 0;
        if (!ANVIL_GAME_WORLD_HAS(g_world, flag_id) || g_world->flag_id(name.c_str(), &id) != ANVIL_OK) {
            return std::nullopt;
        }
        return id;
    }
    std::optional<std::int32_t> modifier_id(const std::string& key) override {
        std::int32_t id = 0;
        if (!ANVIL_GAME_WORLD_HAS(g_world, modifier_id) || g_world->modifier_id(key.c_str(), &id) != ANVIL_OK) {
            return std::nullopt;
        }
        return id;
    }
    bool has_flag(std::int32_t province, std::int32_t flag) override {
        if (!ANVIL_GAME_WORLD_HAS(g_world, province_flags)) return false;
        std::array<AnvilGameFlag, 64> some{};
        std::uint32_t count = 0;
        if (g_world->province_flags(province, some.data(), static_cast<std::uint32_t>(some.size()), &count) !=
            ANVIL_OK) {
            return false;
        }
        if (count <= some.size()) {
            return std::any_of(some.begin(), some.begin() + count, [&](const AnvilGameFlag& f) { return f.id == flag; });
        }
        std::vector<AnvilGameFlag> all(count);
        if (g_world->province_flags(province, all.data(), count, &count) != ANVIL_OK) return false;
        count = std::min<std::uint32_t>(count, static_cast<std::uint32_t>(all.size()));
        return std::any_of(all.begin(), all.begin() + count, [&](const AnvilGameFlag& f) { return f.id == flag; });
    }
    std::int64_t variable(std::int32_t province, const std::string& name) override {
        std::int32_t value = 0;
        if (!ANVIL_GAME_WORLD_HAS(g_world, province_variable) ||
            g_world->province_variable(province, name.c_str(), &value) != ANVIL_OK) {
            return 0;
        }
        return value;
    }
    std::int64_t modifier(std::int32_t province, std::int32_t id) override {
        std::int32_t value = 0;
        if (g_world->province_modifier(province, id, &value) != ANVIL_OK) return 0;
        return value;
    }
};

struct FileEntry {
    files::FileMode def;
    const AnvilMapMode* mode = nullptr;
    std::unique_ptr<files::Evaluator> evaluator;  // of the last computation, for the tooltip
    bool warned = false;                          // unknown modifiers were logged
};

GameWorld g_game_world;
std::vector<std::unique_ptr<FileEntry>> g_file_modes;  // main thread, filled at ready

FileEntry* file_entry(const AnvilMapMode* mode) {
    for (const auto& e : g_file_modes) {
        if (e->mode == mode) return e.get();
    }
    return nullptr;
}

// "7.5" for 7500.
std::string decimal(std::int64_t value) {
    const bool negative = value < 0;
    const std::uint64_t v = negative ? 0 - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
    std::string out = (negative ? "-" : "") + std::to_string(v / 1000);
    if (const std::uint64_t f = v % 1000; f != 0) {
        std::string digits = std::to_string(1000 + f).substr(1);
        while (digits.back() == '0') digits.pop_back();
        out += "." + digits;
    }
    return out;
}

// The tooltip of a mode from a file: the label of the rule the province
// passes, with the value it read.
std::size_t file_describe(void* context, const AnvilMapMode*, std::int32_t province, char* text, std::size_t capacity) {
    auto* entry = static_cast<FileEntry*>(context);
    if (entry->evaluator == nullptr || capacity == 0) return 0;
    const files::Rule* rule = entry->evaluator->match(province);
    if (rule == nullptr) return 0;
    std::string line = rule->label;
    if (rule->test == files::Test::variable) {
        line += " (" + rule->name + " " + decimal(g_game_world.variable(province, rule->name)) + ")";
    }
    const std::size_t n = std::min(line.size(), capacity - 1);
    std::memcpy(text, line.data(), n);
    return n;
}

// Computes the colours of a mode from a file and hands them over.
void compute(FileEntry& entry) {
    if (g_world == nullptr) return;
    const std::uint64_t epoch = g_registry.epoch();
    entry.evaluator = std::make_unique<files::Evaluator>(entry.def, g_game_world);
    if (!entry.warned && !entry.evaluator->unknown().empty()) {
        entry.warned = true;
        const anvil::Host host;
        for (const std::string& key : entry.evaluator->unknown()) {
            host.warn("{}: the game has no modifier {}; its rule never passes", entry.def.source, key);
        }
    }
    const std::vector<std::uint32_t> colors = entry.evaluator->colors();
    auto buffer = std::make_shared<anvil_map::Colors>();
    if (anvil_map::make_colors(colors.data(), static_cast<std::uint32_t>(colors.size()),
                               g_province_limit.load(std::memory_order_relaxed), *buffer) == ANVIL_OK) {
        g_registry.store_colors(entry.mode, std::move(buffer), epoch);
    }
}

// The modes of the playset's files, registered once at ready.
void load_file_modes() {
    const anvil::Host host;
    const auto* game = host.service<AnvilGameService>(ANVIL_GAME_SERVICE, ANVIL_GAME_SERVICE_VERSION);
    if (game == nullptr || !ANVIL_GAME_HAS(game, user_dir) || game->user_dir() == nullptr) {
        host.info("map modes from files need anvil-game 1.24.0 (user_dir): none read");
        return;
    }
    std::vector<std::string> problems;
    const std::vector<files::Found> found = files::playset_files(std::filesystem::path(game->user_dir()), problems);
    std::vector<files::FileMode> modes = files::load(found, problems);
    for (const std::string& p : problems) host.warn("map mode file {}", p);
    for (files::FileMode& def : modes) {
        auto entry = std::make_unique<FileEntry>();
        entry->def = std::move(def);
        const std::string id = std::string(ANVIL_MAP_ID) + "." + entry->def.name;
        AnvilMapModeInfo info{};
        info.size = sizeof(info);
        info.tab = entry->def.tab;
        info.id = id.c_str();
        info.label = entry->def.label.c_str();
        info.description = entry->def.description.empty() ? nullptr : entry->def.description.c_str();
        info.sprite = entry->def.sprite.empty() ? nullptr : entry->def.sprite.c_str();
        info.describe = &file_describe;
        info.context = entry.get();
        std::string why;
        const AnvilMapMode* mode = nullptr;
        if (const AnvilResult r = g_registry.add(&info, g_utf8_game, &mode, why); r != ANVIL_OK) {
            host.warn("{}:{}: {} is not registered: {}", entry->def.source, entry->def.line, id, why);
            continue;
        }
        entry->mode = mode;
        if (entry->def.legend) {
            std::vector<anvil_map::LegendEntry> legend;
            for (files::LegendItem& item : files::legend_of(entry->def)) legend.push_back({item.color, std::move(item.label)});
            anvil_map::set_legend(*const_cast<AnvilMapMode*>(mode), std::move(legend));
        }
        host.info("mode {} from {} borrows map mode {} (tab {}, {} rules)", id, entry->def.source, mode->number,
                  mode->tab, entry->def.rules.size());
        g_file_modes.push_back(std::move(entry));
    }
}

// ---- legends -------------------------------------------------------------------

// "#RRGGBB" for a legend drawn as text.
std::string hex_color(std::uint32_t color) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out = "#";
    for (int shift = 20; shift >= 0; shift -= 4) out += kDigits[(color >> shift) & 0xF];
    return out;
}

// Brings `mode`'s panel up to its last legend; true when its rows changed.
// The panel is created for the first legend.
bool sync_legend(AnvilMapMode& mode) {
    const std::uint64_t generation = mode.legend_generation.load(std::memory_order_acquire);
    if (generation == mode.legend_drawn) return false;
    mode.legend_drawn = generation;
    const std::vector<anvil_map::LegendEntry> legend = anvil_map::legend_of(mode);
    const anvil::Host host;
    if (mode.panel == nullptr) {
        if (legend.empty()) return false;
        AnvilPanel* panel = nullptr;
        if (const AnvilResult r = g_panels->create(mode.label_utf8.c_str(), &panel); r != ANVIL_OK) {
            host.warn("the legend of {}: the overlay made no window ({})", mode.id, static_cast<int>(r));
            return false;
        }
        mode.panel = panel;
    }
    auto* panel = static_cast<AnvilPanel*>(mode.panel);
    std::vector<AnvilPanelRow> rows(legend.size());
    for (std::size_t i = 0; i < legend.size(); ++i) {
        rows[i].kind = ANVIL_PANEL_SWATCH;
        rows[i].label = legend[i].label.c_str();
        rows[i].color = legend[i].color;
    }
    const auto count = static_cast<std::uint32_t>(rows.size());
    AnvilResult r = g_text_legends ? ANVIL_ERROR_INVALID_ARGUMENT
                                   : g_panels->set_rows(panel, rows.data(), count, sizeof(AnvilPanelRow));
    if (r == ANVIL_ERROR_INVALID_ARGUMENT) {
        // An older overlay: the colour as text beside the label.
        if (!g_text_legends) host.info("the overlay draws no colour squares (before 1.26.0): legends show #RRGGBB");
        g_text_legends = true;
        std::vector<std::string> hex(legend.size());
        for (std::size_t i = 0; i < legend.size(); ++i) {
            hex[i] = hex_color(legend[i].color);
            rows[i].kind = ANVIL_PANEL_TEXT;
            rows[i].value = hex[i].c_str();
        }
        r = g_panels->set_rows(panel, rows.data(), count, sizeof(AnvilPanelRow));
    }
    if (r != ANVIL_OK) host.warn("the legend of {}: the overlay refused its rows ({})", mode.id, static_cast<int>(r));
    mode.legend_rows = r == ANVIL_OK ? legend.size() : 0;
    return true;
}

// The shown mode's legend is up while it is shown; the last one's goes down.
void legend_step(AnvilMapMode* shown, AnvilMapMode* last) {
    if (g_panels == nullptr) return;
    if (last != shown && last != nullptr && last->panel != nullptr) {
        g_panels->show(static_cast<AnvilPanel*>(last->panel), 0);
    }
    if (shown == nullptr) return;
    const bool changed = sync_legend(*shown);
    if (shown->panel != nullptr && (changed || last != shown)) {
        g_panels->show(static_cast<AnvilPanel*>(shown->panel), shown->legend_rows > 0 ? 1 : 0);
    }
}

void step() {
    if (!g_in_session.load(std::memory_order_acquire) || g_off.load(std::memory_order_relaxed)) return;
    if (g_world != nullptr && g_province_limit.load(std::memory_order_relaxed) == 0) {
        g_province_limit.store(g_world->province_limit(), std::memory_order_relaxed);
    }
    show_asked();
    std::uintptr_t session = 0;
    std::int32_t number = -1;
    AnvilMapMode* shown = current_mode(session, number) ? plugin_mode(number) : nullptr;
    AnvilMapMode* last = g_shown.load(std::memory_order_relaxed);
    if (shown != last) {
        g_shown.store(shown, std::memory_order_release);
        if (last != nullptr) publish(ANVIL_MAP_EVENT_HIDDEN, last);
        if (shown != nullptr) publish(ANVIL_MAP_EVENT_SHOWN, shown);
        if (FileEntry* entry = shown != nullptr ? file_entry(shown) : nullptr) compute(*entry);
    }
    legend_step(shown, last);
    if (shown != nullptr) {
        // A new buffer (maybe from the shown handler just now) repaints the map.
        const std::uint64_t generation = shown->generation.load(std::memory_order_acquire);
        if (generation != shown->dirtied) {
            shown->dirtied = generation;
            g_dirty(reinterpret_cast<void*>(session), number);
        }
    }
    describe_asked(shown);
}

void on_session(const void* payload) {
    const auto* s = static_cast<const AnvilGameSession*>(payload);
    if (s == nullptr) return;
    if (s->change == ANVIL_GAME_SESSION_STARTED) {
        g_province_limit.store(g_world != nullptr ? g_world->province_limit() : 0, std::memory_order_relaxed);
        g_in_session.store(true, std::memory_order_release);
        return;
    }
    g_in_session.store(false, std::memory_order_release);
    g_province_limit.store(0, std::memory_order_relaxed);
    const std::uint64_t paints = g_paints.exchange(0), tooltips = g_tooltips.exchange(0);
    if (paints != 0 || tooltips != 0) {
        const anvil::Host host;
        host.info("session {} ended: {} paints and {} tooltips of plugin modes", s->number, paints, tooltips);
    }
    g_asked_mode.store(nullptr, std::memory_order_relaxed);
    g_registry.clear_session();
    if (AnvilMapMode* last = g_shown.exchange(nullptr, std::memory_order_acq_rel)) {
        publish(ANVIL_MAP_EVENT_HIDDEN, last);
        legend_step(nullptr, last);
    }
}

// ---- the service ---------------------------------------------------------------

AnvilResult svc_register(const AnvilMapModeInfo* info, const AnvilMapMode** out) {
    const anvil::Host host;
    if (g_off.load(std::memory_order_relaxed)) {
        host.warn("register_mode {}: refused, another loader of map modes runs (eu4agent.dll)",
                  info != nullptr && info->id != nullptr ? info->id : "(null)");
        return ANVIL_ERROR_UNSUPPORTED;
    }
    try {
        std::string why;
        const AnvilResult r = g_registry.add(info, g_utf8_game, out, why);
        if (r == ANVIL_OK) {
            host.info("mode {} borrows map mode {} (tab {})", (*out)->id, (*out)->number, (*out)->tab);
        } else {
            host.warn("register_mode {}: {}", info != nullptr && info->id != nullptr ? info->id : "(null)", why);
        }
        return r;
    } catch (...) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult svc_set_colors(const AnvilMapMode* mode, const std::uint32_t* colors, std::uint32_t count) {
    if (!g_registry.owns(mode)) return ANVIL_ERROR_INVALID_ARGUMENT;
    // The epoch before the check, as in show(): a buffer of a session that
    // ends meanwhile is dropped, not painted in the next one.
    const std::uint64_t epoch = g_registry.epoch();
    if (!g_in_session.load(std::memory_order_acquire)) return ANVIL_ERROR_WRONG_PHASE;
    try {
        auto buffer = std::make_shared<anvil_map::Colors>();
        const AnvilResult r =
            anvil_map::make_colors(colors, count, g_province_limit.load(std::memory_order_relaxed), *buffer);
        if (r != ANVIL_OK) return r;
        return g_registry.store_colors(mode, std::move(buffer), epoch) ? ANVIL_OK : ANVIL_ERROR_WRONG_PHASE;
    } catch (...) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

const char* svc_mode_id(std::int32_t number) {
    const AnvilMapMode* m = g_registry.by_number(number);
    return m != nullptr ? m->id.c_str() : nullptr;
}

const AnvilMapMode* svc_shown() { return g_shown.load(std::memory_order_acquire); }

const char* svc_id(const AnvilMapMode* mode) { return mode != nullptr ? mode->id.c_str() : nullptr; }

AnvilResult svc_show(const AnvilMapMode* mode) {
    return anvil_map::ask_show(g_registry, mode, g_off.load(std::memory_order_relaxed), g_in_session);
}

AnvilResult svc_set_legend(const AnvilMapMode* mode, const AnvilMapLegendEntry* entries, std::uint32_t count,
                           std::uint32_t entry_size) {
    if (!g_registry.owns(mode)) return ANVIL_ERROR_INVALID_ARGUMENT;
    try {
        std::vector<anvil_map::LegendEntry> legend;
        std::string why;
        const AnvilResult r = anvil_map::make_legend(entries, count, entry_size, legend, why);
        if (r != ANVIL_OK) {
            const anvil::Host host;
            host.warn("set_legend {}: {}", mode->id, why);
            return r;
        }
        anvil_map::set_legend(*const_cast<AnvilMapMode*>(mode), std::move(legend));
        return ANVIL_OK;
    } catch (...) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

const AnvilMapModesService kService = {
    sizeof(AnvilMapModesService), &svc_register, &svc_set_colors, &svc_mode_id, &svc_shown, &svc_id, &svc_show,
    &svc_set_legend,
};

bool other_loader() { return GetModuleHandleW(L"eu4agent.dll") != nullptr; }

struct AnvilMap : anvil::Plugin {
    void load(anvil::Host& host) override {
        g_layout.root = host.data(sym::root);
        g_layout.session = sym::session_of_root.session;
        g_layout.map_mode = static_cast<std::uint32_t>(host.value(sym::set_map_mode_dirty, "map_mode"));
        g_layout.map = host.data(sym::map_global);
        g_layout.provinces = sym::map_color_count.provinces;
        g_layout.shift = sym::map_color_count.shift;
        g_layout.magic = sym::map_color_count.magic;
        g_layout.province_id = sym::province_own_id.own_id;
        g_layout.tabs = {static_cast<const std::int32_t*>(host.address(sym::tab_1)),
                         static_cast<const std::int32_t*>(host.address(sym::tab_2)),
                         static_cast<const std::int32_t*>(host.address(sym::tab_3)),
                         static_cast<const std::int32_t*>(host.address(sym::tab_4))};
        g_cstring = host.function(sym::cstring_from_text);
        g_append = host.function(sym::text_append);
        g_dirty = host.function(sym::set_map_mode_dirty);
        g_set_mode = host.function(sym::set_current_map_mode);

        if (const auto* mei = host.service<MeiTextService>(kMeiTextService, 1);
            mei != nullptr && mei->size >= sizeof(MeiTextService) && mei->game_text_utf8 != nullptr) {
            g_utf8_game = mei->game_text_utf8() != 0;
        }
        g_world = host.service<AnvilGameWorldService>(ANVIL_GAME_WORLD_SERVICE, ANVIL_GAME_WORLD_SERVICE_VERSION);
        if (g_world == nullptr) host.info("anvil-game.world is not available: set_colors is limited by the map only");
        g_panels = host.service<AnvilPanelService>(ANVIL_PANEL_SERVICE, ANVIL_PANEL_SERVICE_VERSION);
        if (g_panels != nullptr && !ANVIL_PANEL_HAS(g_panels, shown)) g_panels = nullptr;
        if (g_panels == nullptr) host.info("anvil-overlay.panel is not available: legends are kept, not drawn");

        host.register_service(ANVIL_MAP_MODES_SERVICE, ANVIL_MAP_MODES_SERVICE_VERSION, kService);
        if (other_loader()) {
            g_off.store(true);
            host.warn("eu4agent.dll is loaded: it hooks the same map mode functions and numbers, so anvil-map "
                      "installs no hooks and refuses plugin modes");
            return;
        }
        // The load is one group: all six are enabled together, or none.
        host.hook(g_color, sym::map_color, on_color);
        host.hook(g_label, sym::map_mode_label, on_label);
        host.hook(g_sprite, sym::map_mode_sprite, on_sprite);
        host.redirect(g_settings_sprite, sym::settings_sprite_call, on_settings_sprite);
        host.hook(g_add, sym::add_map_modes, on_add);
        host.hook(g_tooltip, sym::mapmode_tooltip, on_tooltip);
        host.subscribe(ANVIL_GAME_EVENT_SESSION, [](const void* payload) { on_session(payload); });
        host.subscribe(ANVIL_GAME_EVENT_FRAME, [] {
            try {
                step();
            } catch (...) {
                // Only a failed allocation; the next frame tries again.
            }
        });
        host.subscribe(ANVIL_GAME_EVENT_READY, [] {
            const anvil::Host h;
            if (other_loader() && !g_off.exchange(true)) {
                h.warn("eu4agent.dll is loaded: it hooks the same map mode functions and numbers, so anvil-map's "
                       "hooks pass everything on and plugin modes are refused");
                return;
            }
            try {
                load_file_modes();
            } catch (...) {
                h.warn("map modes from files: out of memory while reading them");
            }
        });
        // A mode from a file shown on the map follows the world once a month.
        host.subscribe(ANVIL_GAME_EVENT_MONTH, [] {
            if (!g_in_session.load(std::memory_order_acquire)) return;
            try {
                if (FileEntry* entry = file_entry(g_shown.load(std::memory_order_acquire))) compute(*entry);
            } catch (...) {
                // Only a failed allocation; the next month tries again.
            }
        });
        host.info("service {} v{}: {} numbers to lend, text in {}; events {}, {}", ANVIL_MAP_MODES_SERVICE,
                  ANVIL_MAP_MODES_SERVICE_VERSION, anvil_map::kPool.size(), g_utf8_game ? "UTF-8" : "Windows-1252",
                  ANVIL_MAP_EVENT_SHOWN, ANVIL_MAP_EVENT_HIDDEN);
    }
};

}  // namespace

ANVIL_PLUGIN(AnvilMap)
