#include "plugins_model.hpp"

#include <algorithm>
#include <cstddef>

namespace overlay {
namespace {

namespace mui = anvil::ui;

bool listed(const std::vector<std::string>& list, const std::string& id) {
    return std::find(list.begin(), list.end(), id) != list.end();
}

// The resolver's reasons that the model works out itself: the player's
// switch and dependencies that do not load.
bool model_reason(const anvil::i18n::Message& why) {
    return why.code == "disabled" || why.code == "all_disabled" || why.code == "requires_missing" ||
           why.code == "needs_anvil_game";
}

// "<plugin>.<module>" -> "<module>"
std::string module_id(const ModuleRow& m) {
    const size_t dot = m.key.find('.');
    return dot == std::string::npos ? m.key : m.key.substr(dot + 1);
}

// A switch as this start read it and as the next start will; `fallback`
// from a core before 1.23, which has no switch_state (its opt-ins unknown: off).
struct SwitchPair {
    mui::Switches saved, in_effect;
    std::string refusal;
};

SwitchPair switches_of(const ControlClient& control, const std::string& key, bool this_start_on, bool next_start_on) {
    SwitchPair out;
    out.saved.on = next_start_on;
    out.in_effect.on = this_start_on;
    if (!control.has_switches()) return out;
    if (const auto on = control.switch_state(key, ANVIL_CONTROL_SWITCH_ON)) {
        out.saved.on = on->next_start;
        out.in_effect.on = on->this_start;
        out.refusal = on->refusal;
    }
    if (const auto ironman = control.switch_state(key, ANVIL_CONTROL_SWITCH_IRONMAN)) {
        out.saved.allow_ironman = ironman->next_start;
        out.in_effect.allow_ironman = ironman->this_start;
    }
    if (const auto multiplayer = control.switch_state(key, ANVIL_CONTROL_SWITCH_MULTIPLAYER)) {
        out.saved.allow_multiplayer = multiplayer->next_start;
        out.in_effect.allow_multiplayer = multiplayer->this_start;
    }
    return out;
}

void fill_modules(const ControlClient& control, mui::PluginSnap& p, const std::vector<ModuleRow>& modules) {
    for (const ModuleRow& m : modules) {
        mui::ModuleSnap s;
        s.id = module_id(m);
        s.name = m.name.empty() ? s.id : m.name;
        s.description = m.description;
        s.group = m.group;
        s.changes_game = m.gameplay;
        s.default_on = m.on_by_default;
        s.offers = {m.offers_ironman, m.offers_multiplayer};
        // A module of a plugin that is off at this start is "skipped": switch_state says whether it is on.
        const SwitchPair sw = switches_of(control, m.key, m.enabled, m.enabled);
        s.saved = sw.saved;
        s.in_effect = sw.in_effect;
        p.changes_game = p.changes_game || m.gameplay;
        p.modules.push_back(std::move(s));
    }
}

void fill_settings(const ControlClient& control, mui::PluginSnap& p, OverlaySource& source,
                   const std::vector<ModuleRow>& modules) {
    const uint32_t count = control.setting_count(p.id);
    for (AnvilSetting id = 0; id < count; ++id) {
        const auto info = control.setting_info(p.id, id);
        if (!info) continue;
        OverlaySetting o;
        o.id = id;
        o.info = *info;
        if (info->type == ANVIL_CONTROL_SETTING_ENUM) o.options = control.setting_options(p.id, id);
        mui::SettingSnap s;
        s.name = info->name != nullptr ? info->name : "";
        s.label = info->label != nullptr && *info->label != '\0' ? info->label : s.name;
        s.description = info->description != nullptr ? info->description : "";
        s.group = info->group != nullptr ? info->group : "";
        if (info->size > offsetof(AnvilControlSetting, locked)) {
            if (info->module >= 0 && static_cast<size_t>(info->module) < modules.size()) {
                s.module = module_id(modules[static_cast<size_t>(info->module)]);
            }
            s.advanced = info->advanced != 0;
            s.counts = info->counts != 0;
            s.locked = info->locked != 0;
        }
        s.restart = info->restart != 0;
        s.default_value = control.setting_value(p.id, id, ANVIL_CONTROL_VALUE_DEFAULT);
        s.saved = control.setting_value(p.id, id, ANVIL_CONTROL_VALUE_SAVED);
        s.in_effect = control.setting_value(p.id, id, ANVIL_CONTROL_VALUE_CURRENT);
        p.settings.push_back(std::move(s));
        source.settings.push_back(std::move(o));
    }
}

std::vector<std::string> required(const ControlClient& control, const std::string& id) {
    std::vector<std::string> ids;
    for (const Requirement& r : control.requirements(id)) {
        if (!r.optional && !listed(ids, r.id)) ids.push_back(r.id);
    }
    return ids;
}

// How the start ended for a plugin, when the reason is its own.
void fill_blocked(mui::PluginSnap& p, const PluginRow& row) {
    if (row.state == "failed") {
        p.blocked = mui::Blocked::Failed;
        p.blocked_reason = row.reason_text();
    } else if (row.state == "skipped" && !row.disabled && !row.awaiting_approval && !model_reason(row.why)) {
        p.blocked = mui::Blocked::CannotLoad;
        p.blocked_reason = row.reason_text();
    }
}

mui::Game game_of(const AnvilControlStatus& status) {
    // Outside a session the menu runs what this start loaded: a normal game's rules.
    if (!status.session) return mui::Game::Normal;
    if (status.multiplayer) return mui::Game::Multiplayer;
    if (status.ironman) return mui::Game::Ironman;
    return mui::Game::Normal;
}

}  // namespace

OverlayPlugins build_overlay_plugins(const ControlClient& control, const AnvilControlStatus& status,
                                     const std::vector<PluginRow>& plugins, const std::vector<ModRow>& mods,
                                     const std::vector<std::string>& standard) {
    OverlayPlugins out;
    out.model.game = game_of(status);
    for (size_t r = 0; r < plugins.size(); ++r) {
        const PluginRow& row = plugins[r];
        if (!row.mod.empty()) continue;  // with its mod's entry below
        mui::PluginSnap p;
        OverlaySource source;
        source.row = r;
        source.known = !row.name.empty();
        p.id = row.id;
        p.name = row.name.empty() ? row.id : control.plugin_text(row.id, row.name.c_str());
        p.version = row.version;
        p.origin = listed(standard, row.id) ? mui::Origin::WithAnvil : mui::Origin::Own;
        p.changes_game = row.gameplay;
        p.offers = {row.offers_ironman, row.offers_multiplayer};
        const SwitchPair sw = switches_of(control, row.id, !row.disabled, !row.disabled_next);
        p.saved = sw.saved;
        p.in_effect = sw.in_effect;
        source.refusal = sw.refusal;
        p.needed_here = sw.refusal == "needed";
        p.requires_ids = required(control, row.id);
        fill_modules(control, p, row.modules);
        fill_settings(control, p, source, row.modules);
        if (!source.known) p.blocked = mui::Blocked::CannotLoad;
        fill_blocked(p, row);
        out.model.plugins.push_back(std::move(p));
        out.sources.push_back(std::move(source));
    }
    for (size_t k = 0; k < mods.size(); ++k) {
        const ModRow& m = mods[k];
        const auto row = std::find_if(plugins.begin(), plugins.end(),
                                      [&](const PluginRow& p) { return p.id == m.id && p.mod == m.mod; });
        mui::PluginSnap p;
        OverlaySource source;
        source.mod = k;
        if (row != plugins.end()) source.row = static_cast<size_t>(row - plugins.begin());
        p.id = m.id;
        p.name = m.name.empty() ? m.id : m.name;
        p.version = m.state == "update-pending" && !m.approved_version.empty() ? m.approved_version : m.version;
        p.description = m.description;
        p.origin = mui::Origin::FromMod;
        p.changes_game = m.gameplay;
        if (row != plugins.end()) {
            source.known = control.has_switches() && control.switch_state(m.id, ANVIL_CONTROL_SWITCH_ON).has_value();
            p.offers = {row->offers_ironman, row->offers_multiplayer};
            const SwitchPair sw = switches_of(control, m.id, !row->disabled, !row->disabled_next);
            p.saved = sw.saved;
            p.in_effect = sw.in_effect;
            source.refusal = sw.refusal;
            p.requires_ids = required(control, m.id);
            // A loaded plugin's modules carry their state; one waiting for approval has them as declared.
            fill_modules(control, p, row->modules.empty() ? m.modules : row->modules);
            fill_settings(control, p, source, row->modules);
        } else {
            fill_modules(control, p, m.modules);
        }
        p.approved_in_effect = !m.approved_hash.empty();
        p.approved = m.approved_next || (p.approved_in_effect && !m.revoked_next);
        if (m.state == "update-pending" && !m.approved_next) p.update_waits = m.version;
        if (m.state == "pending") {
            p.blocked = mui::Blocked::WaitsForApproval;
        } else if (m.state != "approved" && m.state != "update-pending") {
            p.blocked = mui::Blocked::CannotLoad;  // conflict, shadowed, invalid
            p.blocked_reason = m.reason_text();
        } else if (row != plugins.end()) {
            fill_blocked(p, *row);
        }
        out.model.plugins.push_back(std::move(p));
        out.sources.push_back(std::move(source));
    }
    return out;
}

std::vector<std::string> standard_plugin_ids() {
#ifdef ANVIL_OVERLAY_STANDARD_PLUGINS
    return split_list(ANVIL_OVERLAY_STANDARD_PLUGINS);
#else
    return {"anvil-game", "anvil-overlay"};
#endif
}

}  // namespace overlay
