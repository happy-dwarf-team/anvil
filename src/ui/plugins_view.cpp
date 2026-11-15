// The Plugins tab both apps draw (plugins_view.hpp). Moved from the Manager's
// plugins_tab.cpp when the overlay got the same tab; the Manager's pictures did
// not change.
#include "ui/plugins_view.hpp"

#include "i18n/translate.hpp"
#include "ui/forge_widgets.hpp"
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
using i18n::trn;

void wrapped(const std::string& text) { text_wrapped(text); }

void colored_wrapped(const ImVec4& color, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    wrapped(text);
    ImGui::PopStyleColor();
}

void weak(const std::string& text) { ImGui::TextDisabled("%s", text.c_str()); }

// SameLine for an item `width` wide, or a new line when the rest of this one is
// shorter (a setting row in the overlay's narrow panel).
// The new line starts at `indent` (window-local x) when it is above 0.
void same_line_if_fits(float width, float indent = 0.0f) {
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < width) {
        ImGui::NewLine();
        if (indent > 0.0f) ImGui::SetCursorPosX(indent);
    }
}

float text_width(const std::string& text) { return ImGui::CalcTextSize(text.c_str()).x; }

// Where setting_label wraps (window-local x; 0 is the content region's end),
// set by PluginsTab::draw_setting around the host's control, and where the
// label began (0 while no yes/no label was drawn): the notes that wrap under
// the row start there, so they read as the label's.
float g_label_wrap_x = 0.0f;
float g_label_x = 0.0f;


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

// A section of the list, untranslated (marked for the catalogs).
const char* section_name(Origin origin) {
    switch (origin) {
        case Origin::FromMod: return i18n::N_("From mods");
        case Origin::WithAnvil: return i18n::N_("Comes with Anvil");
        case Origin::Own: break;
    }
    return i18n::N_("Your plugins");
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
        add(trn("{0} setting|{0} settings", static_cast<long long>(line.settings)));
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
    begin_banner("waiting", Tone::Warn);
    ImGui::TextUnformatted(tr("Waiting for your approval:"));
    for (size_t i = 0; i < model.plugins.size(); ++i) {
        const PluginSnap& p = model.plugins[i];
        if (!waits_for_approval(p)) continue;
        // The version that waits: its row and its detail say whether it is an update.
        const std::string label = p.name + " " + (p.update_waits.empty() ? p.version : p.update_waits);
        const std::string key = entry_key(model, i);
        ImGui::SameLine();
        if (ImGui::GetContentRegionAvail().x < button_width(label.c_str())) ImGui::NewLine();
        if (text_button((label + "##wait-" + key).c_str())) selected = key;
    }
    end_banner(Tone::Warn);
}

void PluginsTab::draw_pending(const Snapshot& model, PluginsHost& host) {
    const size_t pending = pending_count(model);
    if (pending == 0) return;
    begin_banner("pending", Tone::Info);
    ImGui::TextUnformatted(trn("{0} change waits for the next start.|{0} changes wait for the next start.", static_cast<long long>(pending)).c_str());
    // Approvals and locked settings wait too, but Undo does not take them back: no button for nothing.
    const std::vector<Edit> undo = undo_pending(model);
    if (!undo.empty()) {
        ImGui::SameLine();
        ImGui::BeginDisabled(host.read_only());
        if (text_button(tr("Undo them"))) host.apply(undo, OnRefusal::GoOn);
        ImGui::EndDisabled();
    }
    end_banner(Tone::Info);
}

ModAction draw_mod_approval(const ModApproval& a) {
    ModAction action = ModAction::None;
    if (a.good) {
        state_row(Glyph::On, a.state);
    } else {
        begin_banner("mod-state", a.tone);
        wrapped(a.state);
    }
    for (const std::string& detail : a.details) weak_wrapped(detail);
    // When it was approved and where it comes from, on one line where it fits.
    if (!a.approved.empty()) weak(a.approved);
    if (!a.repository.empty()) {
        if (!a.approved.empty()) same_line_if_fits(button_width(tr("Open its repository")));
        if (text_button(tr("Open its repository"))) action = ModAction::OpenRepository;
    }
    ImGui::BeginDisabled(a.disabled);
    if (a.can_approve && primary_button(a.update ? tr("Approve the update...") : tr("Approve..."))) action = ModAction::Approve;
    if (a.can_revoke) {
        if (a.can_approve) ImGui::SameLine();
        if (ImGui::Button(tr("Revoke the approval"))) action = ModAction::Revoke;
    }
    ImGui::EndDisabled();
    if (!a.good) end_banner(a.tone);
    return action;
}

void PluginsTab::draw_panes(const Snapshot& model, PluginsHost& host) {
    if (selected.empty() || index_of_entry(model, selected) == SIZE_MAX) {
        // The list's first entry: sections go Own, FromMod, WithAnvil, each in the model's order.
        const auto first = std::min_element(model.plugins.begin(), model.plugins.end(),
                                            [](const PluginSnap& a, const PluginSnap& b) { return a.origin < b.origin; });
        selected = first == model.plugins.end() ? std::string()
                                                 : entry_key(model, static_cast<size_t>(first - model.plugins.begin()));
    }
    const float list_width = std::clamp(ImGui::GetContentRegionAvail().x * 0.34f, ImGui::GetFontSize() * 16, ImGui::GetFontSize() * 24);
    ImGui::BeginChild("list", ImVec2(list_width, 0), ImGuiChildFlags_Borders);
    draw_list(model, host);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("detail", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (const size_t i = index_of_entry(model, selected); i != SIZE_MAX) draw_detail(model, i, host);
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
        if (chip(label.c_str(), on)) chips ^= c.chip;
        ImGui::PopID();
    }

    const bool filtering = !find.empty() || chips != 0;
    size_t shown = 0;
    for (const PluginSnap& p : model.plugins) shown += passes(model, p, find, chips) ? 1 : 0;
    if (filtering) {
        ImGui::AlignTextToFramePadding();
        weak(trf("{0} of {1} plugins", {std::to_string(shown), std::to_string(model.plugins.size())}));
        ImGui::SameLine();
        if (text_button(tr("Clear"))) {
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
    const Section sections[] = {{Origin::Own, section_name(Origin::Own), "own"},
                                {Origin::FromMod, section_name(Origin::FromMod), "mods"},
                                {Origin::WithAnvil, section_name(Origin::WithAnvil), "anvil"}};
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
        const bool holds_selection = std::any_of(members.begin(), members.end(), [&](size_t i) { return entry_key(model, i) == selected; });
        ImGui::SetNextItemOpen(section.origin != Origin::WithAnvil || holds_selection || filtering,
                               filtering || holds_selection ? ImGuiCond_Always : ImGuiCond_Once);
        // Hovered `raised`, as a row: ember is the selection's colour.
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kRaised);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, kRaised);
        const bool open = ImGui::TreeNodeEx((std::string(tr(section.name)) + " (" + count + ")###" + section.id).c_str(),
                                            ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                ImGuiTreeNodeFlags_AllowOverlap);
        ImGui::PopStyleColor(2);
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
            const std::string key = entry_key(model, i);
            if (list_row(("##entry-" + key).c_str(), key == selected, third ? 3 : 2)) selected = key;
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const float x = row_text_x(at);
            // A line that does not fit ends in "…"; the tooltip has it whole.
            const float dot = v.pending > 0 ? line : 0.0f;
            const float right = ImGui::GetItemRectMax().x - ImGui::GetStyle().ItemInnerSpacing.x;
            bool cut_name = false, cut_line = false, cut_where = false;
            draw_glyph(draw, ImVec2(row_glyph_x(at), at.y + pad), line, v.glyph);
            const float w = draw_strong(draw, ImVec2(x, at.y + pad), ImGui::GetColorU32(v.load.loads ? kText : kTextWeak), p.name.c_str(),
                                        right - dot, &cut_name);
            if (v.pending > 0) draw_pending_dot(draw, ImVec2(x + w + line * 0.5f, at.y + pad + line * 0.5f), line * 0.18f);
            const std::string second = entry_line(v.line);
            draw_text_fit(draw, ImGui::GetFont(), ImVec2(x, at.y + pad + line), right, ImGui::GetColorU32(entry_color(v.line)), second,
                          &cut_line);
            std::string where;
            if (third) {
                where = match.where == Match::Where::Module ? trf("found in module: {0}", {match.what})
                                                            : trf("found in setting: {0}", {match.what});
                draw_text_fit(draw, ImGui::GetFont(), ImVec2(x, at.y + pad + line * 2), right, ImGui::GetColorU32(kInfo), where, &cut_where);
            }
            if (hovered) {
                std::string tip = p.name + " " + p.version + "\n" + p.id;
                if (cut_name || cut_line || cut_where) tip += "\n\n" + second + (third ? "\n" + where : "");
                ImGui::SetTooltip("%s", tip.c_str());
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

    // The header (ui-guidelines §5): the glyph, the name and version at the left; at the right
    // Reset and the app's buttons, then the plugin's own switch in a place of its own, as wide
    // as its longer word, so it stays put from plugin to plugin and when it is clicked.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float gap = style.ItemSpacing.x * 3;
    // Another entry of this id: its settings are the first entry's to change, shown here greyed.
    const bool settings_shown_only = settings_read_only(model, i);
    std::vector<Edit> reset = reset_plugin(p);
    if (settings_shown_only) std::erase_if(reset, [](const Edit& e) { return e.kind == Edit::Kind::Setting; });
    const bool buttons = host.has_header_buttons(i);
    const float toggle_w = ImGui::GetFrameHeight() * 1.9f + style.ItemSpacing.x +
                           std::max(ImGui::CalcTextSize(tr("On")).x, ImGui::CalcTextSize(tr("Off")).x);
    float buttons_w = 0;
    if (!reset.empty()) buttons_w += button_width(tr("Reset to defaults...")) + style.ItemSpacing.x;
    if (buttons) buttons_w += host.header_buttons_width(i) + style.ItemSpacing.x;
    if (buttons_w > 0) buttons_w += ImGui::CalcTextSize("|").x + style.ItemSpacing.x;
    const float controls_w = buttons_w + toggle_w;
    const float left = ImGui::GetCursorPosX();
    const float right = left + ImGui::GetContentRegionAvail().x;
    glyph_item(v.glyph);
    ImGui::SameLine();
    // The name and version, cut with "…" where the controls begin; on a narrow pane the
    // controls take the next line rather than leave the name no room.
    ImFont* strong_font =g_title_font != nullptr ? g_title_font : ImGui::GetFont();
    const float name_w = strong_font->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, p.name.c_str()).x + style.ItemSpacing.x +
                         ImGui::CalcTextSize(p.version.c_str()).x;
    const bool one_line = right - ImGui::GetCursorPosX() - gap - controls_w >= std::min(name_w, ImGui::GetFontSize() * 6);
    {
        const float name_right = one_line ? right - controls_w - gap : right;
        ImGui::AlignTextToFramePadding();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float to = at.x + (name_right - ImGui::GetCursorPosX());
        const float y = at.y + style.FramePadding.y;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        bool cut_name = false, cut_version = false;
        const float w = draw_strong(draw, ImVec2(at.x, y), ImGui::GetColorU32(kText), p.name.c_str(), to, &cut_name);
        const float vx = at.x + w + style.ItemSpacing.x;
        if (!cut_name && vx < to) draw_text_fit(draw, ImGui::GetFont(), ImVec2(vx, y), to, ImGui::GetColorU32(kTextWeak), p.version, &cut_version);
        ImGui::Dummy(ImVec2(std::max(0.0f, to - at.x), ImGui::GetFrameHeight()));
        if ((cut_name || cut_version) && ImGui::IsItemHovered()) ImGui::SetTooltip("%s %s", p.name.c_str(), p.version.c_str());
    }
    if (one_line) ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - controls_w));
    ImGui::BeginDisabled(locked);
    if (!reset.empty()) {
        if (ImGui::Button(tr("Reset to defaults..."))) ask_edits(model, host, trf("Reset {0} to defaults", {p.name}), tr("Reset"), reset);
        ImGui::SameLine();
    }
    if (buttons) {
        host.draw_header_buttons(i);
        ImGui::SameLine();
    }
    ImGui::EndDisabled();
    if (buttons_w > 0) {
        ImGui::AlignTextToFramePadding();
        weak("|");
        ImGui::SameLine();
    }
    ImGui::SetCursorPosX(right - toggle_w);
    toggle_left_ = ImGui::GetCursorScreenPos().x;
    const std::string refusal = host.switch_refusal(i);
    if (toggle("##on", p.saved.on, p.saved.on ? tr("On") : tr("Off"), locked || !known || !refusal.empty(), false,
               refusal.empty() ? nullptr : refusal.c_str())) {
        if (p.saved.on) {
            host.apply(set_switch(p, "", Which::On, false), OnRefusal::Stop);
        } else {
            const Chain chain = turn_on_chain(model, p.id);
            if (chain.steps.size() > 1 || !chain.missing.empty()) {
                ask_turn_on(model, chain);
            } else {
                host.apply(set_switch(p, "", Which::On, true), OnRefusal::Stop);
            }
        }
    }
    if (!refusal.empty()) weak_wrapped(refusal);

    // The rest scrolls as one, so its modules and settings get the whole pane
    // however much the facts above them say; the header stays in sight.
    ImGui::Separator();
    ImGui::BeginChild("body", ImVec2(0, 0), ImGuiChildFlags_None);
    // What the player needs to know about it, in the order it matters.
    host.draw_facts(i);
    if (v.load.why == LoadState::Why::CannotLoad) {
        banner("cannot-load", Tone::Bad,
               v.load.reason.empty() ? std::string(tr("It cannot load.")) : trf("It cannot load: {0}", {v.load.reason}));
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
        begin_banner("needs", Tone::Warn);
        text_wrapped(text);
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
        end_banner(Tone::Warn);
    }
    if (!v.needed_by.empty() && p.saved.on) {
        weak_wrapped(trf("Needed by {0}. Turning it off stops it too; it keeps its own switch.", {names_of(model, v.needed_by)}));
    }
    if (!p.description.empty()) wrapped(p.description);
    host.draw_notes(i);
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
    if (settings_shown_only && !p.settings.empty()) {
        const size_t first = index_of(model, p.id);
        weak_wrapped(trf("Its settings are changed in the first entry of {0}, under \"{1}\".",
                         {p.name, tr(section_name(model.plugins[first].origin))}));
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
            // One row per module, a hairline between them.
            for (size_t k = 0; k < group.modules.size(); ++k) {
                if (k > 0) {
                    ImGui::PushStyleColor(ImGuiCol_Separator, kLine);
                    ImGui::Separator();
                    ImGui::PopStyleColor();
                }
                draw_module(model, i, group.modules[k], locked || plugin_off, settings_shown_only, host);
            }
            if (!group.name.empty()) ImGui::TreePop();
        }
    }
    const SettingsLayout own = settings_layout(p, "");
    if (!own.shown.empty() || !own.advanced.empty()) {
        ImGui::SeparatorText(tr("Settings"));
        draw_settings_of(model, i, "", locked || settings_shown_only, host);
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
    if (chosen >= 0) host.apply(set_where(p, module, choices[chosen + skip]), OnRefusal::Stop);
    weak_wrapped(where_sentence(saved.on || without_off ? shown : saved, offers));
}

void PluginsTab::draw_module(const Snapshot& model, size_t i, size_t m, bool disabled, bool settings_shown_only, PluginsHost& host) {
    const PluginSnap& p = model.plugins[i];
    const ModuleSnap& mod = p.modules[m];
    ImGui::PushID(mod.id.c_str());
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool on = mod.saved.on;
    const std::string key = p.id + "." + mod.id;
    const SettingsLayout layout = settings_layout(p, mod.id);
    const size_t n = layout.shown.size() + layout.advanced.size();
    const std::size_t changed = changed_module_settings(p, mod.id);

    // One line: its name, its facts, a chevron and its switch; a click anywhere on the
    // line opens what it does, where it runs and its settings. The facts say what the
    // switch does not: that it changes the game (while it is off and its choice of
    // games is hidden), its settings, what was changed, and in the overlay what this
    // game does with it. Where it runs is in the open part only.
    struct Part {
        std::string text;
        ImVec4 color;
    };
    std::vector<Part> facts;
    if (mod.changes_game && !on) facts.push_back({tr("changes the game"), kTextWeak});
    if (n > 0) facts.push_back({trn("{0} setting|{0} settings", static_cast<long long>(n)), kTextWeak});
    if (changed > 0) facts.push_back({trf("{0} changed", {std::to_string(changed)}), kInfo});
    if (host.in_game()) {
        if (std::string text = in_game_text(module_in_game(model, p, mod)); !text.empty()) facts.push_back({std::move(text), kTextWeak});
        if (!(mod.saved == mod.in_effect)) facts.push_back({tr("at the next start"), kInfo});
    }
    std::string all_facts;
    for (const Part& part : facts) all_facts += (all_facts.empty() ? "" : " \xC2\xB7 ") + part.text;

    // Opened at first: a module whose settings were changed. Decided once, so a
    // row does not close under the cursor when its last change is reset, nor
    // open when a setting changes from outside.
    ImGuiStorage* storage = ImGui::GetStateStorage();
    int* const open_state = storage->GetIntRef(ImGui::GetID("open"), -1);
    if (*open_state < 0) *open_state = changed > 0 ? 1 : 0;
    bool open = *open_state != 0;
    const float line = ImGui::GetTextLineHeight();
    const float frame = ImGui::GetFrameHeight();
    const float h = frame + 2;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float right_edge = at.x + ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kFrameHovered);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, kFrameActive);
    if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, h))) {
        open = !open;
        *open_state = open ? 1 : 0;
    }
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26);
        ImGui::TextUnformatted(mod.name.c_str());
        if (!all_facts.empty()) weak(all_facts);
        if (!mod.description.empty()) ImGui::TextUnformatted(mod.description.c_str());
        weak(key);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    // The switch lines up under the plugin's; on a narrow pane it keeps the right edge
    // and drops its word before it leaves the line.
    const float word_w = std::max(ImGui::CalcTextSize(tr("On")).x, ImGui::CalcTextSize(tr("Off")).x);
    const float need = toggle_track_width(true) + style.ItemSpacing.x + word_w;
    const float wanted = std::min(toggle_left_ > at.x ? toggle_left_ : right_edge - need, right_edge - need);
    const bool room = wanted - at.x > ImGui::GetFontSize() * 8;
    const float toggle_x = room ? wanted : right_edge - toggle_track_width(true);
    const float chevron_x = toggle_x - style.ItemSpacing.x - line;
    const float text_right = chevron_x - style.ItemSpacing.x;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float x = at.x + style.FramePadding.x;
    const float y = at.y + (h - line) * 0.5f;
    bool cut = false;
    const float name_w = draw_strong(draw, ImVec2(x, y), ImGui::GetColorU32(on && !disabled ? kText : kTextWeak), mod.name.c_str(), text_right, &cut);
    // The facts after the name, cut with "..."; with room for less than a few letters,
    // none (the tooltip has them).
    float fx = x + name_w + style.ItemSpacing.x * 1.5f;
    if (!cut && text_right - fx > ImGui::GetFontSize() * 4) {
        for (size_t k = 0; k < facts.size() && !cut; ++k) {
            const std::string text = (k > 0 ? " \xC2\xB7 " : "") + facts[k].text;
            fx += draw_text_fit(draw, ImGui::GetFont(), ImVec2(fx, y), text_right, ImGui::GetColorU32(facts[k].color), text, &cut);
        }
    }
    // The chevron: > closed, v open.
    {
        const ImU32 color = ImGui::GetColorU32(kTextWeak);
        const float s = line * 0.22f;
        const float thickness = std::max(1.5f, line * 0.09f);
        const ImVec2 c(chevron_x + line * 0.5f, at.y + h * 0.5f);
        if (open) {
            draw->AddLine(ImVec2(c.x - s, c.y - s * 0.5f), ImVec2(c.x, c.y + s * 0.5f), color, thickness);
            draw->AddLine(ImVec2(c.x, c.y + s * 0.5f), ImVec2(c.x + s, c.y - s * 0.5f), color, thickness);
        } else {
            draw->AddLine(ImVec2(c.x - s * 0.5f, c.y - s), ImVec2(c.x + s * 0.5f, c.y), color, thickness);
            draw->AddLine(ImVec2(c.x + s * 0.5f, c.y), ImVec2(c.x - s * 0.5f, c.y + s), color, thickness);
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(toggle_x, at.y + (h - frame) * 0.5f));
    if (toggle("##on", on, room ? (on ? tr("On") : tr("Off")) : nullptr, disabled, true)) host.apply(set_switch(p, mod.id, Which::On, !on), OnRefusal::Stop);
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + h));
    ImGui::Dummy(ImVec2(0, 0));
    if (!open) {
        ImGui::PopID();
        return;
    }

    // The open part, under the name: what it does, where it runs (while it is on; its
    // switch turns it off), its settings.
    ImGui::Indent(style.FramePadding.x);
    if (!mod.description.empty()) weak_wrapped(mod.description);
    if (mod.changes_game && on) draw_where(model, i, mod.id, mod.saved, mod.offers, true, disabled, host);
    if (n > 0) {
        if (on) {
            draw_settings_of(model, i, mod.id, disabled || settings_shown_only, host);
        } else if (ImGui::TreeNode(trf("Settings ({0}), kept while it is off", {std::to_string(n)}).c_str())) {
            draw_settings_of(model, i, mod.id, true, host);
            ImGui::TreePop();
        }
        // Locked settings stay out of the reset (reset_module_settings).
        if (const std::vector<Edit> resets = reset_module_settings(p, mod.id); resets.size() > 1) {
            ImGui::BeginDisabled(disabled || settings_shown_only);
            if (text_button(trf("Reset settings ({0})...", {std::to_string(resets.size())}).c_str())) {
                ask_edits(model, host, trf("Reset the settings of {0}", {mod.name}), tr("Reset"), resets);
            }
            ImGui::EndDisabled();
        }
    }
    ImGui::Unindent(style.FramePadding.x);
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

void setting_label(const std::string& label) {
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::AlignTextToFramePadding();
    g_label_x = ImGui::GetCursorPosX();
    text_wrapped_at(label, g_label_wrap_x);
}

void PluginsTab::draw_setting(const Snapshot& model, size_t i, size_t s, bool disabled, PluginsHost& host) {
    const PluginSnap& p = model.plugins[i];
    const SettingSnap& snap = p.settings[s];
    const bool locked = host.in_game() && snap.locked;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    // The notes after the control, measured first: a yes/no label wraps under
    // itself so that they stay on its first line (label_reserve).
    std::string note, now, changed;
    if (host.in_game()) {
        // Settings apply at once in the game unless they say otherwise (ui-guidelines §3).
        if (locked || snap.restart) note = locked ? tr("locked in a multiplayer game") : tr("next start");
        const bool pending = setting_pending(snap);
        if (pending || !snap.overridden.empty()) now = trf("now: {0}", {host.shown_value(i, s, pending ? snap.in_effect : snap.overridden)});
    }
    if (setting_changed(snap)) changed = trf("changed · default: {0}", {host.shown_value(i, s, snap.default_value)});
    float trailing = gap + text_width("(?)");
    if (!note.empty()) trailing += gap + text_width(note);
    if (!now.empty()) trailing += gap + text_width(now);
    if (!changed.empty()) trailing += gap + text_width(changed) + gap + button_width(tr("Reset"));
    const float avail = ImGui::GetContentRegionAvail().x;
    const float reserve = label_reserve(avail, trailing, ImGui::GetFontSize() * 12);
    ImGui::PushID(static_cast<int>(s));
    ImGui::BeginDisabled(disabled || locked);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    g_label_wrap_x = reserve > 0.0f ? ImGui::GetCursorPosX() + avail - reserve : 0.0f;
    g_label_x = 0.0f;
    host.draw_setting_control(i, s, disabled || locked);
    g_label_wrap_x = 0.0f;
    const float indent = g_label_x;
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
    // What follows wraps to its own line, under a yes/no label, when the panel is too narrow for it.
    if (!note.empty()) {
        same_line_if_fits(text_width(note), indent);
        weak(note);
    }
    if (!now.empty()) {
        same_line_if_fits(text_width(now), indent);
        ImGui::TextColored(kInfo, "%s", now.c_str());
    }
    if (!changed.empty()) {
        same_line_if_fits(text_width(changed) + gap + button_width(tr("Reset")), indent);
        ImGui::TextColored(kInfo, "%s", changed.c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(disabled || locked);
        if (text_button(tr("Reset"))) host.apply(reset_setting(p, snap.name), OnRefusal::GoOn);
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
    c.on_refusal = OnRefusal::GoOn;  // a reset: one refusal leaves the rest to go on
    ask(std::move(c));
}

void PluginsTab::ask_turn_on(const Snapshot& model, const Chain& chain) {
    Confirm c;
    std::vector<Edit> edits = edits_of(model, chain);
    c.title = trn("Turn on {0} plugin|Turn on {0} plugins", static_cast<long long>(edits.size()));
    c.button = c.title;
    for (const Chain::Step& step : chain.steps) {
        c.lines.push_back(step.needed_by.empty() ? name_of(model, step.id)
                                                 : trf("{0}, needed by {1}", {name_of(model, step.id), name_of(model, step.needed_by)}));
    }
    for (const std::string& id : chain.missing) c.lines.push_back(trf("{0} is not installed: install it first.", {id}));
    c.edits = std::move(edits);
    c.on_refusal = OnRefusal::Stop;  // dependencies first: none is turned on past one that was refused
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
    if (!begin_dialog("###confirm", confirm_->title)) return;
    for (const std::string& line : confirm_->lines) bullet(line);
    dialog_buttons({confirm_->button.c_str(), tr("Cancel")});
    ImGui::BeginDisabled(host.read_only() || (confirm_->edits.empty() && !confirm_->action));
    if (primary_button(confirm_->button.c_str())) {
        // The action may ask again: take this question first.
        Confirm c = std::move(*confirm_);
        confirm_.reset();
        ImGui::CloseCurrentPopup();
        if (c.action) {
            c.action();
        } else {
            host.apply(c.edits, c.on_refusal);
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
