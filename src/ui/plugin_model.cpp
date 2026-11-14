#include "ui/plugin_model.hpp"

#include <algorithm>

#include "ui/settings_order.hpp"

namespace anvil::ui {
namespace {

bool field(const Switches& s, Which which) {
    return which == Which::On ? s.on : which == Which::Ironman ? s.allow_ironman : s.allow_multiplayer;
}
constexpr Which kWhiches[] = {Which::On, Which::Ironman, Which::Multiplayer};

Edit switch_edit(const PluginSnap& plugin, std::string_view module, Which which, bool on) {
    Edit e;
    e.kind = Edit::Kind::Switch;
    e.plugin = plugin.id;
    e.module = std::string(module);
    e.which = which;
    e.on = on;
    return e;
}

Edit setting_edit(const PluginSnap& plugin, const SettingSnap& setting, std::string value) {
    Edit e;
    e.kind = Edit::Kind::Setting;
    e.plugin = plugin.id;
    e.setting = setting.name;
    e.old_value = setting.saved;
    e.value = std::move(value);
    return e;
}

// The edits that move `from` to `to`, field by field.
void switch_edits(const PluginSnap& plugin, std::string_view module, const Switches& from, const Switches& to,
                  std::vector<Edit>& out) {
    for (const Which which : kWhiches) {
        if (field(from, which) != field(to, which)) out.push_back(switch_edit(plugin, module, which, field(to, which)));
    }
}

std::size_t switch_differences(const Switches& a, const Switches& b) {
    std::size_t n = 0;
    for (const Which which : kWhiches) n += field(a, which) != field(b, which) ? 1 : 0;
    return n;
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool contains(std::string_view text, std::string_view query) {
    if (query.empty()) return true;
    if (query.size() > text.size()) return false;
    for (std::size_t i = 0; i + query.size() <= text.size(); ++i) {
        std::size_t j = 0;
        while (j < query.size() && lower(text[i + j]) == lower(query[j])) ++j;
        if (j == query.size()) return true;
    }
    return false;
}

LoadState load_state(const Snapshot& snapshot, const PluginSnap& plugin, std::vector<std::string_view>& visiting) {
    LoadState s;
    if (plugin.blocked == Blocked::CannotLoad) {
        s.why = LoadState::Why::CannotLoad;
        return s;
    }
    if (plugin.blocked == Blocked::Failed) {
        s.why = LoadState::Why::Failed;
        return s;
    }
    if (plugin.blocked == Blocked::WaitsForApproval && !plugin.approved) {
        s.why = LoadState::Why::WaitsForApproval;
        return s;
    }
    if (!plugin.saved.on) {
        s.why = LoadState::Why::Off;
        return s;
    }
    visiting.push_back(plugin.id);
    for (const std::string& id : plugin.requires_ids) {
        // A cycle is the core's error to report; here it does not block.
        if (std::find(visiting.begin(), visiting.end(), id) != visiting.end()) continue;
        const PluginSnap* dep = find_plugin(snapshot, id);
        if (dep == nullptr) {
            s.why = LoadState::Why::NotInstalled;
            s.missing = id;
            break;
        }
        const LoadState d = load_state(snapshot, *dep, visiting);
        if (d.loads) continue;
        if (d.why == LoadState::Why::NeedsPlugin || d.why == LoadState::Why::NotInstalled) {
            s.why = d.why;
            s.missing = d.missing;
            s.through.push_back(id);
            s.through.insert(s.through.end(), d.through.begin(), d.through.end());
        } else {
            s.why = LoadState::Why::NeedsPlugin;
            s.missing = id;
        }
        break;
    }
    visiting.pop_back();
    s.loads = s.why == LoadState::Why::None;
    return s;
}

// Opt-ins live on the modules when there are any, else on the plugin.
bool kept_out(const Switches& in_effect, bool changes_game, Game game) {
    if (!changes_game) return false;
    if (game == Game::Ironman) return !in_effect.allow_ironman;
    if (game == Game::Multiplayer) return !in_effect.allow_multiplayer;
    return false;
}

}  // namespace

const PluginSnap* find_plugin(const Snapshot& snapshot, std::string_view id) {
    for (const PluginSnap& p : snapshot.plugins) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

const ModuleSnap* find_module(const PluginSnap& plugin, std::string_view id) {
    for (const ModuleSnap& m : plugin.modules) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

LoadState load_state(const Snapshot& snapshot, const PluginSnap& plugin) {
    std::vector<std::string_view> visiting;
    return load_state(snapshot, plugin, visiting);
}

bool setting_pending(const SettingSnap& setting) { return setting.saved != setting.in_effect; }
bool setting_changed(const SettingSnap& setting) { return setting.saved != setting.default_value; }

std::size_t pending_count(const PluginSnap& plugin) {
    std::size_t n = switch_differences(plugin.saved, plugin.in_effect);
    for (const ModuleSnap& m : plugin.modules) n += switch_differences(m.saved, m.in_effect);
    for (const SettingSnap& s : plugin.settings) n += setting_pending(s) ? 1 : 0;
    n += plugin.approved != plugin.approved_in_effect ? 1 : 0;
    return n;
}

std::size_t pending_count(const Snapshot& snapshot) {
    std::size_t n = 0;
    for (const PluginSnap& p : snapshot.plugins) n += pending_count(p);
    return n;
}

namespace {
bool waits_for_approval(const PluginSnap& p) {
    return (p.blocked == Blocked::WaitsForApproval && !p.approved) || !p.update_waits.empty();
}
}  // namespace

std::size_t to_approve(const Snapshot& snapshot) {
    return static_cast<std::size_t>(std::count_if(snapshot.plugins.begin(), snapshot.plugins.end(), waits_for_approval));
}

InGame module_in_game(const Snapshot& snapshot, const PluginSnap& plugin, const ModuleSnap& module) {
    if (!view(snapshot, plugin).in_this_game) return InGame::OffWithPlugin;
    if (!module.in_effect.on) return InGame::Off;
    if (kept_out(module.in_effect, module.changes_game, snapshot.game)) {
        return snapshot.game == Game::Ironman ? InGame::KeptOutOfIronman : InGame::KeptOutOfMultiplayer;
    }
    return InGame::Runs;
}

PluginView view(const Snapshot& snapshot, const PluginSnap& plugin) {
    PluginView v;
    v.load = load_state(snapshot, plugin);
    v.pending = pending_count(plugin);
    v.changed = static_cast<std::size_t>(std::count_if(plugin.settings.begin(), plugin.settings.end(), setting_changed));
    for (const PluginSnap& other : snapshot.plugins) {
        if (!other.saved.on) continue;
        if (std::find(other.requires_ids.begin(), other.requires_ids.end(), plugin.id) != other.requires_ids.end()) {
            v.needed_by.push_back(other.id);
        }
    }
    const bool own_opt_ins = plugin.modules.empty() && plugin.changes_game;
    const bool kept = own_opt_ins && kept_out(plugin.in_effect, true, snapshot.game);
    v.in_this_game = snapshot.game == Game::None
                         ? v.load.loads
                         : plugin.in_effect.on && plugin.blocked == Blocked::None &&
                               (plugin.origin != Origin::FromMod || plugin.approved_in_effect) && !kept;

    using W = LoadState::Why;
    using K = EntryLine::Kind;
    EntryLine& line = v.line;
    switch (v.load.why) {
        case W::CannotLoad: line.kind = K::CannotLoad; break;
        case W::Failed: line.kind = K::Failed; break;
        case W::WaitsForApproval: line.kind = K::WaitsForApproval; break;
        case W::Off: line.kind = K::Off; break;
        case W::NeedsPlugin: {
            line.kind = K::NeedsPlugin;
            const PluginSnap* missing = find_plugin(snapshot, v.load.missing);
            line.name = missing != nullptr ? missing->name : v.load.missing;
            break;
        }
        case W::NotInstalled:
            line.kind = K::NotInstalled;
            line.name = v.load.missing;
            break;
        case W::None: break;
    }
    if (line.kind == K::Summary && !plugin.update_waits.empty()) {
        line.kind = K::UpdateWaits;
        line.name = plugin.update_waits;
    }
    if (line.kind == K::Summary && kept) {
        line.kind = snapshot.game == Game::Ironman ? K::KeptOutOfIronman : K::KeptOutOfMultiplayer;
    }
    line.modules = plugin.modules.size();
    line.modules_on = static_cast<std::size_t>(
        std::count_if(plugin.modules.begin(), plugin.modules.end(), [](const ModuleSnap& m) { return m.saved.on; }));
    line.settings = plugin.settings.size();
    line.changed = v.changed;
    line.changes_game = plugin.changes_game;

    switch (v.load.why) {
        case W::CannotLoad:
        case W::Failed: v.glyph = Glyph::CannotLoad; break;
        case W::WaitsForApproval:
        case W::NeedsPlugin:
        case W::NotInstalled: v.glyph = Glyph::Attention; break;
        case W::Off: v.glyph = Glyph::Off; break;
        case W::None: v.glyph = !plugin.update_waits.empty() ? Glyph::Attention : kept ? Glyph::Off : Glyph::On; break;
    }
    v.needs_attention = v.glyph == Glyph::Attention || v.glyph == Glyph::CannotLoad;
    return v;
}

Where where(const Switches& s) {
    if (!s.on) return Where::Off;
    if (s.allow_ironman && s.allow_multiplayer) return Where::Everywhere;
    if (s.allow_ironman) return Where::PlusIronman;
    if (s.allow_multiplayer) return Where::PlusMultiplayer;
    return Where::Normal;
}

Switches with_where(Switches s, Where w) {
    if (w == Where::Off) {
        s.on = false;  // the opt-ins are kept while it is off
        return s;
    }
    s.on = true;
    s.allow_ironman = w == Where::PlusIronman || w == Where::Everywhere;
    s.allow_multiplayer = w == Where::PlusMultiplayer || w == Where::Everywhere;
    return s;
}

bool where_offered(const Offers& offers, Where w) {
    if ((w == Where::PlusIronman || w == Where::Everywhere) && !offers.ironman) return false;
    if ((w == Where::PlusMultiplayer || w == Where::Everywhere) && !offers.multiplayer) return false;
    return true;
}

std::vector<Edit> set_where(const PluginSnap& plugin, std::string_view module, Where w) {
    std::vector<Edit> out;
    const ModuleSnap* m = module.empty() ? nullptr : find_module(plugin, module);
    if (!module.empty() && m == nullptr) return out;
    const Switches& from = m != nullptr ? m->saved : plugin.saved;
    switch_edits(plugin, module, from, with_where(from, w), out);
    return out;
}

std::vector<Edit> set_switch(const PluginSnap& plugin, std::string_view module, Which which, bool on) {
    std::vector<Edit> out;
    const ModuleSnap* m = module.empty() ? nullptr : find_module(plugin, module);
    if (!module.empty() && m == nullptr) return out;
    if (field(m != nullptr ? m->saved : plugin.saved, which) != on) out.push_back(switch_edit(plugin, module, which, on));
    return out;
}

std::vector<Edit> set_setting(const PluginSnap& plugin, std::string_view setting, std::string_view value) {
    std::vector<Edit> out;
    for (const SettingSnap& s : plugin.settings) {
        if (s.name == setting && s.saved != value) out.push_back(setting_edit(plugin, s, std::string(value)));
    }
    return out;
}

namespace {
void collect_chain(const Snapshot& snapshot, const PluginSnap& plugin, std::string_view needed_by, Chain& chain,
                   std::vector<std::string_view>& seen) {
    if (std::find(seen.begin(), seen.end(), plugin.id) != seen.end()) return;
    seen.push_back(plugin.id);
    for (const std::string& id : plugin.requires_ids) {
        const PluginSnap* dep = find_plugin(snapshot, id);
        if (dep == nullptr) {
            if (std::find(chain.missing.begin(), chain.missing.end(), id) == chain.missing.end()) chain.missing.push_back(id);
            continue;
        }
        collect_chain(snapshot, *dep, plugin.id, chain, seen);
    }
    if (!plugin.saved.on) chain.steps.push_back({plugin.id, std::string(needed_by)});
}
}  // namespace

Chain turn_on_chain(const Snapshot& snapshot, std::string_view id) {
    Chain chain;
    const PluginSnap* plugin = find_plugin(snapshot, id);
    if (plugin == nullptr) return chain;
    std::vector<std::string_view> seen;
    collect_chain(snapshot, *plugin, "", chain, seen);
    return chain;
}

std::vector<Edit> edits_of(const Snapshot& snapshot, const Chain& chain) {
    std::vector<Edit> out;
    for (const Chain::Step& step : chain.steps) {
        if (const PluginSnap* p = find_plugin(snapshot, step.id)) out.push_back(switch_edit(*p, "", Which::On, true));
    }
    return out;
}

std::vector<Edit> reset_setting(const PluginSnap& plugin, std::string_view setting) {
    std::vector<Edit> out;
    for (const SettingSnap& s : plugin.settings) {
        if (s.name == setting && setting_changed(s)) out.push_back(setting_edit(plugin, s, s.default_value));
    }
    return out;
}

std::vector<Edit> reset_module_settings(const PluginSnap& plugin, std::string_view module) {
    std::vector<Edit> out;
    for (const SettingSnap& s : plugin.settings) {
        if (s.module == module && setting_changed(s)) out.push_back(setting_edit(plugin, s, s.default_value));
    }
    return out;
}

std::vector<Edit> reset_plugin(const PluginSnap& plugin) {
    std::vector<Edit> out;
    for (const SettingSnap& s : plugin.settings) {
        if (setting_changed(s)) out.push_back(setting_edit(plugin, s, s.default_value));
    }
    for (const ModuleSnap& m : plugin.modules) {
        switch_edits(plugin, m.id, m.saved, Switches{m.default_on, false, false}, out);
    }
    Switches plugin_default = plugin.saved;  // never the plugin's own switch
    plugin_default.allow_ironman = false;
    plugin_default.allow_multiplayer = false;
    switch_edits(plugin, "", plugin.saved, plugin_default, out);
    return out;
}

std::vector<Edit> undo_pending(const PluginSnap& plugin) {
    std::vector<Edit> out;
    switch_edits(plugin, "", plugin.saved, plugin.in_effect, out);
    for (const ModuleSnap& m : plugin.modules) switch_edits(plugin, m.id, m.saved, m.in_effect, out);
    for (const SettingSnap& s : plugin.settings) {
        if (setting_pending(s)) out.push_back(setting_edit(plugin, s, s.in_effect));
    }
    return out;
}

std::vector<Edit> undo_pending(const Snapshot& snapshot) {
    std::vector<Edit> out;
    for (const PluginSnap& p : snapshot.plugins) {
        std::vector<Edit> mine = undo_pending(p);
        out.insert(out.end(), mine.begin(), mine.end());
    }
    return out;
}

Match find(const PluginSnap& plugin, std::string_view query) {
    Match m;
    if (query.empty() || contains(plugin.name, query) || contains(plugin.id, query)) {
        m.where = Match::Where::Name;
        return m;
    }
    for (const ModuleSnap& mod : plugin.modules) {
        if (contains(mod.name, query)) {
            m.modules.push_back(mod.id);
            if (m.where == Match::Where::None) {
                m.where = Match::Where::Module;
                m.what = mod.name;
            }
        }
    }
    for (const SettingSnap& s : plugin.settings) {
        if (contains(s.label, query) || contains(s.description, query)) {
            m.settings.push_back(s.name);
            if (m.where == Match::Where::None) {
                m.where = Match::Where::Setting;
                m.what = s.label;
            }
        }
    }
    return m;
}

bool has_chip(const Snapshot& snapshot, const PluginSnap& plugin, Chip chip) {
    switch (chip) {
        case ChipNone: return true;
        case ChipChangesGame: return plugin.changes_game;
        case ChipPending: return pending_count(plugin) > 0;
        case ChipChanged:
            return std::any_of(plugin.settings.begin(), plugin.settings.end(), setting_changed);
        case ChipAttention: return view(snapshot, plugin).needs_attention;
        case ChipOff: return load_state(snapshot, plugin).why == LoadState::Why::Off;
    }
    return false;
}

bool passes(const Snapshot& snapshot, const PluginSnap& plugin, std::string_view query, unsigned chips) {
    if (find(plugin, query).where == Match::Where::None) return false;
    for (const Chip chip : {ChipAttention, ChipChangesGame, ChipChanged, ChipPending, ChipOff}) {
        if ((chips & chip) != 0 && !has_chip(snapshot, plugin, chip)) return false;
    }
    return true;
}

std::size_t chip_count(const Snapshot& snapshot, Chip chip) {
    return static_cast<std::size_t>(std::count_if(snapshot.plugins.begin(), snapshot.plugins.end(),
                                                  [&](const PluginSnap& p) { return has_chip(snapshot, p, chip); }));
}

SectionCounts section_counts(const Snapshot& snapshot, Origin origin) {
    SectionCounts c;
    for (const PluginSnap& p : snapshot.plugins) {
        if (p.origin != origin) continue;
        ++c.plugins;
        const PluginView v = view(snapshot, p);
        c.attention += v.glyph == Glyph::Attention ? 1 : 0;
        c.cannot_load += v.glyph == Glyph::CannotLoad ? 1 : 0;
        c.waits_for_approval += waits_for_approval(p) ? 1 : 0;
    }
    return c;
}

SettingsLayout settings_layout(const PluginSnap& plugin, std::string_view module) {
    std::vector<std::size_t> shown;
    std::vector<std::size_t> advanced;
    for (std::size_t i = 0; i < plugin.settings.size(); ++i) {
        if (plugin.settings[i].module != module) continue;
        (plugin.settings[i].advanced ? advanced : shown).push_back(i);
    }
    const auto ordered = [&](const std::vector<std::size_t>& indices) {
        std::vector<std::string_view> groups;
        for (const std::size_t i : indices) groups.push_back(plugin.settings[i].group);
        std::vector<std::size_t> out;
        for (const std::size_t position : settings_display_order(groups)) out.push_back(indices[position]);
        return out;
    };
    return {ordered(shown), ordered(advanced)};
}

std::vector<ModuleGroup> module_groups(const PluginSnap& plugin) {
    std::vector<ModuleGroup> out;
    for (std::size_t i = 0; i < plugin.modules.size(); ++i) {
        const ModuleSnap& m = plugin.modules[i];
        auto it = std::find_if(out.begin(), out.end(), [&](const ModuleGroup& g) { return g.name == m.group; });
        if (it == out.end()) {
            out.push_back({m.group, {}, 0});
            it = out.end() - 1;
        }
        it->modules.push_back(i);
        it->on += m.saved.on ? 1 : 0;
    }
    // Modules without a group come first, as settings without one do.
    std::stable_partition(out.begin(), out.end(), [](const ModuleGroup& g) { return g.name.empty(); });
    return out;
}

}  // namespace anvil::ui
