// The Manager's Plugins tab (ADR 0013, docs/ui-guidelines.md §5): one list of
// the player's plugins, the plugins from mods and those that come with Anvil,
// and the detail of the one selected, with its modules and settings in one
// tree. The rules come from the shared view model (src/ui/plugin_model.hpp);
// this file draws them and writes anvil.toml and the players' settings files.
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

// The list entry's second line in the player's words, and its colour.
std::string entry_line(const mui::EntryLine& line) {
    using K = mui::EntryLine::Kind;
    switch (line.kind) {
        case K::Off: return tr("off");
        case K::NeedsPlugin: return trf("off: needs {0}", {line.name});
        case K::NotInstalled: return trf("off: needs {0}, which is not installed", {line.name});
        case K::CannotLoad: return tr("cannot load");
        case K::Failed: return tr("failed");
        case K::WaitsForApproval: return tr("waits for approval");
        case K::UpdateWaits: return trf("update {0} waits for approval", {line.name});
        case K::KeptOutOfIronman: return tr("kept out of ironman");
        case K::KeptOutOfMultiplayer: return tr("kept out of multiplayer");
        case K::Summary: break;
    }
    std::string text;
    const auto add = [&](const std::string& part) { text += (text.empty() ? "" : " · ") + part; };
    if (line.modules > 0) {
        add(trf("{0} of {1} modules on", {std::to_string(line.modules_on), std::to_string(line.modules)}));
    } else if (line.settings > 0) {
        add(trf("{0} setting(s)", {std::to_string(line.settings)}));
    }
    if (line.changed > 0) add(trf("{0} changed", {std::to_string(line.changed)}));
    if (text.empty() && line.changes_game) add(tr("changes the game"));
    if (text.empty()) add(tr("on"));  // the glyph's state in words
    return text;
}

ImVec4 entry_color(const mui::EntryLine& line) {
    using K = mui::EntryLine::Kind;
    switch (line.kind) {
        case K::CannotLoad:
        case K::Failed: return mui::kBad;
        case K::NeedsPlugin:
        case K::NotInstalled:
        case K::WaitsForApproval:
        case K::UpdateWaits:
        case K::KeptOutOfIronman:
        case K::KeptOutOfMultiplayer: return mui::kWarn;
        case K::Off: return mui::kTextDim;
        case K::Summary: break;
    }
    return mui::kTextWeak;
}

// "Where it runs" in a sentence, naming each kept-out mode once (ui-guidelines §5).
std::string where_sentence(const mui::Switches& s, const mui::Offers& offers) {
    if (!s.on) return tr("Off: it does not load.");
    std::string text = s.allow_ironman && s.allow_multiplayer ? tr("Runs in normal games, ironman and multiplayer.")
                       : s.allow_ironman                    ? tr("Runs in normal games and ironman.")
                       : s.allow_multiplayer                ? tr("Runs in normal games and multiplayer.")
                                                            : tr("Runs in normal games.");
    if (!s.allow_ironman) {
        text += " " + std::string(offers.ironman ? tr("Kept out of ironman unless you allow it.") : tr("Its author keeps it out of ironman."));
    }
    if (!s.allow_multiplayer) {
        text += " " + std::string(offers.multiplayer ? tr("Kept out of multiplayer unless you allow it.")
                                                     : tr("Its author keeps it out of multiplayer."));
    }
    return text;
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

std::string switch_word(bool on) { return on ? tr("on") : tr("off"); }
std::string allow_word(bool on) { return on ? tr("allowed") : tr("not allowed"); }

}  // namespace

void ManagerApp::rebuild_plugins() {
    std::optional<PluginsSnapshot> old = std::move(plugins_);
    plugins_.reset();
    if (!snapshot_ || !snapshot_->report || !package_) return;
    plugins_ = build_plugins_snapshot(game_, *snapshot_->report, snapshot_->anvil_toml, package_->standard_plugins,
                                      [this](const fs::path& folder, const std::string& english) {
                                          return plugin_text(folder, english);
                                      });
    setting_texts_.clear();
    // Until the Manager reads what the last start loaded, what the files held when it first read this
    // folder, or when the game last started from it, is what is in effect: a switch saved since then
    // waits for the next start as a setting does. The old snapshot is the same folder's (choose_game
    // drops it).
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
    if (ImGui::Button(tr("Open the plugins folder"))) open_folder(game_ / "anvil" / "plugins");
    ImGui::SameLine();
    ImGui::BeginDisabled(pending_.valid());
    if (ImGui::Button(tr("Check again"))) refresh();
    ImGui::EndDisabled();
    ImGui::SameLine();
    weak(tr("You can also drop a plugin zip on this window."));

    if (!report) {
        wrapped(tr("Anvil is not installed in this folder: no plugins load."));
        return;
    }
    if (!report->cannot_check.empty()) {
        colored_wrapped(kBad, trf("Cannot check the plugins: {0}", {report->cannot_check}));
        return;
    }
    if (!plugins_) rebuild_plugins();
    if (!plugins_) return;
    for (const cli::Problem& problem : report->problems) {
        colored_wrapped(kBad, u8(fs::path(widen(problem.file)).filename()) + ": " + problem.message);
    }
    if (!report->enabled) colored_wrapped(kWarn, tr("anvil.toml says enabled = false: no plugin loads."));
    if (snapshot_->installation && !snapshot_->installation->installed()) {
        colored_wrapped(kWarn, tr("version.dll is not Anvil's or missing: nothing below loads until Anvil is installed."));
    }
    if (running_) colored_wrapped(kWarn, tr("The game is running: change plugins in the game's overlay, or here after it closes."));

    const mui::Snapshot& model = plugins_->model;
    // What waits for the player shows from anywhere (ui-guidelines §5, "Plugins from mods").
    if (mui::to_approve(model) > 0) {
        ImGui::AlignTextToFramePadding();
        colored_wrapped(kWarn, tr("Waiting for your approval:"));
        for (const mui::PluginSnap& p : model.plugins) {
            if (!(p.blocked == mui::Blocked::WaitsForApproval && !p.approved) && p.update_waits.empty()) continue;
            ImGui::SameLine();
            const std::string label = p.update_waits.empty() ? p.name : trf("{0} (update to {1})", {p.name, p.update_waits});
            if (ImGui::SmallButton((label + "##wait-" + p.id).c_str())) selected_ = p.id;
        }
    }
    if (const size_t pending = mui::pending_count(model); pending > 0) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(mui::kInfo, "%s", trf("{0} change(s) wait for the next start.", {std::to_string(pending)}).c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(read_only());
        if (ImGui::SmallButton(tr("Undo them"))) apply_edits(mui::undo_pending(model));
        ImGui::EndDisabled();
    }
    ImGui::Spacing();

    if (selected_.empty() || plugin_index(selected_) == SIZE_MAX) {
        // The list's first entry: sections go Own, FromMod, WithAnvil, each in the model's order.
        const auto first = std::min_element(model.plugins.begin(), model.plugins.end(),
                                            [](const mui::PluginSnap& a, const mui::PluginSnap& b) { return a.origin < b.origin; });
        selected_ = first == model.plugins.end() ? std::string() : first->id;
    }
    const float list_width = std::clamp(ImGui::GetContentRegionAvail().x * 0.34f, ImGui::GetFontSize() * 16, ImGui::GetFontSize() * 24);
    ImGui::BeginChild("list", ImVec2(list_width, 0), ImGuiChildFlags_Borders);
    draw_plugin_list();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("detail", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (const size_t i = plugin_index(selected_); i != SIZE_MAX) draw_plugin_detail(i);
    ImGui::EndChild();
}

void ManagerApp::draw_plugin_list() {
    const mui::Snapshot& model = plugins_->model;
    const auto& report = *snapshot_->report;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Find"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_F) &&
        ImGui::GetIO().KeyCtrl) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputText("##find", &find_);
    if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape)) find_.clear();

    // The chips, each shown while it holds a plugin or is on; they wrap.
    struct ChipInfo {
        mui::Chip chip;
        const char* name;
    };
    const ChipInfo chips[] = {{mui::ChipAttention, i18n::N_("Needs attention")},
                              {mui::ChipChangesGame, i18n::N_("Changes the game")},
                              {mui::ChipChanged, i18n::N_("Changed settings")},
                              {mui::ChipPending, i18n::N_("Waits for the next start")},
                              {mui::ChipOff, i18n::N_("Off")}};
    float line_left = ImGui::GetContentRegionAvail().x;
    bool first = true;
    for (const ChipInfo& c : chips) {
        const size_t count = mui::chip_count(model, c.chip);
        const bool on = (chips_ & c.chip) != 0;
        if (count == 0 && !on) continue;
        const std::string label = trf("{0} ({1})", {tr(c.name), std::to_string(count)}) + "##chip";
        const float width = ImGui::CalcTextSize(label.c_str(), nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;
        if (!first && width + ImGui::GetStyle().ItemSpacing.x <= line_left) {
            ImGui::SameLine();
            line_left -= width + ImGui::GetStyle().ItemSpacing.x;
        } else {
            line_left = ImGui::GetContentRegionAvail().x - width;
        }
        first = false;
        ImGui::PushID(static_cast<int>(c.chip));
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, mui::with_alpha(mui::kEmber, 0.28f));
            ImGui::PushStyleColor(ImGuiCol_Border, mui::kEmber);
        }
        if (ImGui::SmallButton(label.c_str())) chips_ ^= c.chip;
        if (on) ImGui::PopStyleColor(2);
        ImGui::PopID();
    }

    const bool filtering = !find_.empty() || chips_ != 0;
    size_t shown = 0;
    for (const mui::PluginSnap& p : model.plugins) shown += mui::passes(model, p, find_, chips_) ? 1 : 0;
    if (filtering) {
        ImGui::AlignTextToFramePadding();
        weak(trf("{0} of {1} plugins", {std::to_string(shown), std::to_string(model.plugins.size())}));
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Clear"))) {
            find_.clear();
            chips_ = 0;
        }
    }
    ImGui::Separator();
    if (filtering && shown == 0) {
        wrapped(chips_ != 0 ? trf("No plugin matches \"{0}\" with these filters.", {find_}) : trf("No plugin matches \"{0}\".", {find_}));
        weak_wrapped(tr("The search looks in plugin names and ids, module names, setting labels and descriptions."));
        if (ImGui::Button(tr("Clear the search and filters"))) {
            find_.clear();
            chips_ = 0;
        }
        return;
    }

    struct Section {
        mui::Origin origin;
        const char* name;
        const char* id;
    };
    const Section sections[] = {{mui::Origin::Own, i18n::N_("Your plugins"), "own"},
                                {mui::Origin::FromMod, i18n::N_("From mods"), "mods"},
                                {mui::Origin::WithAnvil, i18n::N_("Comes with Anvil"), "anvil"}};
    const float line = ImGui::GetTextLineHeight();
    for (const Section& section : sections) {
        std::vector<size_t> members;
        for (size_t i = 0; i < model.plugins.size(); ++i) {
            if (model.plugins[i].origin == section.origin && mui::passes(model, model.plugins[i], find_, chips_)) members.push_back(i);
        }
        const mui::SectionCounts counts = mui::section_counts(model, section.origin);
        if (section.origin == mui::Origin::FromMod && counts.plugins == 0 && report.mod_warnings.empty() && report.playset_found) continue;
        if (filtering && members.empty()) continue;
        const std::string count = filtering ? trf("{0} found", {std::to_string(members.size())}) : std::to_string(counts.plugins);
        const bool holds_selection = std::any_of(members.begin(), members.end(), [&](size_t i) { return model.plugins[i].id == selected_; });
        ImGui::SetNextItemOpen(section.origin != mui::Origin::WithAnvil || holds_selection || filtering,
                               filtering || holds_selection ? ImGuiCond_Always : ImGuiCond_Once);
        const bool open = ImGui::TreeNodeEx((std::string(tr(section.name)) + " (" + count + ")###" + section.id).c_str(),
                                            ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                ImGuiTreeNodeFlags_AllowOverlap);
        for (const auto& [n, glyph] : {std::pair{counts.attention, mui::Glyph::Attention}, std::pair{counts.cannot_load, mui::Glyph::CannotLoad}}) {
            if (n == 0) continue;
            ImGui::SameLine();
            mui::glyph_item(glyph);
            ImGui::SameLine(0, 2);
            ImGui::TextColored(mui::glyph_color(glyph), "%zu", n);
        }
        if (!open) continue;
        if (section.origin == mui::Origin::FromMod) {
            std::string summary = trf("{0} plugin(s) in {1} enabled mod(s)", {std::to_string(counts.plugins), std::to_string(report.enabled_mods)});
            if (counts.waits_for_approval > 0) summary += ", " + trf("{0} waiting for approval", {std::to_string(counts.waits_for_approval)});
            if (counts.cannot_load > 0) summary += ", " + trf("{0} cannot load", {std::to_string(counts.cannot_load)});
            weak_wrapped(summary);
            if (!report.playset_found) colored_wrapped(kWarn, trf("No dlc_load.json in {0}: no mods are enabled.", {report.user_dir}));
            for (const std::string& w : report.mod_warnings) colored_wrapped(kWarn, w);
        }
        if (section.origin == mui::Origin::Own && members.empty()) weak(tr("No plugins are installed."));
        for (const size_t i : members) {
            const mui::PluginSnap& p = model.plugins[i];
            const mui::PluginView v = mui::view(model, p);
            const mui::Match match = mui::find(p, find_);
            const bool third = match.where == mui::Match::Where::Module || match.where == mui::Match::Where::Setting;
            const float pad = ImGui::GetStyle().FramePadding.y;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable(("##entry-" + p.id).c_str(), p.id == selected_, ImGuiSelectableFlags_None,
                                  ImVec2(0, line * (third ? 3 : 2) + pad * 2))) {
                selected_ = p.id;
            }
            if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("%s", (p.id + " " + p.version).c_str());
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const float x = at.x + line + ImGui::GetStyle().ItemInnerSpacing.x * 2;
            mui::draw_glyph(draw, ImVec2(at.x + 2, at.y + pad), line, v.glyph);
            const ImU32 name_color = ImGui::GetColorU32(v.load.loads ? mui::kText : mui::kTextWeak);
            draw->AddText(ImVec2(x, at.y + pad), name_color, p.name.c_str());
            if (v.pending > 0) {
                const float w = ImGui::CalcTextSize(p.name.c_str()).x;
                mui::draw_pending_dot(draw, ImVec2(x + w + line * 0.5f, at.y + pad + line * 0.5f), line * 0.18f);
            }
            draw->AddText(ImVec2(x, at.y + pad + line), ImGui::GetColorU32(entry_color(v.line)), entry_line(v.line).c_str());
            if (third) {
                const std::string where = match.where == mui::Match::Where::Module ? trf("found in module: {0}", {match.what})
                                                                                    : trf("found in setting: {0}", {match.what});
                draw->AddText(ImVec2(x, at.y + pad + line * 2), ImGui::GetColorU32(mui::kInfo), where.c_str());
            }
        }
        ImGui::Spacing();
    }
}

void ManagerApp::draw_plugin_detail(size_t i) {
    const mui::Snapshot& model = plugins_->model;
    const mui::PluginSnap& p = model.plugins[i];
    PluginSource& source = plugins_->sources[i];
    const mui::PluginView v = mui::view(model, p);
    const bool locked = read_only();
    ImGui::PushID(p.id.c_str());

    // The header: the name, the plugin's own switch, then Reset and Remove (ui-guidelines §5).
    mui::glyph_item(v.glyph);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(p.name.c_str());
    ImGui::SameLine();
    weak(p.version);
    ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 3);
    if (mui::plugin_toggle("##on", p.saved.on, p.saved.on ? tr("On") : tr("Off"), locked || !source.manifest)) {
        if (p.saved.on) {
            apply_edits(mui::set_switch(p, "", mui::Which::On, false));
        } else {
            const mui::Chain chain = mui::turn_on_chain(model, p.id);
            if (chain.steps.size() > 1 || !chain.missing.empty()) {
                ask_turn_on(chain);
            } else {
                apply_edits(mui::set_switch(p, "", mui::Which::On, true));
            }
        }
    }
    const std::vector<mui::Edit> reset = mui::reset_plugin(p);
    const bool removable = p.origin == mui::Origin::Own && !source.folder.empty();
    if (!reset.empty() || removable) {
        ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 3);
        weak("|");
    }
    ImGui::BeginDisabled(locked);
    if (!reset.empty()) {
        ImGui::SameLine();
        if (ImGui::Button(tr("Reset to defaults..."))) ask_edits(trf("Reset {0} to defaults", {p.name}), tr("Reset"), reset);
    }
    if (removable) {
        ImGui::SameLine();
        if (ImGui::Button(tr("Remove..."))) ask_remove(source.folder);
    }
    ImGui::EndDisabled();

    // What the player needs to know about it, in the order it matters.
    if (source.manifest) {
        const Manifest& m = *source.manifest;
        weak_wrapped(m.authors.empty() ? trf("id {0}", {m.id}) : trf("id {0}, by {1}", {m.id, join(m.authors)}));
    }
    if (source.mod != nullptr) draw_mod_approval(i);
    if (p.blocked == mui::Blocked::CannotLoad) {
        colored_wrapped(kBad, p.blocked_reason.empty() ? std::string(tr("It cannot load.")) : trf("It cannot load: {0}", {p.blocked_reason}));
    }
    if (v.load.why == mui::LoadState::Why::NeedsPlugin || v.load.why == mui::LoadState::Why::NotInstalled) {
        const mui::PluginSnap* missing = mui::find_plugin(model, v.load.missing);
        const std::string name = missing != nullptr ? missing->name : v.load.missing;
        std::string through;
        for (const std::string& id : v.load.through) {
            const mui::PluginSnap* t = mui::find_plugin(model, id);
            through += (through.empty() ? "" : ", ") + (t != nullptr ? t->name : id);
        }
        const bool off = missing != nullptr && !missing->saved.on;
        const std::string text = missing == nullptr ? (through.empty() ? trf("Needs {0}, which is not installed, so it does not load.", {name})
                                                                        : trf("Needs {0} (through {1}), which is not installed, so it does not load.", {name, through}))
                                 : off ? (through.empty() ? trf("Needs {0}, which is off, so it does not load.", {name})
                                                          : trf("Needs {0} (through {1}), which is off, so it does not load.", {name, through}))
                                       : (through.empty() ? trf("Needs {0}, which does not load, so it does not load either.", {name})
                                                          : trf("Needs {0} (through {1}), which does not load, so it does not load either.", {name, through}));
        colored_wrapped(kWarn, text);
        const mui::Chain chain = mui::turn_on_chain(model, p.id);
        if (!chain.steps.empty()) {
            std::string names;
            for (const mui::Chain::Step& step : chain.steps) {
                if (step.id == p.id) continue;
                const mui::PluginSnap* s = mui::find_plugin(model, step.id);
                names += (names.empty() ? "" : ", ") + (s != nullptr ? s->name : step.id);
            }
            if (!names.empty()) {
                ImGui::BeginDisabled(locked);
                if (ImGui::Button(trf("Turn on {0}...", {names}).c_str())) ask_turn_on(chain);
                ImGui::EndDisabled();
            }
        }
    }
    if (!v.needed_by.empty() && p.saved.on) {
        std::string names;
        for (const std::string& id : v.needed_by) {
            const mui::PluginSnap* n = mui::find_plugin(model, id);
            names += (names.empty() ? "" : ", ") + (n != nullptr ? n->name : id);
        }
        weak_wrapped(trf("Needed by {0}. Turning it off stops it too; it keeps its own switch.", {names}));
    }
    if (!p.description.empty()) wrapped(p.description);
    if (source.folder.size() > 0) {
        for (const cli::GamePlugin& gp : snapshot_->report->plugins) {
            if (gp.folder != source.folder) continue;
            for (const cli::Problem& problem : problems_shown(gp.report)) {
                // The reason it cannot load already says it.
                if (p.blocked == mui::Blocked::CannotLoad && p.blocked_reason.find(problem.message) != std::string::npos) continue;
                colored_wrapped(kWarn, problem.message);
            }
        }
    }
    for (const cli::OverlapWarning& warning : snapshot_->report->warnings) {
        if (warning.plugin == p.id || warning.other == p.id) colored_wrapped(kWarn, warning.message);
    }
    const std::vector<const CatalogPlugin*> updates = catalog_updates_here();
    if (const auto update = std::find_if(updates.begin(), updates.end(), [&](const CatalogPlugin* c) { return c->id == p.id; });
        update != updates.end()) {
        colored_wrapped(kGood, trf("{0} is available in the Catalog tab.", {(*update)->version}));
        if (ImGui::SmallButton(tr("Open the Catalog tab"))) show_catalog_tab_ = true;
    }
    if (!source.settings_error.empty()) colored_wrapped(kBad, source.settings_error);
    for (const std::string& warning : source.settings_warnings) colored_wrapped(kWarn, trf("Ignored in your file: {0}", {warning}));
    ImGui::Separator();

    // The body scrolls; the header above stays in sight.
    ImGui::BeginChild("body", ImVec2(0, 0), ImGuiChildFlags_None);
    const bool plugin_off = !p.saved.on || !source.manifest;
    const bool any_game = p.changes_game;
    if (p.modules.empty() && p.changes_game && source.manifest) {
        ImGui::SeparatorText(tr("Where it runs"));
        draw_where(i, "", p.saved, p.offers, true, locked || plugin_off);
    }
    if (any_game) {
        weak_wrapped(tr("Plugins and modules that change the game are kept out of ironman and multiplayer games unless you allow "
                        "them; in multiplayer the players compare the overlay's code of the ones that act."));
    }
    if (plugin_off && !p.modules.empty()) {
        weak_wrapped(tr("The plugin is off, so none of its modules load. Each module keeps its switch and its settings."));
    }
    if (!p.modules.empty()) {
        ImGui::SeparatorText(trf("Modules ({0})", {std::to_string(p.modules.size())}).c_str());
        for (const mui::ModuleGroup& group : mui::module_groups(p)) {
            bool open = true;
            if (!group.name.empty()) {
                ImGui::SetNextItemOpen(p.modules.size() <= mui::kFoldGroupsAbove, ImGuiCond_Once);
                open = ImGui::TreeNodeEx((group.name + "  " + trf("{0} of {1} on", {std::to_string(group.on), std::to_string(group.modules.size())}) +
                                          "###group-" + group.name)
                                             .c_str(),
                                         ImGuiTreeNodeFlags_SpanAvailWidth);
            }
            if (!open) continue;
            for (const size_t m : group.modules) draw_module(i, m, locked || plugin_off);
            if (!group.name.empty()) ImGui::TreePop();
        }
    }
    const mui::SettingsLayout own = mui::settings_layout(p, "");
    if (!own.shown.empty() || !own.advanced.empty()) {
        ImGui::SeparatorText(tr("Settings"));
        draw_settings_of(i, "", locked);
    }
    ImGui::EndChild();
    ImGui::PopID();
}

void ManagerApp::draw_where(size_t i, std::string_view module, const mui::Switches& saved, const mui::Offers& offers,
                            bool without_off, bool disabled) {
    const mui::PluginSnap& p = plugins_->model.plugins[i];
    const mui::Where choices[] = {mui::Where::Off, mui::Where::Normal, mui::Where::PlusIronman, mui::Where::PlusMultiplayer,
                                  mui::Where::Everywhere};
    const char* labels[] = {tr("Off"), tr("Normal games"), tr("+ Ironman"), tr("+ Multiplayer"), tr("Everywhere")};
    bool enabled[5];
    const char* tips[5] = {};
    const std::string no_ironman = tr("Its author keeps it out of ironman.");
    const std::string no_multiplayer = tr("Its author keeps it out of multiplayer.");
    for (int k = 0; k < 5; ++k) {
        enabled[k] = mui::where_offered(offers, choices[k]);
        if (!enabled[k]) tips[k] = !offers.ironman && (k == 2 || k == 4) ? no_ironman.c_str() : no_multiplayer.c_str();
    }
    const int skip = without_off ? 1 : 0;
    // Without Off (a plugin without modules: its toggle turns it off), the choice stays shown, greyed, while it is off.
    mui::Switches shown = saved;
    if (without_off) shown.on = true;
    const int current = static_cast<int>(mui::where(shown)) - skip;
    const int chosen = mui::segmented("where", labels + skip, enabled + skip, tips + skip, 5 - skip, current, disabled);
    if (chosen >= 0) apply_edits(mui::set_where(p, module, choices[chosen + skip]));
    weak_wrapped(where_sentence(saved.on || without_off ? shown : saved, offers));
}

void ManagerApp::draw_module(size_t i, size_t m, bool disabled) {
    const mui::PluginSnap& p = plugins_->model.plugins[i];
    const mui::ModuleSnap& mod = p.modules[m];
    ImGui::PushID(mod.id.c_str());
    // Its name, what it does, then its switch: a checkbox, or where it runs when it changes the game.
    const auto describe = [&] {
        if (mod.description.empty()) return;
        ImGui::Indent();
        weak_wrapped(mod.description);
        ImGui::Unindent();
    };
    if (mod.changes_game) {
        ImGui::BeginDisabled(disabled);
        ImGui::TextUnformatted(mod.name.c_str());
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetItemTooltip("%s", (p.id + "." + mod.id).c_str());
        describe();
        draw_where(i, mod.id, mod.saved, mod.offers, false, disabled);
    } else {
        bool on = mod.saved.on;
        ImGui::BeginDisabled(disabled);
        if (ImGui::Checkbox(mod.name.c_str(), &on)) apply_edits(mui::set_switch(p, mod.id, mui::Which::On, on));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetItemTooltip("%s", (p.id + "." + mod.id).c_str());
        describe();
    }
    const mui::SettingsLayout layout = mui::settings_layout(p, mod.id);
    const size_t n = layout.shown.size() + layout.advanced.size();
    if (n > 0) {
        ImGui::Indent();
        if (mod.saved.on) {
            draw_settings_of(i, mod.id, disabled);
        } else if (ImGui::TreeNode(trf("Settings ({0}), kept while it is off", {std::to_string(n)}).c_str())) {
            draw_settings_of(i, mod.id, true);
            ImGui::TreePop();
        }
        const std::vector<mui::Edit> reset = mui::reset_module_settings(p, mod.id);
        if (reset.size() > 1) {
            ImGui::BeginDisabled(disabled);
            if (ImGui::SmallButton(trf("Reset settings ({0})...", {std::to_string(reset.size())}).c_str())) {
                ask_edits(trf("Reset the settings of {0}", {mod.name}), tr("Reset"), reset);
            }
            ImGui::EndDisabled();
        }
        ImGui::Unindent();
    }
    ImGui::Spacing();
    ImGui::PopID();
}

void ManagerApp::draw_settings_of(size_t i, std::string_view module, bool disabled) {
    const mui::SettingsLayout layout = mui::settings_layout(plugins_->model.plugins[i], module);
    const auto draw_list = [&](const std::vector<size_t>& indices) {
        std::string group;
        for (const size_t s : indices) {
            const std::string& g = plugins_->model.plugins[i].settings[s].group;
            if (g != group) {
                group = g;
                // A subheading, quieter than the section's separator above it.
                if (!group.empty()) {
                    ImGui::Spacing();
                    weak(group);
                }
            }
            draw_setting(i, s, disabled);
        }
    };
    draw_list(layout.shown);
    if (!layout.advanced.empty() && ImGui::TreeNode(trf("Advanced ({0})", {std::to_string(layout.advanced.size())}).c_str())) {
        draw_list(layout.advanced);
        ImGui::TreePop();
    }
}

void ManagerApp::draw_setting(size_t i, size_t s, bool disabled) {
    PluginSource& source = plugins_->sources[i];
    if (!source.schema) return;
    const Setting& setting = source.schema->settings[s];
    SettingValue& value = source.values[s];
    const mui::SettingSnap& snap = plugins_->model.plugins[i].settings[s];
    std::string& text = setting_texts_[plugins_->model.plugins[i].id + "/" + setting.name];
    if (text.empty()) text = setting.type == SettingType::Key ? (value.integer == 0 ? std::string() : key_name(value.integer)) : value.text;
    ImGui::PushID(static_cast<int>(s));
    ImGui::BeginDisabled(disabled);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
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
    ImGui::EndDisabled();
    // "(?)": its description and its default; a changed value says so in words, with Reset.
    ImGui::SameLine();
    weak("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
        if (!snap.description.empty()) ImGui::TextUnformatted(snap.description.c_str());
        ImGui::TextUnformatted(trf("Default: {0}", {shown_value(setting, setting.default_value)}).c_str());
        weak(setting.name);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    if (!(value == setting.default_value)) {
        ImGui::SameLine();
        ImGui::TextColored(mui::kInfo, "%s", trf("changed · default: {0}", {shown_value(setting, setting.default_value)}).c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(disabled);
        if (ImGui::SmallButton(tr("Reset"))) {
            value = setting.default_value;
            text.clear();
            commit = true;
        }
        ImGui::EndDisabled();
    }
    if (commit) save_plugin_settings(i);
    ImGui::PopID();
}

void ManagerApp::draw_mod_approval(size_t i) {
    const cli::GameModPlugin& m = *plugins_->sources[i].mod;
    weak_wrapped(trf("from {0}", {mod_source(m)}));
    const bool good = m.state == "approved" && m.status == "loaded";
    const bool waits = m.state == "pending" || m.state == "update-pending" || m.state == "conflict";
    colored_wrapped(good ? kGood : waits ? kWarn : kBad, mod_state_text(m));
    if (!good) weak_wrapped(m.why.code.empty() && m.why.text.empty() ? m.reason : i18n::text(m.why));
    if (!m.others.empty()) weak_wrapped(trf("Other mods with this id: {0}", {join(m.others)}));
    for (const cli::Problem& problem : problems_shown(m.offered())) colored_wrapped(kWarn, problem.message);
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

void ManagerApp::ask_edits(std::string title, std::string button, std::vector<mui::Edit> edits) {
    Confirm c;
    c.title = std::move(title);
    c.button = std::move(button);
    for (const mui::Edit& e : edits) {
        const size_t i = plugin_index(e.plugin);
        if (i == SIZE_MAX) continue;
        const mui::PluginSnap& p = plugins_->model.plugins[i];
        if (e.kind == mui::Edit::Kind::Setting) {
            const PluginSource& source = plugins_->sources[i];
            for (const Setting& setting : source.schema->settings) {
                if (setting.name != e.setting) continue;
                c.lines.push_back(trf("{0}: {2} (now {1})", {p.settings[setting.id].label, shown_value(setting, source.values[setting.id]),
                                                         shown_value(setting, setting.default_value)}));
            }
            continue;
        }
        const mui::ModuleSnap* mod = e.module.empty() ? nullptr : mui::find_module(p, e.module);
        const std::string what = mod != nullptr ? mod->name : p.name;
        if (e.which == mui::Which::On) {
            c.lines.push_back(trf("{0}: {2} (now {1})", {what, switch_word(!e.on), switch_word(e.on)}));
        } else {
            c.lines.push_back(trf(e.which == mui::Which::Ironman ? i18n::N_("{0} in ironman: {2} (now {1})") : i18n::N_("{0} in multiplayer: {2} (now {1})"),
                                  {what, allow_word(!e.on), allow_word(e.on)}));
        }
    }
    c.edits = std::move(edits);
    confirm_ = std::move(c);
    open_confirm_ = true;
}

void ManagerApp::ask_turn_on(const mui::Chain& chain) {
    const mui::Snapshot& model = plugins_->model;
    Confirm c;
    std::vector<mui::Edit> edits = mui::edits_of(model, chain);
    c.title = trf("Turn on {0} plugin(s)", {std::to_string(edits.size())});
    c.button = c.title;
    const auto name = [&](const std::string& id) {
        const mui::PluginSnap* p = mui::find_plugin(model, id);
        return p != nullptr ? p->name : id;
    };
    for (const mui::Chain::Step& step : chain.steps) {
        c.lines.push_back(step.needed_by.empty() ? name(step.id) : trf("{0}, needed by {1}", {name(step.id), name(step.needed_by)}));
    }
    for (const std::string& id : chain.missing) c.lines.push_back(trf("{0} is not installed: install it first.", {id}));
    c.edits = std::move(edits);
    confirm_ = std::move(c);
    open_confirm_ = true;
}

void ManagerApp::ask_remove(const std::string& folder) {
    Confirm c;
    c.title = tr("Remove the plugin");
    c.button = tr("Remove");
    c.lines.push_back(trf("Remove the folder {0}? Its settings and data in anvil/data stay.", {"anvil/plugins/" + folder}));
    c.remove_folder = folder;
    confirm_ = std::move(c);
    open_confirm_ = true;
}

void ManagerApp::draw_confirm_modal() {
    if (open_confirm_) {
        ImGui::OpenPopup("###confirm");
        open_confirm_ = false;
    }
    next_modal(34);
    if (!confirm_) return;
    if (!ImGui::BeginPopupModal((confirm_->title + "###confirm").c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    for (const std::string& line : confirm_->lines) bullet(line);
    ImGui::Spacing();
    ImGui::BeginDisabled(read_only() || (confirm_->edits.empty() && confirm_->remove_folder.empty()));
    if (mui::primary_button(confirm_->button.c_str())) {
        if (!confirm_->remove_folder.empty()) {
            std::string error;
            if (!remove_plugin_folder(game_, confirm_->remove_folder, package_->standard_plugins, error)) {
                failed(tr("The plugin was not removed"), error, false);
            }
            refresh();
        } else {
            apply_edits(confirm_->edits);
        }
        confirm_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"))) {
        confirm_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}  // namespace anvil::manager
