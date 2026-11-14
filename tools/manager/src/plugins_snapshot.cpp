#include "plugins_snapshot.hpp"

#include "i18n/message.hpp"
#include "i18n/translate.hpp"

#include <algorithm>

namespace anvil::manager {
namespace fs = std::filesystem;
namespace mui = anvil::ui;

namespace {

bool listed(const std::vector<std::string>& list, const std::string& id) {
    return std::find(list.begin(), list.end(), id) != list.end();
}

fs::path widen(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

// The resolver's reasons that the model works out itself: the player's
// switch and dependencies that do not load.
bool model_reason(const i18n::Message& why) {
    return why.code == "disabled" || why.code == "all_disabled" || why.code == "requires_missing" ||
           why.code == "needs_anvil_game";
}

std::vector<std::string> required(const Manifest& m) {
    std::vector<std::string> ids;
    for (const Dependency& d : m.dependencies) {
        if (!d.optional) ids.push_back(d.id);
    }
    // affects_gameplay = true needs anvil-game (design §13), which the resolver adds.
    if (affects_gameplay_anywhere(m) && m.id != "anvil-game" && !listed(ids, "anvil-game")) ids.push_back("anvil-game");
    return ids;
}

void fill_from_manifest(mui::PluginSnap& p, PluginSource& source, const Manifest& m, const AnvilSettings& toml,
                        const PluginText& text) {
    p.id = m.id;
    p.name = m.name.empty() ? m.id : text(m.folder, m.name);
    p.version = m.version.to_string();
    p.description = m.description.empty() ? std::string() : text(m.folder, m.description);
    p.changes_game = affects_gameplay_anywhere(m);
    p.offers = {m.allow_ironman == Allow::OptIn, m.allow_multiplayer == Allow::OptIn};
    p.saved = {!listed(toml.disabled_plugins, m.id), listed(toml.allow_ironman, m.id), listed(toml.allow_multiplayer, m.id)};
    p.requires_ids = required(m);
    for (const Module& mod : m.modules) {
        const std::string key = module_key(m, mod);
        mui::ModuleSnap s;
        s.id = mod.id;
        s.name = mod.name.empty() ? mod.id : text(m.folder, mod.name);
        s.description = mod.description.empty() ? std::string() : text(m.folder, mod.description);
        s.group = mod.group.empty() ? std::string() : text(m.folder, mod.group);
        s.changes_game = mod.affects_gameplay;
        s.default_on = mod.enabled_by_default;
        s.offers = {mod.allow_ironman == Allow::OptIn, mod.allow_multiplayer == Allow::OptIn};
        s.saved = {module_enabled(toml, key, mod.enabled_by_default), listed(toml.allow_ironman, key),
                   listed(toml.allow_multiplayer, key)};
        s.in_effect = s.saved;
        p.modules.push_back(std::move(s));
    }
    source.manifest = m;
    source.player_file = fs::path("anvil") / "data" / widen(m.id) / "settings.toml";
}

void fill_settings(mui::PluginSnap& p, PluginSource& source, const fs::path& game_dir, const PluginText& text) {
    const Manifest& m = *source.manifest;
    source.player_file = game_dir / source.player_file;
    if (m.settings.empty()) return;
    const SettingsFileResult schema = load_settings(m.folder / widen(m.settings));
    if (!schema.file) {
        source.settings_error = schema.error;
        return;
    }
    source.schema = *schema.file;
    PlayerSettings player = load_player_settings(*source.schema, source.player_file);
    source.settings_warnings = std::move(player.warnings);
    for (const Setting& setting : source.schema->settings) {
        const SettingValue value = player.values[setting.id].value_or(setting.default_value);
        source.values.push_back(value);
        mui::SettingSnap s;
        s.name = setting.name;
        s.label = setting.label.empty() ? setting.name : text(m.folder, setting.label);
        s.description = setting.description.empty() ? std::string() : text(m.folder, setting.description);
        s.group = setting.group.empty() ? std::string() : text(m.folder, setting.group);
        s.module = setting.module;
        s.advanced = setting.advanced;
        s.counts = setting_counts(setting, m);
        s.restart = setting.restart;
        s.default_value = value_text(setting, setting.default_value);
        s.saved = value_text(setting, value);
        s.in_effect = s.saved;
        p.settings.push_back(std::move(s));
    }
}

}  // namespace

std::string value_text(const Setting& setting, const SettingValue& value) { return setting_toml(setting, value); }

PluginsSnapshot build_plugins_snapshot(const fs::path& game_dir, const cli::GameReport& report, const AnvilSettings& toml,
                                       const std::vector<std::string>& standard_plugins, const PluginText& text) {
    PluginsSnapshot out;
    for (const cli::GamePlugin& plugin : report.plugins) {
        mui::PluginSnap p;
        PluginSource source;
        source.folder = plugin.folder;
        if (const auto& m = plugin.report.manifest) {
            fill_from_manifest(p, source, *m, toml, text);
            source.standard = listed(standard_plugins, m->id);
            p.origin = source.standard ? mui::Origin::WithAnvil : mui::Origin::Own;
            fill_settings(p, source, game_dir, text);
        } else {
            // A folder whose plugin.toml cannot be read: an entry that cannot load.
            p.id = plugin.folder;
            p.name = plugin.folder;
            p.saved.on = !listed(toml.disabled_plugins, plugin.folder);
            p.blocked = mui::Blocked::CannotLoad;
        }
        const auto skipped = std::find_if(report.skipped.begin(), report.skipped.end(), [&](const cli::SkippedPlugin& s) {
            return s.mod.empty() && s.folder == plugin.folder;
        });
        if (skipped != report.skipped.end() && !skipped->disabled && !model_reason(skipped->why)) {
            p.blocked = mui::Blocked::CannotLoad;
            p.blocked_reason = i18n::text(skipped->why);
        }
        p.in_effect = p.saved;
        out.model.plugins.push_back(std::move(p));
        out.sources.push_back(std::move(source));
    }
    for (const cli::GameModPlugin& mod : report.mod_plugins) {
        mui::PluginSnap p;
        PluginSource source;
        source.mod = &mod;
        const auto& manifest = mod.report.manifest ? mod.report.manifest : mod.offered().manifest;
        if (manifest) {
            fill_from_manifest(p, source, *manifest, toml, text);
            fill_settings(p, source, game_dir, text);
        } else {
            p.id = mod.id;
            p.name = mod.id;
            p.saved.on = !listed(toml.disabled_plugins, mod.id);
        }
        p.origin = mui::Origin::FromMod;
        p.version = mod.state == "update-pending" ? mod.approved_version : mod.version;
        p.approved = !mod.approved_hash.empty();
        p.approved_in_effect = p.approved;
        if (mod.state == "update-pending") p.update_waits = mod.version;
        if (mod.state == "pending") {
            p.blocked = mui::Blocked::WaitsForApproval;
            p.approved = p.approved_in_effect = false;
        } else if (mod.state != "approved" && mod.state != "update-pending") {
            p.blocked = mui::Blocked::CannotLoad;  // conflict, shadowed, invalid
            p.blocked_reason = mod.why.code.empty() && mod.why.text.empty() ? mod.reason : i18n::text(mod.why);
        } else if (mod.status != "loaded" && !model_reason(mod.skip_why) && !mod.skip_reason.empty()) {
            p.blocked = mui::Blocked::CannotLoad;
            p.blocked_reason = i18n::text(mod.skip_why);
        }
        p.in_effect = p.saved;
        out.model.plugins.push_back(std::move(p));
        out.sources.push_back(std::move(source));
    }
    return out;
}

void apply_last_start(PluginsSnapshot& snapshot, const LastStart& last) {
    AnvilSettings lists;
    lists.disabled_plugins = last.disabled_plugins;
    lists.allow_ironman = last.allow_ironman;
    lists.allow_multiplayer = last.allow_multiplayer;
    lists.enabled_modules = last.enabled_modules;
    lists.disabled_modules = last.disabled_modules;
    for (mui::PluginSnap& p : snapshot.model.plugins) {
        p.in_effect = {!listed(lists.disabled_plugins, p.id), listed(lists.allow_ironman, p.id), listed(lists.allow_multiplayer, p.id)};
        if (p.origin == mui::Origin::FromMod) {
            p.approved_in_effect = std::any_of(last.approvals.begin(), last.approvals.end(),
                                               [&](const LastStart::Approval& a) { return a.id == p.id; });
        }
        for (mui::ModuleSnap& m : p.modules) {
            const std::string key = p.id + "." + m.id;
            m.in_effect = {module_enabled(lists, key, m.default_on), listed(lists.allow_ironman, key), listed(lists.allow_multiplayer, key)};
        }
        // The record has the values of each plugin that resolved; one that did not keeps its own.
        const auto values = last.settings.find(p.id);
        for (mui::SettingSnap& s : p.settings) {
            if (values == last.settings.end()) {
                s.in_effect = s.saved;
                continue;
            }
            const auto value = values->second.find(s.name);
            s.in_effect = value != values->second.end() ? value->second : s.saved;
        }
    }
}

bool apply_setting_edit(PluginSource& source, const mui::Edit& edit, std::string& error) {
    if (!source.schema) {
        error = "no settings";
        return false;
    }
    for (const Setting& setting : source.schema->settings) {
        if (setting.name != edit.setting) continue;
        SettingValue& value = source.values[setting.id];
        if (value_text(setting, setting.default_value) == edit.value) {
            value = setting.default_value;
            return true;
        }
        if (value_text(setting, value) == edit.value) return true;
        error = "unknown value " + edit.value + " for " + setting.name;
        return false;
    }
    error = "no setting " + edit.setting;
    return false;
}

}  // namespace anvil::manager
