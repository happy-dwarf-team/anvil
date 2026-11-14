// The Manager's Plugins tab (ADR 0013, docs/ui-guidelines.md §5). The overlay
// draws the same tab: src/ui/plugins_view.cpp draws the list and the detail
// from the shared view model (src/ui/plugin_model.hpp). This file is the
// Manager's host for it: it reads the folder, edits values with the settings
// schema, writes anvil.toml and the players' settings files, and adds what only
// the Manager says (authors, problems, catalog updates, Remove).
#include "core/anvil_toml_edit.hpp"
#include "core/log.hpp"
#include "i18n/translate.hpp"
#include "manager_app.hpp"
#include "mods.hpp"
#include "ui.hpp"
#include "ui/plugin_widgets.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <span>
#include <string_view>

namespace anvil::manager {

namespace fs = std::filesystem;
namespace mui = anvil::ui;
using namespace ui;
using i18n::tr;
using i18n::trf;

std::string mod_source(const cli::GameModPlugin& p) {
    return trf("mod '{0}' ({1})", {p.mod, p.workshop_id.empty() ? std::string(tr("local mod")) : trf("Workshop {0}", {p.workshop_id})});
}

// What a plugin from a mod waits for, or does, in the player's words.
std::string mod_state_text(const cli::GameModPlugin& p) {
    if (p.state == "approved") return p.status == "loaded" ? std::string(tr("approved: loads")) : trf("approved, but {0}", {i18n::text(p.skip_why)});
    if (p.state == "update-pending") return trf("an update waits for approval; the approved {0} loads", {p.approved_version});
    if (p.state == "pending") return tr("waits for approval: does not load");
    if (p.state == "conflict") return tr("conflict: does not load");
    if (p.state == "shadowed") return tr("a plugin in anvil/plugins has this id and loads instead");
    return tr("cannot be approved: does not load");
}

namespace {

std::span<const cli::Problem> problems_shown(const cli::CheckReport& report) {
    const std::span<const cli::Problem> all(report.problems);
    return report.exe_build.empty() ? all.first(std::min<size_t>(all.size(), 1)) : all;
}

// A setting's value as the player reads it.
std::string shown_value(const Setting& setting, const SettingValue& value) {
    switch (setting.type) {
        case SettingType::Bool: return value.integer != 0 ? tr("on") : tr("off");
        case SettingType::Enum: {
            const auto i = static_cast<size_t>(value.integer);
            return i < setting.options.size() ? (setting.options[i].label.empty() ? setting.options[i].name : setting.options[i].label)
                                              : std::string("?");
        }
        case SettingType::Key: return value.integer == 0 ? std::string(tr("no key")) : key_name(value.integer);
        case SettingType::String: return value.text;
        default: return setting_toml(setting, value);
    }
}

}  // namespace

// The Manager as the host of the shared Plugins tab.
struct ManagerApp::PluginsHost final : mui::PluginsHost {
    ManagerApp& app;
    explicit PluginsHost(ManagerApp& a) : app(a) {}

    PluginSource& source(size_t i) const { return app.plugins_->sources[i]; }

    bool read_only() const override { return app.read_only(); }
    void apply(const std::vector<mui::Edit>& edits) override { app.apply_edits(edits); }
    bool known(size_t i) const override { return source(i).manifest.has_value(); }
    void draw_setting_control(size_t i, size_t s, bool disabled) override { app.draw_setting_control(i, s, disabled); }

    // The snapshot holds values as settings.toml writes them (value_text).
    std::string shown_value(size_t i, size_t s, const std::string& value) const override {
        const PluginSource& src = source(i);
        if (!src.schema || s >= src.schema->settings.size()) return value;
        const Setting& setting = src.schema->settings[s];
        if (value == value_text(setting, setting.default_value)) return manager::shown_value(setting, setting.default_value);
        if (s < src.values.size() && value == value_text(setting, src.values[s])) return manager::shown_value(setting, src.values[s]);
        return value;
    }

    bool has_header_buttons(size_t i) const override {
        return app.plugins_->model.plugins[i].origin == mui::Origin::Own && !source(i).folder.empty();
    }
    void draw_header_buttons(size_t i) override {
        ImGui::SameLine();
        if (ImGui::Button(tr("Remove..."))) app.ask_remove(source(i).folder);
    }

    void draw_facts(size_t i) override {
        const PluginSource& src = source(i);
        if (src.manifest) {
            const Manifest& m = *src.manifest;
            weak_wrapped(m.authors.empty() ? trf("id {0}", {m.id}) : trf("id {0}, by {1}", {m.id, join(m.authors)}));
        }
        if (src.mod != nullptr) app.draw_mod_approval(i);
    }

    void draw_notes(size_t i) override {
        const mui::PluginSnap& p = app.plugins_->model.plugins[i];
        const PluginSource& src = source(i);
        const cli::GameReport& report = *app.snapshot_->report;
        // What needs a look, each in its banner: what anvil check finds, overlaps, the settings file.
        std::vector<std::string> warnings;
        if (!src.folder.empty()) {
            for (const cli::GamePlugin& gp : report.plugins) {
                if (gp.folder != src.folder) continue;
                for (const cli::Problem& problem : problems_shown(gp.report)) {
                    // The reason it cannot load already says it.
                    if (p.blocked == mui::Blocked::CannotLoad && p.blocked_reason.find(problem.message) != std::string::npos) continue;
                    warnings.push_back(problem.message);
                }
            }
        }
        for (const cli::OverlapWarning& warning : report.warnings) {
            if (warning.plugin == p.id || warning.other == p.id) warnings.push_back(warning.message);
        }
        for (const std::string& warning : src.settings_warnings) warnings.push_back(trf("Ignored in your file: {0}", {warning}));
        if (!src.settings_error.empty()) banner("settings-error", Tone::Bad, src.settings_error);
        if (!warnings.empty()) {
            begin_banner("warnings", Tone::Warn);
            for (const std::string& warning : warnings) wrapped(warning);
            end_banner(Tone::Warn);
        }
        const std::vector<const CatalogPlugin*> updates = app.catalog_updates_here();
        if (const auto update = std::find_if(updates.begin(), updates.end(), [&](const CatalogPlugin* c) { return c->id == p.id; });
            update != updates.end()) {
            begin_banner("update", Tone::Info);
            ImGui::TextUnformatted(trf("{0} is available in the Catalog.", {(*update)->version}).c_str());
            ImGui::SameLine();
            if (text_button(tr("Open the Catalog"))) app.page_ = "Catalog";
            end_banner(Tone::Info);
        }
    }

    // From mods: shown with its playset's warnings even when it has no plugins.
    bool show_empty_section(mui::Origin origin) const override {
        const cli::GameReport& report = *app.snapshot_->report;
        return origin == mui::Origin::FromMod && (!report.mod_warnings.empty() || !report.playset_found);
    }
    void draw_section_note(mui::Origin origin, const mui::SectionCounts& counts) override {
        if (origin != mui::Origin::FromMod) return;
        const cli::GameReport& report = *app.snapshot_->report;
        std::string summary = trf("{0} plugin(s) in {1} enabled mod(s)", {std::to_string(counts.plugins), std::to_string(report.enabled_mods)});
        if (counts.waits_for_approval > 0) summary += ", " + trf("{0} waiting for approval", {std::to_string(counts.waits_for_approval)});
        if (counts.cannot_load > 0) summary += ", " + trf("{0} cannot load", {std::to_string(counts.cannot_load)});
        weak_wrapped(summary);
        if (!report.playset_found || !report.mod_warnings.empty()) {
            begin_banner("playset", Tone::Warn);
            if (!report.playset_found) wrapped(trf("No dlc_load.json in {0}: no mods are enabled.", {report.user_dir}));
            for (const std::string& w : report.mod_warnings) wrapped(w);
            end_banner(Tone::Warn);
        }
    }
};

void ManagerApp::rebuild_plugins() {
    std::optional<PluginsSnapshot> old = std::move(plugins_);
    plugins_.reset();
    if (!snapshot_ || !snapshot_->report || !package_) return;
    plugins_ = build_plugins_snapshot(game_, *snapshot_->report, snapshot_->anvil_toml, package_->standard_plugins,
                                      [this](const fs::path& folder, const std::string& english) {
                                          return plugin_text(folder, english);
                                      });
    setting_texts_.clear();
    // What waits for the next start is measured against the record of the last start (ADR 0013,
    // point 8). Without one (a folder no Anvil 1.23 has started from), what the files held when the
    // Manager first read this folder, or when the game last started from it, is what is in effect: a
    // switch saved since then waits for the next start as a setting does. The old snapshot is the same
    // folder's (choose_game drops it).
    if (snapshot_->last_start) {
        apply_last_start(*plugins_, *snapshot_->last_start);
        return;
    }
    if (!old) return;
    for (mui::PluginSnap& p : plugins_->model.plugins) {
        const auto was = std::find_if(old->model.plugins.begin(), old->model.plugins.end(),
                                      [&](const mui::PluginSnap& o) { return o.id == p.id; });
        if (was == old->model.plugins.end()) continue;
        p.in_effect = was->in_effect;
        p.approved_in_effect = was->approved_in_effect;
        for (mui::ModuleSnap& m : p.modules) {
            for (const mui::ModuleSnap& o : was->modules) {
                if (o.id == m.id) m.in_effect = o.in_effect;
            }
        }
        for (mui::SettingSnap& s : p.settings) {
            for (const mui::SettingSnap& o : was->settings) {
                if (o.name == s.name) s.in_effect = o.in_effect;
            }
        }
    }
}

void ManagerApp::plugins_take_effect() {
    if (!plugins_) return;
    for (mui::PluginSnap& p : plugins_->model.plugins) {
        p.in_effect = p.saved;
        p.approved_in_effect = p.approved;
        for (mui::ModuleSnap& m : p.modules) m.in_effect = m.saved;
        for (mui::SettingSnap& s : p.settings) s.in_effect = s.saved;
    }
}

size_t ManagerApp::plugin_index(std::string_view id) const {
    if (!plugins_) return SIZE_MAX;
    for (size_t i = 0; i < plugins_->model.plugins.size(); ++i) {
        if (plugins_->model.plugins[i].id == id) return i;
    }
    return SIZE_MAX;
}

void ManagerApp::draw_plugins_tab() {
    const auto& report = snapshot_->report;
    ImGui::Spacing();
    ImGui::BeginDisabled(read_only() || !report || !report->cannot_check.empty());
    if (ImGui::Button(tr("Install a plugin from a zip..."))) {
        if (const auto file = pick_zip(window_)) open_zip(*file);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (text_button(tr("Open the plugins folder"))) open_folder(game_ / "anvil" / "plugins");
    ImGui::SameLine();
    ImGui::BeginDisabled(pending_.valid());
    if (text_button(tr("Check again"))) refresh();
    ImGui::EndDisabled();
    // The hint follows the buttons where it fits, else it takes the next line.
    const char* hint = tr("You can also drop a plugin zip on this window.");
    ImGui::SameLine();
    if (ImGui::CalcTextSize(hint).x > ImGui::GetContentRegionAvail().x) ImGui::NewLine();
    weak(hint);

    ImGui::Spacing();
    if (!report) {
        banner("no-anvil", Tone::Info, tr("Anvil is not installed in this folder: no plugins load."));
        return;
    }
    if (!report->cannot_check.empty()) {
        banner("cannot-check", Tone::Bad, trf("Cannot check the plugins: {0}", {report->cannot_check}));
        return;
    }
    if (!plugins_) rebuild_plugins();
    if (!plugins_) return;
    if (!report->problems.empty()) {
        begin_banner("problems", Tone::Bad);
        for (const cli::Problem& problem : report->problems) wrapped(u8(fs::path(widen(problem.file)).filename()) + ": " + problem.message);
        end_banner(Tone::Bad);
    }
    if (!report->enabled) banner("disabled", Tone::Warn, tr("anvil.toml says enabled = false: no plugin loads."));
    if (snapshot_->installation && !snapshot_->installation->installed()) {
        banner("not-installed", Tone::Warn, tr("version.dll is not Anvil's or missing: nothing below loads until Anvil is installed."));
    }
    if (running_) banner("running", Tone::Warn, tr("The game is running: change plugins in the game's overlay, or here after it closes."));

    PluginsHost host(*this);
    const mui::Snapshot& model = plugins_->model;
    // What waits for the player shows from anywhere (ui-guidelines §5, "Plugins from mods").
    plugins_tab_.draw_waiting(model);
    plugins_tab_.draw_pending(model, host);
    ImGui::Spacing();
    plugins_tab_.draw_panes(model, host);
}

void ManagerApp::draw_plugins_confirm() {
    PluginsHost host(*this);
    plugins_tab_.draw_confirm(host);
}

void ManagerApp::draw_setting_control(size_t i, size_t s, bool disabled) {
    PluginSource& source = plugins_->sources[i];
    if (!source.schema) return;
    const Setting& setting = source.schema->settings[s];
    SettingValue& value = source.values[s];
    const mui::SettingSnap& snap = plugins_->model.plugins[i].settings[s];
    std::string& text = setting_texts_[plugins_->model.plugins[i].id + "/" + setting.name];
    if (text.empty()) text = setting.type == SettingType::Key ? (value.integer == 0 ? std::string() : key_name(value.integer)) : value.text;
    (void)disabled;  // the shared row disables the control
    bool commit = false;
    switch (setting.type) {
        case SettingType::Bool: {
            bool on = value.integer != 0;
            if (ImGui::Checkbox(snap.label.c_str(), &on)) {
                value.integer = on ? 1 : 0;
                commit = true;
            }
            break;
        }
        case SettingType::Int: {
            const int64_t step = setting.step ? setting.step->integer : 1;
            const bool changed = ImGui::InputScalar(snap.label.c_str(), ImGuiDataType_S64, &value.integer, &step);
            if (setting.min) value.integer = std::max(value.integer, setting.min->integer);
            if (setting.max) value.integer = std::min(value.integer, setting.max->integer);
            commit = (changed && !ImGui::IsItemActive()) || ImGui::IsItemDeactivatedAfterEdit();
            break;
        }
        case SettingType::Float: {
            const double step = setting.step ? setting.step->real : 0.0;
            const bool changed = ImGui::InputDouble(snap.label.c_str(), &value.real, step, step * 10, "%.4g");
            if (setting.min) value.real = std::max(value.real, setting.min->real);
            if (setting.max) value.real = std::min(value.real, setting.max->real);
            commit = (changed && !ImGui::IsItemActive()) || ImGui::IsItemDeactivatedAfterEdit();
            break;
        }
        case SettingType::Enum: {
            const auto index = static_cast<size_t>(value.integer);
            const std::string current = index < setting.options.size() ? plugin_text(source.manifest->folder, setting.options[index].label) : "?";
            if (ImGui::BeginCombo(snap.label.c_str(), current.c_str())) {
                for (size_t k = 0; k < setting.options.size(); ++k) {
                    if (ImGui::Selectable(plugin_text(source.manifest->folder, setting.options[k].label).c_str(), k == index)) {
                        value.integer = static_cast<int64_t>(k);
                        commit = true;
                    }
                }
                ImGui::EndCombo();
            }
            break;
        }
        case SettingType::String:
            ImGui::InputText(snap.label.c_str(), &text);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                value.text = text;
                commit = true;
            }
            break;
        case SettingType::Key:
            ImGui::InputTextWithHint(snap.label.c_str(), tr("no key"), &text);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                if (text.empty()) {
                    value.integer = 0;
                    commit = true;
                } else if (const auto code = parse_key(text)) {
                    value.integer = *code;
                    commit = true;
                }
            }
            if (!text.empty() && !parse_key(text)) {
                ImGui::SameLine();
                ImGui::TextColored(kBad, "%s", tr("not a key (e.g. Ctrl+Shift+F12)"));
            }
            break;
    }
    if (commit) save_plugin_settings(i);
}

void ManagerApp::draw_mod_approval(size_t i) {
    const cli::GameModPlugin& m = *plugins_->sources[i].mod;
    weak_wrapped(trf("from {0}", {mod_source(m)}));
    const bool good = m.state == "approved" && m.status == "loaded";
    const bool waits = m.state == "pending" || m.state == "update-pending" || m.state == "conflict";
    if (good) {
        state_row(anvil::ui::Glyph::On, mod_state_text(m));
        if (!m.others.empty()) weak_wrapped(trf("Other mods with this id: {0}", {join(m.others)}));
    }
    const Tone tone = waits ? Tone::Warn : Tone::Bad;
    if (!good) {
        begin_banner("mod-state", tone);
        wrapped(mod_state_text(m));
        weak_wrapped(m.why.code.empty() && m.why.text.empty() ? m.reason : i18n::text(m.why));
        if (!m.others.empty()) weak_wrapped(trf("Other mods with this id: {0}", {join(m.others)}));
    }
    for (const cli::Problem& problem : problems_shown(m.offered())) wrapped(problem.message);
    ImGui::BeginDisabled(read_only());
    if (can_approve_shown(m)) {
        if (mui::primary_button(m.state == "update-pending" ? tr("Approve the update...") : tr("Approve..."))) {
            approve_ = m;
            open_approve_ = true;
        }
        if (!m.approved_hash.empty()) ImGui::SameLine();
    }
    if (!m.approved_hash.empty() && ImGui::Button(tr("Revoke the approval"))) {
        std::string error;
        if (!revoke_shown_plugin(game_, m, error)) failed(tr("The approval was not revoked"), error, false);
        refresh();
    }
    ImGui::EndDisabled();
    if (!good) end_banner(tone);
}

void ManagerApp::save_plugin_settings(size_t i) {
    PluginSource& source = plugins_->sources[i];
    mui::PluginSnap& p = plugins_->model.plugins[i];
    for (const Setting& setting : source.schema->settings) {
        if (const std::string why = check_setting_value(setting, source.values[setting.id]); !why.empty()) {
            show_message(tr("Not saved"), p.settings[setting.id].label + ": " + why);
            return;
        }
    }
    std::string error;
    if (!write_player_settings(*source.schema, source.values, source.player_file, error)) {
        failed(tr("The settings were not saved"), error, error.find("error 5)") != std::string::npos);
        return;
    }
    source.settings_warnings.clear();
    for (const Setting& setting : source.schema->settings) p.settings[setting.id].saved = value_text(setting, source.values[setting.id]);
}

// Writes the model's edits: switches and opt-ins to anvil.toml, settings to
// the players' files. Switches change what loads, so the folder is read again.
void ManagerApp::apply_edits(const std::vector<mui::Edit>& edits) {
    if (edits.empty() || !plugins_ || read_only()) return;
    const fs::path toml = game_ / "anvil" / "anvil.toml";
    bool switches = false;
    std::vector<size_t> settings;
    std::string error;
    for (const mui::Edit& e : edits) {
        const size_t i = plugin_index(e.plugin);
        if (i == SIZE_MAX) continue;
        bool ok = true;
        if (e.kind == mui::Edit::Kind::Setting) {
            ok = apply_setting_edit(plugins_->sources[i], e, error);
            setting_texts_.erase(e.plugin + "/" + e.setting);  // a string or key field shows the new value
            if (ok && std::find(settings.begin(), settings.end(), i) == settings.end()) settings.push_back(i);
        } else if (e.which != mui::Which::On) {
            switches = true;
            ok = set_plugin_listed(toml, e.which == mui::Which::Ironman ? "allow_ironman" : "allow_multiplayer", e.key(), e.on, error);
        } else if (e.module.empty()) {
            switches = true;
            ok = set_plugin_listed(toml, "disabled_plugins", e.plugin, !e.on, error);
        } else {
            switches = true;
            const mui::ModuleSnap* mod = mui::find_module(plugins_->model.plugins[i], e.module);
            const bool by_default = mod == nullptr || mod->default_on;
            // On: out of disabled_modules, and into enabled_modules when it is off by default. Off: the reverse.
            if (e.on) {
                ok = set_plugin_listed(toml, "disabled_modules", e.key(), false, error) &&
                     (by_default || set_plugin_listed(toml, "enabled_modules", e.key(), true, error));
            } else {
                ok = by_default ? set_plugin_listed(toml, "disabled_modules", e.key(), true, error)
                                : set_plugin_listed(toml, "enabled_modules", e.key(), false, error);
            }
        }
        if (!ok) {
            failed(e.kind == mui::Edit::Kind::Setting ? tr("The settings were not saved") : tr("anvil.toml was not changed"), error,
                   error.find("error 5)") != std::string::npos);
            break;
        }
    }
    for (const size_t i : settings) save_plugin_settings(i);
    if (switches) refresh();
}

void ManagerApp::ask_remove(const std::string& folder) {
    mui::Confirm c;
    c.title = tr("Remove the plugin");
    c.button = tr("Remove");
    c.lines.push_back(trf("Remove the folder {0}? Its settings and data in anvil/data stay.", {"anvil/plugins/" + folder}));
    c.action = [this, folder] {
        std::string error;
        if (!remove_plugin_folder(game_, folder, package_->standard_plugins, error)) failed(tr("The plugin was not removed"), error, false);
        refresh();
    };
    plugins_tab_.ask(std::move(c));
}

}  // namespace anvil::manager
