// The Plugins tab both apps draw (plugins_view.hpp). Moved from the Manager's
// plugins_tab.cpp when the overlay got the same tab; the Manager's pictures did
// not change.
#include "ui/plugins_view.hpp"

#include "i18n/translate.hpp"
#include "ui/plugin_widgets.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <string_view>

namespace anvil::ui {

namespace {

using i18n::tr;
using i18n::trf;

void wrapped(const std::string& text) { ImGui::TextWrapped("%s", text.c_str()); }

void colored_wrapped(const ImVec4& color, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    wrapped(text);
    ImGui::PopStyleColor();
}

void weak(const std::string& text) { ImGui::TextDisabled("%s", text.c_str()); }

void weak_wrapped(const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    wrapped(text);
    ImGui::PopStyleColor();
}

void bullet(const std::string& text) {
    ImGui::Bullet();
    ImGui::SameLine();
    wrapped(text);
}

std::string name_of(const Snapshot& model, const std::string& id) {
    const PluginSnap* p = find_plugin(model, id);
    return p != nullptr ? p->name : id;
}

std::string names_of(const Snapshot& model, const std::vector<std::string>& ids) {
    std::string out;
    for (const std::string& id : ids) out += (out.empty() ? "" : ", ") + name_of(model, id);
    return out;
}

size_t index_of(const Snapshot& model, std::string_view id) {
    for (size_t i = 0; i < model.plugins.size(); ++i) {
        if (model.plugins[i].id == id) return i;
    }
    return SIZE_MAX;
}

size_t setting_index(const PluginSnap& p, std::string_view name) {
    for (size_t s = 0; s < p.settings.size(); ++s) {
        if (p.settings[s].name == name) return s;
    }
    return SIZE_MAX;
}

// A module in this game, when it is not simply running (the overlay's second state).
std::string in_game_text(InGame state) {
    switch (state) {
        case InGame::Off: return tr("off in this game");
        case InGame::OffWithPlugin: return tr("off with its plugin in this game");
        case InGame::KeptOutOfIronman: return tr("kept out of this game: not allowed in ironman");
        case InGame::KeptOutOfMultiplayer: return tr("kept out of this game: not allowed in multiplayer");
        case InGame::Runs: break;
    }
    return {};
}

}  // namespace

std::string entry_line(const EntryLine& line) {
    using K = EntryLine::Kind;
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

ImVec4 entry_color(const EntryLine& line) {
    using K = EntryLine::Kind;
    switch (line.kind) {
        case K::CannotLoad:
        case K::Failed: return kBad;
        case K::NeedsPlugin:
        case K::NotInstalled:
        case K::WaitsForApproval:
        case K::UpdateWaits:
        case K::KeptOutOfIronman:
        case K::KeptOutOfMultiplayer: return kWarn;
        case K::Off: return kTextDim;
        case K::Summary: break;
    }
    return kTextWeak;
}

std::string where_sentence(const Switches& s, const Offers& offers) {
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

std::string switch_word(bool on) { return on ? tr("on") : tr("off"); }
std::string allow_word(bool on) { return on ? tr("allowed") : tr("not allowed"); }

// ---- the lines above the list ---------------------------------------------------------

void PluginsTab::draw_waiting(const Snapshot& model) {
    if (to_approve(model) == 0) return;
    ImGui::AlignTextToFramePadding();
    colored_wrapped(kWarn, tr("Waiting for your approval:"));
    for (const PluginSnap& p : model.plugins) {
        if (!(p.blocked == Blocked::WaitsForApproval && !p.approved) && p.update_waits.empty()) continue;
        ImGui::SameLine();
        const std::string label = p.update_waits.empty() ? p.name : trf("{0} (update to {1})", {p.name, p.update_waits});
        if (ImGui::SmallButton((label + "##wait-" + p.id).c_str())) selected = p.id;
    }
}

void PluginsTab::draw_pending(const Snapshot& model, PluginsHost& host) {
    const size_t pending = pending_count(model);
    if (pending == 0) return;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kInfo, "%s", trf("{0} change(s) wait for the next start.", {std::to_string(pending)}).c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(host.read_only());
    if (ImGui::SmallButton(tr("Undo them"))) host.apply(undo_pending(model));
    ImGui::EndDisabled();
}

void PluginsTab::draw_panes(const Snapshot& model, PluginsHost& host) {
    if (selected.empty() || index_of(model, selected) == SIZE_MAX) {
        // The list's first entry: sections go Own, FromMod, WithAnvil, each in the model's order.
        const auto first = std::min_element(model.plugins.begin(), model.plugins.end(),
                                            [](const PluginSnap& a, const PluginSnap& b) { return a.origin < b.origin; });
        selected = first == model.plugins.end() ? std::string() : first->id;
    }
    const float list_width = std::clamp(ImGui::GetContentRegionAvail().x * 0.34f, ImGui::GetFontSize() * 16, ImGui::GetFontSize() * 24);
    ImGui::BeginChild("list", ImVec2(list_width, 0), ImGuiChildFlags_Borders);
    draw_list(model, host);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("detail", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (const size_t i = index_of(model, selected); i != SIZE_MAX) draw_detail(model, i, host);
    ImGui::EndChild();
}

// ---- the list -----------------------------------------------------------------------

void PluginsTab::draw_list(const Snapshot& model, PluginsHost& host) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Find"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_F) && ImGui::GetIO().KeyCtrl) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputText("##find", &find);
    if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape)) find.clear();

    // The chips, each shown while it holds a plugin or is on; they wrap.
    struct ChipInfo {
        Chip chip;
        const char* name;
    };
    const ChipInfo all_chips[] = {{ChipAttention, i18n::N_("Needs attention")},
                                  {ChipChangesGame, i18n::N_("Changes the game")},
                                  {ChipChanged, i18n::N_("Changed settings")},
                                  {ChipPending, i18n::N_("Waits for the next start")},
                                  {ChipOff, i18n::N_("Off")}};
    float line_left = ImGui::GetContentRegionAvail().x;
    bool first = true;
    for (const ChipInfo& c : all_chips) {
        const size_t count = chip_count(model, c.chip);
        const bool on = (chips & c.chip) != 0;
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
            ImGui::PushStyleColor(ImGuiCol_Button, with_alpha(kEmber, 0.28f));
            ImGui::PushStyleColor(ImGuiCol_Border, kEmber);
        }
        if (ImGui::SmallButton(label.c_str())) chips ^= c.chip;
        if (on) ImGui::PopStyleColor(2);
        ImGui::PopID();
    }

    const bool filtering = !find.empty() || chips != 0;
    size_t shown = 0;
    for (const PluginSnap& p : model.plugins) shown += passes(model, p, find, chips) ? 1 : 0;
    if (filtering) {
        ImGui::AlignTextToFramePadding();
        weak(trf("{0} of {1} plugins", {std::to_string(shown), std::to_string(model.plugins.size())}));
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Clear"))) {
            find.clear();
            chips = 0;
        }
    }
    ImGui::Separator();
    if (filtering && shown == 0) {
        wrapped(chips != 0 ? trf("No plugin matches \"{0}\" with these filters.", {find}) : trf("No plugin matches \"{0}\".", {find}));
        weak_wrapped(tr("The search looks in plugin names and ids, module names, setting labels and descriptions."));
        if (ImGui::Button(tr("Clear the search and filters"))) {
            find.clear();
            chips = 0;
        }
        return;
    }

    struct Section {
        Origin origin;
        const char* name;
        const char* id;
    };
    const Section sections[] = {{Origin::Own, i18n::N_("Your plugins"), "own"},
                                {Origin::FromMod, i18n::N_("From mods"), "mods"},
                                {Origin::WithAnvil, i18n::N_("Comes with Anvil"), "anvil"}};
    const float line = ImGui::GetTextLineHeight();
    for (const Section& section : sections) {
        std::vector<size_t> members;
        for (size_t i = 0; i < model.plugins.size(); ++i) {
            if (model.plugins[i].origin == section.origin && passes(model, model.plugins[i], find, chips)) members.push_back(i);
        }
        const SectionCounts counts = section_counts(model, section.origin);
        if (section.origin != Origin::Own && counts.plugins == 0 && !host.show_empty_section(section.origin)) continue;
        if (filtering && members.empty()) continue;
        const std::string count = filtering ? trf("{0} found", {std::to_string(members.size())}) : std::to_string(counts.plugins);
        const bool holds_selection = std::any_of(members.begin(), members.end(), [&](size_t i) { return model.plugins[i].id == selected; });
        ImGui::SetNextItemOpen(section.origin != Origin::WithAnvil || holds_selection || filtering,
                               filtering || holds_selection ? ImGuiCond_Always : ImGuiCond_Once);
        const bool open = ImGui::TreeNodeEx((std::string(tr(section.name)) + " (" + count + ")###" + section.id).c_str(),
                                            ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                ImGuiTreeNodeFlags_AllowOverlap);
        for (const auto& [n, glyph] : {std::pair{counts.attention, Glyph::Attention}, std::pair{counts.cannot_load, Glyph::CannotLoad}}) {
            if (n == 0) continue;
            ImGui::SameLine();
            glyph_item(glyph);
            ImGui::SameLine(0, 2);
            ImGui::TextColored(glyph_color(glyph), "%zu", n);
        }
        if (!open) continue;
        host.draw_section_note(section.origin, counts);
        if (section.origin == Origin::Own && members.empty()) weak(tr("No plugins are installed."));
        for (const size_t i : members) {
            const PluginSnap& p = model.plugins[i];
            const PluginView v = view(model, p);
            const Match match = anvil::ui::find(p, find);
            const bool third = match.where == Match::Where::Module || match.where == Match::Where::Setting;
            const float pad = ImGui::GetStyle().FramePadding.y;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable(("##entry-" + p.id).c_str(), p.id == selected, ImGuiSelectableFlags_None,
                                  ImVec2(0, line * (third ? 3 : 2) + pad * 2))) {
                selected = p.id;
            }
            if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("%s", (p.id + " " + p.version).c_str());
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const float x = at.x + line + ImGui::GetStyle().ItemInnerSpacing.x * 2;
            draw_glyph(draw, ImVec2(at.x + 2, at.y + pad), line, v.glyph);
            const ImU32 name_color = ImGui::GetColorU32(v.load.loads ? kText : kTextWeak);
            draw->AddText(ImVec2(x, at.y + pad), name_color, p.name.c_str());
            if (v.pending > 0) {
                const float w = ImGui::CalcTextSize(p.name.c_str()).x;
                draw_pending_dot(draw, ImVec2(x + w + line * 0.5f, at.y + pad + line * 0.5f), line * 0.18f);
            }
            draw->AddText(ImVec2(x, at.y + pad + line), ImGui::GetColorU32(entry_color(v.line)), entry_line(v.line).c_str());
            if (third) {
                const std::string where = match.where == Match::Where::Module ? trf("found in module: {0}", {match.what})
                                                                              : trf("found in setting: {0}", {match.what});
                draw->AddText(ImVec2(x, at.y + pad + line * 2), ImGui::GetColorU32(kInfo), where.c_str());
            }
        }
        ImGui::Spacing();
    }
}

// ---- the detail ---------------------------------------------------------------------

void PluginsTab::draw_detail(const Snapshot& model, size_t i, PluginsHost& host) {
    const PluginSnap& p = model.plugins[i];
    const PluginView v = view(model, p);
    const bool locked = host.read_only();
    const bool known = host.known(i);
    ImGui::PushID(p.id.c_str());

    // The header: the name, the plugin's own switch, then Reset and the app's buttons (ui-guidelines §5).
    glyph_item(v.glyph);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(p.name.c_str());
    ImGui::SameLine();
    weak(p.version);
    ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 3);
    const std::string refusal = host.switch_refusal(i);
    if (plugin_toggle("##on", p.saved.on, p.saved.on ? tr("On") : tr("Off"), locked || !known || !refusal.empty())) {
        if (p.saved.on) {
            host.apply(set_switch(p, "", Which::On, false));
        } else {
            const Chain chain = turn_on_chain(model, p.id);
            if (chain.steps.size() > 1 || !chain.missing.empty()) {
                ask_turn_on(model, chain);
            } else {
                host.apply(set_switch(p, "", Which::On, true));
            }
        }
    }
    if (!refusal.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", refusal.c_str());
    const std::vector<Edit> reset = reset_plugin(p);
    const bool buttons = host.has_header_buttons(i);
    if (!reset.empty() || buttons) {
        ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 3);
        weak("|");
    }
    ImGui::BeginDisabled(locked);
    if (!reset.empty()) {
        ImGui::SameLine();
        if (ImGui::Button(tr("Reset to defaults..."))) ask_edits(model, host, trf("Reset {0} to defaults", {p.name}), tr("Reset"), reset);
    }
    if (buttons) host.draw_header_buttons(i);
    ImGui::EndDisabled();
    if (!refusal.empty()) weak_wrapped(refusal);

    // What the player needs to know about it, in the order it matters.
    host.draw_facts(i);
    if (p.blocked == Blocked::CannotLoad) {
        colored_wrapped(kBad, p.blocked_reason.empty() ? std::string(tr("It cannot load.")) : trf("It cannot load: {0}", {p.blocked_reason}));
    }
    if (v.load.why == LoadState::Why::NeedsPlugin || v.load.why == LoadState::Why::NotInstalled) {
        const PluginSnap* missing = find_plugin(model, v.load.missing);
        const std::string name = missing != nullptr ? missing->name : v.load.missing;
        const std::string through = names_of(model, v.load.through);
        const bool off = missing != nullptr && !missing->saved.on;
        const std::string text = missing == nullptr ? (through.empty() ? trf("Needs {0}, which is not installed, so it does not load.", {name})
                                                                        : trf("Needs {0} (through {1}), which is not installed, so it does not load.", {name, through}))
                                 : off ? (through.empty() ? trf("Needs {0}, which is off, so it does not load.", {name})
                                                          : trf("Needs {0} (through {1}), which is off, so it does not load.", {name, through}))
                                       : (through.empty() ? trf("Needs {0}, which does not load, so it does not load either.", {name})
                                                          : trf("Needs {0} (through {1}), which does not load, so it does not load either.", {name, through}));
        colored_wrapped(kWarn, text);
        const Chain chain = turn_on_chain(model, p.id);
        if (!chain.steps.empty()) {
            std::vector<std::string> others;
            for (const Chain::Step& step : chain.steps) {
                if (step.id != p.id) others.push_back(step.id);
            }
            if (!others.empty()) {
                ImGui::BeginDisabled(locked);
                if (ImGui::Button(trf("Turn on {0}...", {names_of(model, others)}).c_str())) ask_turn_on(model, chain);
                ImGui::EndDisabled();
            }
        }
    }
    if (!v.needed_by.empty() && p.saved.on) {
        weak_wrapped(trf("Needed by {0}. Turning it off stops it too; it keeps its own switch.", {names_of(model, v.needed_by)}));
    }
    if (!p.description.empty()) wrapped(p.description);
    host.draw_notes(i);
    ImGui::Separator();

    // The body scrolls; the header above stays in sight.
    ImGui::BeginChild("body", ImVec2(0, 0), ImGuiChildFlags_None);
    const bool plugin_off = !p.saved.on || !known;
    if (p.modules.empty() && p.changes_game && known) {
        ImGui::SeparatorText(tr("Where it runs"));
        draw_where(model, i, "", p.saved, p.offers, true, locked || plugin_off, host);
    }
    if (p.changes_game) {
        weak_wrapped(tr("Plugins and modules that change the game are kept out of ironman and multiplayer games unless you allow "
                        "them; in multiplayer the players compare the overlay's code of the ones that act."));
    }
    if (plugin_off && !p.modules.empty()) {
        weak_wrapped(tr("The plugin is off, so none of its modules load. Each module keeps its switch and its settings."));
    }
    if (!p.modules.empty()) {
        ImGui::SeparatorText(trf("Modules ({0})", {std::to_string(p.modules.size())}).c_str());
        for (const ModuleGroup& group : module_groups(p)) {
            bool open = true;
            if (!group.name.empty()) {
                ImGui::SetNextItemOpen(p.modules.size() <= kFoldGroupsAbove, ImGuiCond_Once);
                open = ImGui::TreeNodeEx((group.name + "  " + trf("{0} of {1} on", {std::to_string(group.on), std::to_string(group.modules.size())}) +
                                          "###group-" + group.name)
                                             .c_str(),
                                         ImGuiTreeNodeFlags_SpanAvailWidth);
            }
            if (!open) continue;
            for (const size_t m : group.modules) draw_module(model, i, m, locked || plugin_off, host);
            if (!group.name.empty()) ImGui::TreePop();
        }
    }
    const SettingsLayout own = settings_layout(p, "");
    if (!own.shown.empty() || !own.advanced.empty()) {
        ImGui::SeparatorText(tr("Settings"));
        draw_settings_of(model, i, "", locked, host);
    }
    ImGui::EndChild();
    ImGui::PopID();
}

void PluginsTab::draw_where(const Snapshot& model, size_t i, std::string_view module, const Switches& saved, const Offers& offers,
                            bool without_off, bool disabled, PluginsHost& host) {
    const PluginSnap& p = model.plugins[i];
    const Where choices[] = {Where::Off, Where::Normal, Where::PlusIronman, Where::PlusMultiplayer, Where::Everywhere};
    const char* labels[] = {tr("Off"), tr("Normal games"), tr("+ Ironman"), tr("+ Multiplayer"), tr("Everywhere")};
    bool enabled[5];
    const char* tips[5] = {};
    const std::string no_ironman = tr("Its author keeps it out of ironman.");
    const std::string no_multiplayer = tr("Its author keeps it out of multiplayer.");
    for (int k = 0; k < 5; ++k) {
        enabled[k] = where_offered(offers, choices[k]);
        if (!enabled[k]) tips[k] = !offers.ironman && (k == 2 || k == 4) ? no_ironman.c_str() : no_multiplayer.c_str();
    }
    const int skip = without_off ? 1 : 0;
    // Without Off (a plugin without modules: its toggle turns it off), the choice stays shown, greyed, while it is off.
    Switches shown = saved;
    if (without_off) shown.on = true;
    const int current = static_cast<int>(where(shown)) - skip;
    const int chosen = segmented("where", labels + skip, enabled + skip, tips + skip, 5 - skip, current, disabled);
    if (chosen >= 0) host.apply(set_where(p, module, choices[chosen + skip]));
    weak_wrapped(where_sentence(saved.on || without_off ? shown : saved, offers));
}

void PluginsTab::draw_module(const Snapshot& model, size_t i, size_t m, bool disabled, PluginsHost& host) {
    const PluginSnap& p = model.plugins[i];
    const ModuleSnap& mod = p.modules[m];
    ImGui::PushID(mod.id.c_str());
    // In the overlay: what this game does with it, and a change that waits.
    const auto in_game = [&] {
        if (!host.in_game()) return;
        if (const std::string text = in_game_text(module_in_game(model, p, mod)); !text.empty()) {
            ImGui::SameLine();
            weak(text);
        }
        if (!(mod.saved == mod.in_effect)) {
            ImGui::SameLine();
            ImGui::TextColored(kInfo, "%s", tr("at the next start"));
        }
    };
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
        in_game();
        describe();
        draw_where(model, i, mod.id, mod.saved, mod.offers, false, disabled, host);
    } else {
        bool on = mod.saved.on;
        ImGui::BeginDisabled(disabled);
        if (ImGui::Checkbox(mod.name.c_str(), &on)) host.apply(set_switch(p, mod.id, Which::On, on));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetItemTooltip("%s", (p.id + "." + mod.id).c_str());
        in_game();
        describe();
    }
    const SettingsLayout layout = settings_layout(p, mod.id);
    const size_t n = layout.shown.size() + layout.advanced.size();
    if (n > 0) {
        ImGui::Indent();
        if (mod.saved.on) {
            draw_settings_of(model, i, mod.id, disabled, host);
        } else if (ImGui::TreeNode(trf("Settings ({0}), kept while it is off", {std::to_string(n)}).c_str())) {
            draw_settings_of(model, i, mod.id, true, host);
            ImGui::TreePop();
        }
        const std::vector<Edit> reset = reset_module_settings(p, mod.id);
        if (reset.size() > 1) {
            ImGui::BeginDisabled(disabled);
            if (ImGui::SmallButton(trf("Reset settings ({0})...", {std::to_string(reset.size())}).c_str())) {
                ask_edits(model, host, trf("Reset the settings of {0}", {mod.name}), tr("Reset"), reset);
            }
            ImGui::EndDisabled();
        }
        ImGui::Unindent();
    }
    ImGui::Spacing();
    ImGui::PopID();
}

void PluginsTab::draw_settings_of(const Snapshot& model, size_t i, std::string_view module, bool disabled, PluginsHost& host) {
    const PluginSnap& p = model.plugins[i];
    const SettingsLayout layout = settings_layout(p, module);
    const auto draw_list = [&](const std::vector<size_t>& indices) {
        std::string group;
        for (const size_t s : indices) {
            const std::string& g = p.settings[s].group;
            if (g != group) {
                group = g;
                // A subheading, quieter than the section's separator above it.
                if (!group.empty()) {
                    ImGui::Spacing();
                    weak(group);
                }
            }
            draw_setting(model, i, s, disabled, host);
        }
    };
    draw_list(layout.shown);
    if (!layout.advanced.empty() && ImGui::TreeNode(trf("Advanced ({0})", {std::to_string(layout.advanced.size())}).c_str())) {
        draw_list(layout.advanced);
        ImGui::TreePop();
    }
}

void PluginsTab::draw_setting(const Snapshot& model, size_t i, size_t s, bool disabled, PluginsHost& host) {
    const PluginSnap& p = model.plugins[i];
    const SettingSnap& snap = p.settings[s];
    const bool locked = host.in_game() && snap.locked;
    ImGui::PushID(static_cast<int>(s));
    ImGui::BeginDisabled(disabled || locked);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    host.draw_setting_control(i, s, disabled || locked);
    ImGui::EndDisabled();
    // "(?)": its description and its default; a changed value says so in words, with Reset.
    ImGui::SameLine();
    weak("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
        if (!snap.description.empty()) ImGui::TextUnformatted(snap.description.c_str());
        ImGui::TextUnformatted(trf("Default: {0}", {host.shown_value(i, s, snap.default_value)}).c_str());
        weak(snap.name);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    if (host.in_game()) {
        // Settings apply at once in the game unless they say otherwise (ui-guidelines §3).
        if (locked) {
            ImGui::SameLine();
            weak(tr("locked in a multiplayer game"));
        } else if (snap.restart) {
            ImGui::SameLine();
            weak(tr("next start"));
        }
        if (setting_pending(snap)) {
            ImGui::SameLine();
            ImGui::TextColored(kInfo, "%s", trf("now: {0}", {host.shown_value(i, s, snap.in_effect)}).c_str());
        }
    }
    if (setting_changed(snap)) {
        ImGui::SameLine();
        ImGui::TextColored(kInfo, "%s", trf("changed · default: {0}", {host.shown_value(i, s, snap.default_value)}).c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(disabled || locked);
        if (ImGui::SmallButton(tr("Reset"))) host.apply(reset_setting(p, snap.name));
        ImGui::EndDisabled();
    }
    ImGui::PopID();
}

// ---- confirmations --------------------------------------------------------------------

void PluginsTab::ask(Confirm confirm) {
    confirm_ = std::move(confirm);
    open_confirm_ = true;
}

void PluginsTab::ask_edits(const Snapshot& model, const PluginsHost& host, std::string title, std::string button, std::vector<Edit> edits) {
    Confirm c;
    c.title = std::move(title);
    c.button = std::move(button);
    for (const Edit& e : edits) {
        const size_t i = index_of(model, e.plugin);
        if (i == SIZE_MAX) continue;
        const PluginSnap& p = model.plugins[i];
        if (e.kind == Edit::Kind::Setting) {
            if (const size_t s = setting_index(p, e.setting); s != SIZE_MAX) {
                c.lines.push_back(trf("{0}: {2} (now {1})", {p.settings[s].label, host.shown_value(i, s, p.settings[s].saved),
                                                         host.shown_value(i, s, e.value)}));
            }
            continue;
        }
        const ModuleSnap* mod = e.module.empty() ? nullptr : find_module(p, e.module);
        const std::string what = mod != nullptr ? mod->name : p.name;
        if (e.which == Which::On) {
            c.lines.push_back(trf("{0}: {2} (now {1})", {what, switch_word(!e.on), switch_word(e.on)}));
        } else {
            c.lines.push_back(trf(e.which == Which::Ironman ? i18n::N_("{0} in ironman: {2} (now {1})") : i18n::N_("{0} in multiplayer: {2} (now {1})"),
                                  {what, allow_word(!e.on), allow_word(e.on)}));
        }
    }
    c.edits = std::move(edits);
    ask(std::move(c));
}

void PluginsTab::ask_turn_on(const Snapshot& model, const Chain& chain) {
    Confirm c;
    std::vector<Edit> edits = edits_of(model, chain);
    c.title = trf("Turn on {0} plugin(s)", {std::to_string(edits.size())});
    c.button = c.title;
    for (const Chain::Step& step : chain.steps) {
        c.lines.push_back(step.needed_by.empty() ? name_of(model, step.id)
                                                 : trf("{0}, needed by {1}", {name_of(model, step.id), name_of(model, step.needed_by)}));
    }
    for (const std::string& id : chain.missing) c.lines.push_back(trf("{0} is not installed: install it first.", {id}));
    c.edits = std::move(edits);
    ask(std::move(c));
}

void PluginsTab::draw_confirm(PluginsHost& host) {
    if (open_confirm_) {
        ImGui::OpenPopup("###confirm");
        open_confirm_ = false;
    }
    // Centred on every frame, a fixed width and the height of its contents.
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34, 0), ImGuiCond_Always);
    if (!confirm_) return;
    if (!ImGui::BeginPopupModal((confirm_->title + "###confirm").c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    for (const std::string& line : confirm_->lines) bullet(line);
    ImGui::Spacing();
    ImGui::BeginDisabled(host.read_only() || (confirm_->edits.empty() && !confirm_->action));
    if (primary_button(confirm_->button.c_str())) {
        // The action may ask again: take this question first.
        Confirm c = std::move(*confirm_);
        confirm_.reset();
        ImGui::CloseCurrentPopup();
        if (c.action) {
            c.action();
        } else {
            host.apply(c.edits);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"))) {
        confirm_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}  // namespace anvil::ui
