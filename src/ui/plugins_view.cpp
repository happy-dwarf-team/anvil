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
#include <cstdlib>
#pragma warning(disable : 4996)  // PROTOTYPE: getenv
#include <string_view>

namespace anvil::ui {

namespace {

using i18n::tr;
using i18n::trf;
using i18n::trn;

// PROTOTYPE (branch prototype/module-toggles): three layouts of a module's switch,
// picked with ANVIL_PROTO_VARIANT=A|B|C or the floating bar (Ctrl+Left / Ctrl+Right).
int g_proto_variant = [] {
    char e[4] = {};
    size_t n = 0;
    getenv_s(&n, e, sizeof e, "ANVIL_PROTO_VARIANT");
    const int k = std::atoi(e);
    return k >= 0 && k < 12 ? k : 0;
}();
const char* const kProtoNames[] = {"C   the card as shown", "C1  compact cards, tags, folded settings",
                                   "C2  ember edge on the cards that are on", "C3  one grouped card, a row per module",
                                   "C4  a row per module that opens", "C5  two-line rows: name, then its state",
                                   "C6  C5, and the open row is a card (one at a time)", "C7  rows like the plugin list: glyph, chevron at the right",
                                   "C8  C4: the whole row opens, its facts cut with ...", "C9  C8, and the open row is a card (one at a time)",
                                   "C10 C8 with its facts at the right, by the toggle", "C11 C8 with lines between rows and the chevron at the right"};
constexpr int kProtoCount = 12;
std::string g_open_key;   // C6: the one open module (plugin.module)
std::string g_open_init;  // C6: the plugin it was chosen for
bool g_first_module = true;  // C3: no line above the first row
float g_toggle_left = 0;  // the plugin toggle's track, in screen x

float g_tags_limit = FLT_MAX;  // screen x a tag must end before (the toggle's place)
bool proto_tag(const std::string& text, ImVec4 color) {
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    const float px = ImGui::GetFontSize() * 0.4f;
    const float end = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + size.x + px * 2;
    if (end > g_tags_limit) return false;
    ImGui::SameLine();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float frame = ImGui::GetFrameHeight();
    const float h = size.y + 2;
    const ImVec2 top(at.x, at.y + (frame - h) * 0.5f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(top, ImVec2(top.x + size.x + px * 2, top.y + h), ImGui::GetColorU32(kFrame), h * 0.5f);
    draw->AddText(ImVec2(top.x + px, top.y + 1), ImGui::GetColorU32(color), text.c_str());
    ImGui::Dummy(ImVec2(size.x + px * 2, frame));
    return true;
}

void proto_switcher() {
    if (ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) g_proto_variant = (g_proto_variant + kProtoCount - 1) % kProtoCount;
    if (ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) g_proto_variant = (g_proto_variant + 1) % kProtoCount;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 8, vp->WorkPos.y + 8), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.25f, 0.45f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.5f, 0.75f, 1, 1));
    ImGui::Begin("##proto", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav);
    if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) g_proto_variant = (g_proto_variant + kProtoCount - 1) % kProtoCount;
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("PROTOTYPE  %s", kProtoNames[g_proto_variant]);
    ImGui::SameLine();
    if (ImGui::ArrowButton("##next", ImGuiDir_Right)) g_proto_variant = (g_proto_variant + 1) % kProtoCount;
    ImGui::End();
    ImGui::PopStyleColor(2);
}

void wrapped(const std::string& text) { text_wrapped(text); }

void colored_wrapped(const ImVec4& color, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    wrapped(text);
    ImGui::PopStyleColor();
}

void weak(const std::string& text) { ImGui::TextDisabled("%s", text.c_str()); }

// SameLine for an item `width` wide, or a new line when the rest of this one is
// shorter (a setting row in the overlay's narrow panel).
void same_line_if_fits(float width) {
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < width) ImGui::NewLine();
}

float text_width(const std::string& text) { return ImGui::CalcTextSize(text.c_str()).x; }


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
    for (const PluginSnap& p : model.plugins) {
        if (!(p.blocked == Blocked::WaitsForApproval && !p.approved) && p.update_waits.empty()) continue;
        // The version that waits: its row and its detail say whether it is an update.
        const std::string label = p.name + " " + (p.update_waits.empty() ? p.version : p.update_waits);
        ImGui::SameLine();
        if (ImGui::GetContentRegionAvail().x < button_width(label.c_str())) ImGui::NewLine();
        if (text_button((label + "##wait-" + p.id).c_str())) selected = p.id;
    }
    end_banner(Tone::Warn);
}

void PluginsTab::draw_pending(const Snapshot& model, PluginsHost& host) {
    const size_t pending = pending_count(model);
    if (pending == 0) return;
    begin_banner("pending", Tone::Info);
    ImGui::TextUnformatted(trn("{0} change waits for the next start.|{0} changes wait for the next start.", static_cast<long long>(pending)).c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(host.read_only());
    if (text_button(tr("Undo them"))) host.apply(undo_pending(model));
    ImGui::EndDisabled();
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
            if (list_row(("##entry-" + p.id).c_str(), p.id == selected, third ? 3 : 2)) selected = p.id;
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
    const std::vector<Edit> reset = reset_plugin(p);
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
    g_toggle_left = ImGui::GetCursorScreenPos().x;
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
    if (!refusal.empty()) weak_wrapped(refusal);

    // The rest scrolls as one, so its modules and settings get the whole pane
    // however much the facts above them say; the header stays in sight.
    ImGui::Separator();
    ImGui::BeginChild("body", ImVec2(0, 0), ImGuiChildFlags_None);
    // What the player needs to know about it, in the order it matters.
    host.draw_facts(i);
    if (p.blocked == Blocked::CannotLoad) {
        banner("cannot-load", Tone::Bad,
               p.blocked_reason.empty() ? std::string(tr("It cannot load.")) : trf("It cannot load: {0}", {p.blocked_reason}));
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
    if (!p.modules.empty()) {
        ImGui::SeparatorText(trf("Modules ({0})", {std::to_string(p.modules.size())}).c_str());
        g_first_module = true;
        const bool grouped = g_proto_variant == 3;
        if (grouped) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, kRaised);
            ImGui::PushStyleColor(ImGuiCol_Border, kLine);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetFontSize() * 0.75f, ImGui::GetFontSize() * 0.5f));
            ImGui::BeginChild("modules-card", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        }
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
        if (grouped) {
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
        }
    }
    const SettingsLayout own = settings_layout(p, "");
    if (!own.shown.empty() || !own.advanced.empty()) {
        ImGui::SeparatorText(tr("Settings"));
        draw_settings_of(model, i, "", locked, host);
    }
    if (std::getenv("ANVIL_PROTO_SCROLL") != nullptr) ImGui::SetScrollY(ImGui::GetScrollMaxY());  // PROTOTYPE
    ImGui::EndChild();
    ImGui::PopID();
    proto_switcher();
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
    // Its name, what it does, then its switch: a checkbox, or where it runs when it
    // changes the game. Every name, description and control under it starts in one
    // column: past the checkbox's box.
    const auto describe = [&] {
        if (!mod.description.empty()) weak_wrapped(mod.description);
    };
    const auto name_line = [&] {
        ImGui::BeginDisabled(disabled);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(mod.name.c_str());
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetItemTooltip("%s", (p.id + "." + mod.id).c_str());
        in_game();
    };
    // The module's toggle with its word, its track under the plugin's own (or at the right edge when that is too far).
    const auto right_toggle = [&](float right_edge) {
        const ImGuiStyle& st = ImGui::GetStyle();
        const float word_w = std::max(ImGui::CalcTextSize(tr("On")).x, ImGui::CalcTextSize(tr("Off")).x);
        const float need = toggle_track_width(true) + st.ItemSpacing.x + word_w;
        const float x = std::min(g_toggle_left, right_edge - need);
        ImGui::SameLine();
        const float at = ImGui::GetCursorScreenPos().x;
        bool on = mod.saved.on;
        // A narrow pane: the word goes before the toggle leaves the row.
        const bool room = at <= x;
        ImGui::SetCursorScreenPos(ImVec2(room ? x : std::max(at, right_edge - toggle_track_width(true)), ImGui::GetCursorScreenPos().y));
        if (toggle("##on", on, room ? (on ? tr("On") : tr("Off")) : nullptr, disabled, true)) host.apply(set_switch(p, mod.id, Which::On, !on));
    };
    const auto settings = [&] {
        const SettingsLayout layout = settings_layout(p, mod.id);
        const size_t n = layout.shown.size() + layout.advanced.size();
        if (n == 0) return;
        if (mod.saved.on) {
            draw_settings_of(model, i, mod.id, disabled, host);
        } else if (ImGui::TreeNode(trf("Settings ({0}), kept while it is off", {std::to_string(n)}).c_str())) {
            draw_settings_of(model, i, mod.id, true, host);
            ImGui::TreePop();
        }
    };
    const int v = g_proto_variant;
    const std::vector<Edit> changed = reset_module_settings(p, mod.id);
    const SettingsLayout layout = settings_layout(p, mod.id);
    const size_t n_settings = layout.shown.size() + layout.advanced.size();
    const bool on = mod.saved.on;
    // The header's facts as tags (C1 on): what kind of module it is and what was changed.
    // Tags that do not fit before the toggle are left out of the line (C4 shows them when opened).
    const auto tags = [&](float right_edge) {
        if (v == 0) return;
        const float word_w = std::max(ImGui::CalcTextSize(tr("On")).x, ImGui::CalcTextSize(tr("Off")).x);
        g_tags_limit = std::min(g_toggle_left, right_edge - toggle_track_width(true) - ImGui::GetStyle().ItemSpacing.x - word_w) -
                       ImGui::GetStyle().ItemSpacing.x;
        if (!changed.empty()) proto_tag(trf("{0} changed", {std::to_string(changed.size())}), kInfo);
        if (mod.changes_game) proto_tag(tr("changes the game"), kTextWeak);
        g_tags_limit = FLT_MAX;
    };
    const auto title = [&](ImVec4 color) {
        ImGui::AlignTextToFramePadding();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float w = draw_strong(ImGui::GetWindowDrawList(), ImVec2(at.x, at.y + ImGui::GetStyle().FramePadding.y), ImGui::GetColorU32(color), mod.name.c_str());
        ImGui::Dummy(ImVec2(w, ImGui::GetFrameHeight()));
        if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("%s", (p.id + "." + mod.id).c_str());
        in_game();
    };
    // The settings: open (C, C2, C3) or folded behind one line that says how many (C1, C4).
    const auto settings_folded = [&] {
        if (n_settings == 0) return;
        const std::string label = changed.empty() ? trf("Settings ({0})", {std::to_string(n_settings)})
                                                  : trf("Settings ({0}, {1} changed)", {std::to_string(n_settings), std::to_string(changed.size())});
        if (ImGui::TreeNode((label + "###settings").c_str())) {
            draw_settings_of(model, i, mod.id, disabled || !on, host);
            ImGui::TreePop();
        }
    };
    const auto body = [&](bool folded) {
        describe();
        if (mod.changes_game && on) draw_where(model, i, mod.id, mod.saved, mod.offers, true, disabled, host);
        if (folded) {
            settings_folded();
        } else {
            settings();
        }
    };
    if (v == 3) {
        // C3: one card for all modules; a thin line between rows.
        if (!g_first_module) {
            ImGui::PushStyleColor(ImGuiCol_Separator, kLine);
            ImGui::Separator();
            ImGui::PopStyleColor();
        }
        g_first_module = false;
        const float right_edge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        title(on ? kText : kTextWeak);
        tags(right_edge);
        right_toggle(right_edge);
        body(true);
        ImGui::PopID();
        return;
    }
    if (v >= 5) {
        // C5 to C11: a row that opens anywhere it is clicked, its facts cut with "..." before the
        // toggle (the full text in the tooltip), the toggle in its place.
        const bool two_line = v == 5 || v == 6 || v == 7;
        const bool glyph = v == 7;
        const bool chevron_right = v == 7 || v == 11;
        const bool accordion = v == 6 || v == 9;
        const bool facts_right = v == 10;
        const bool lines = v == 11;
        const ImGuiStyle& st = ImGui::GetStyle();
        const float line = ImGui::GetTextLineHeight();
        const float frame = ImGui::GetFrameHeight();
        const std::string key = p.id + "." + mod.id;
        if (accordion && g_open_init != p.id) {
            g_open_init = p.id;
            g_open_key.clear();
            for (const ModuleSnap& other : p.modules) {
                if (!reset_module_settings(p, other.id).empty()) {
                    g_open_key = p.id + "." + other.id;
                    break;
                }
            }
        }
        if (lines) {
            if (!g_first_module) {
                ImGui::PushStyleColor(ImGuiCol_Separator, kLine);
                ImGui::Separator();
                ImGui::PopStyleColor();
            }
            g_first_module = false;
        }
        ImGuiStorage* storage = ImGui::GetStateStorage();
        const ImGuiID open_id = ImGui::GetID("open");
        bool open = accordion ? g_open_key == key : storage->GetBool(open_id, !changed.empty());
        // Its facts, what the toggle does not say: where it runs (or that it changes the game) and
        // its settings; on two lines, with nothing of that, the start of what it does.
        std::string facts;
        const auto add = [&](const std::string& part) { facts += (facts.empty() ? "" : " \xC2\xB7 ") + part; };
        if (mod.changes_game) {
            const char* long_words[] = {tr("Off"), tr("Normal games"), tr("Normal games + Ironman"), tr("Normal games + Multiplayer"), tr("Everywhere")};
            const char* short_words[] = {tr("Off"), tr("Normal games"), tr("+ Ironman"), tr("+ Multiplayer"), tr("Everywhere")};
            const int w = static_cast<int>(where(mod.saved));
            add(on ? std::string(two_line ? long_words[w] : short_words[w]) : std::string(tr("Changes the game")));
        }
        if (n_settings > 0) add(trf("{0} settings", {std::to_string(n_settings)}));
        if (facts.empty() && two_line) facts = mod.description;
        const std::string changed_text = changed.empty() ? std::string() : trf("{0} changed", {std::to_string(changed.size())});
        const bool card = accordion && open;
        if (card) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, kRaised);
            ImGui::PushStyleColor(ImGuiCol_Border, kLine);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetFontSize() * 0.25f, ImGui::GetFontSize() * 0.25f));
            ImGui::BeginChild("card", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        }
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float right_edge = at.x + ImGui::GetContentRegionAvail().x;
        const float h = two_line ? line * 2 + st.FramePadding.y * 2 + 2 : line + st.FramePadding.y * 2 + 2;
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kFrameHovered);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, kFrameActive);
        if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, h))) {
            open = !open;
            if (accordion) {
                g_open_key = open ? key : std::string();
            } else {
                storage->SetBool(open_id, open);
            }
        }
        ImGui::PopStyleColor(2);
        const bool row_hovered = ImGui::IsItemHovered();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float word_w = std::max(ImGui::CalcTextSize(tr("On")).x, ImGui::CalcTextSize(tr("Off")).x);
        const float need = toggle_track_width(true) + st.ItemSpacing.x + word_w;
        const bool room = std::min(g_toggle_left, right_edge - need) - at.x > ImGui::GetFontSize() * 8;
        const float toggle_x = room ? std::min(g_toggle_left, right_edge - need) : right_edge - toggle_track_width(true);
        const float chevron = chevron_right ? line : 0.0f;
        const float text_right = toggle_x - st.ItemSpacing.x * 2 - chevron;
        float x = at.x + st.FramePadding.x;
        const float y1 = at.y + (two_line ? st.FramePadding.y : (h - line) * 0.5f);
        const float y2 = y1 + line + 2;
        const ImU32 dim = ImGui::GetColorU32(kTextWeak);
        if (glyph) {
            draw_glyph(draw, ImVec2(x, y1), line, on ? Glyph::On : Glyph::Off);
            x += line + st.ItemInnerSpacing.x * 1.5f;
        } else if (!chevron_right) {
            const float s = line * 0.32f;
            const ImVec2 c(x + line * 0.5f, y1 + line * 0.5f);
            if (open) {
                draw->AddTriangleFilled(ImVec2(c.x - s, c.y - s * 0.6f), ImVec2(c.x + s, c.y - s * 0.6f), ImVec2(c.x, c.y + s * 0.8f), dim);
            } else {
                draw->AddTriangleFilled(ImVec2(c.x - s * 0.6f, c.y - s), ImVec2(c.x - s * 0.6f, c.y + s), ImVec2(c.x + s * 0.8f, c.y), dim);
            }
            x += line + st.ItemInnerSpacing.x * 1.5f;
        }
        bool cut = false;
        const float name_w = draw_strong(draw, ImVec2(x, y1), ImGui::GetColorU32(on ? kText : kTextWeak), mod.name.c_str(), text_right, &cut);
        // The facts: under the name (two lines), after it, or at the right by the toggle; "N changed" in blue last.
        float fx = two_line ? x : x + name_w + st.ItemSpacing.x * 1.5f;
        const float fy = two_line ? y2 : y1;
        if (facts_right && !cut) {
            const std::string all = facts + (changed_text.empty() ? "" : (facts.empty() ? "" : " \xC2\xB7 ") + changed_text);
            fx = std::max(fx, text_right - ImGui::CalcTextSize(all.c_str()).x);
        }
        bool facts_cut = cut && !two_line;
        // Less room than a few letters: no facts on the row (the tooltip has them), never a lone "...".
        if (!facts_cut && text_right - fx > ImGui::GetFontSize() * 4) {
            const float fw = facts.empty() ? 0.0f : draw_text_fit(draw, ImGui::GetFont(), ImVec2(fx, fy), text_right, dim, facts, &facts_cut);
            if (!changed_text.empty() && !facts_cut) {
                draw_text_fit(draw, ImGui::GetFont(), ImVec2(fx + fw, fy), text_right, ImGui::GetColorU32(kInfo),
                              (facts.empty() ? "" : " \xC2\xB7 ") + changed_text, &facts_cut);
            }
        }
        if (chevron_right) {
            const float s = line * 0.22f;
            const ImVec2 c(text_right + st.ItemSpacing.x + line * 0.5f, at.y + h * 0.5f);
            const float tw = std::max(1.5f, line * 0.09f);
            if (open) {
                draw->AddLine(ImVec2(c.x - s, c.y - s * 0.5f), ImVec2(c.x, c.y + s * 0.5f), dim, tw);
                draw->AddLine(ImVec2(c.x, c.y + s * 0.5f), ImVec2(c.x + s, c.y - s * 0.5f), dim, tw);
            } else {
                draw->AddLine(ImVec2(c.x - s * 0.5f, c.y - s), ImVec2(c.x + s * 0.5f, c.y), dim, tw);
                draw->AddLine(ImVec2(c.x + s * 0.5f, c.y), ImVec2(c.x - s * 0.5f, c.y + s), dim, tw);
            }
        }
        // The tooltip: the whole name and facts, what it does, its id.
        if (row_hovered) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26);
            ImGui::TextUnformatted(mod.name.c_str());
            if (!facts.empty() || !changed_text.empty()) {
                weak(facts + (changed_text.empty() ? "" : (facts.empty() ? "" : " \xC2\xB7 ") + changed_text));
            }
            if (!mod.description.empty() && facts != mod.description) ImGui::TextUnformatted(mod.description.c_str());
            weak(key);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        ImGui::SetCursorScreenPos(ImVec2(toggle_x, at.y + (h - frame) * 0.5f));
        if (toggle("##on", on, room ? (on ? tr("On") : tr("Off")) : nullptr, disabled, true)) host.apply(set_switch(p, mod.id, Which::On, !on));
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + h));
        ImGui::Dummy(ImVec2(1, 0));
        if (open) {
            const float indent = (glyph || !chevron_right ? line + st.ItemInnerSpacing.x * 1.5f : 0.0f) + st.FramePadding.x;
            ImGui::Indent(indent);
            body(false);
            ImGui::Unindent(indent);
        }
        if (card) {
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
            ImGui::Dummy(ImVec2(1, 2));
        }
        ImGui::PopID();
        return;
    }
    if (v == 4) {
        // C4: one line per module (name, where it runs in two words, tags, toggle); a click opens
        // what it does, where it runs and its settings. Opened at first: the ones with changes.
        const float right_edge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemOpen(!changed.empty(), ImGuiCond_Once);
        ImGui::PushStyleColor(ImGuiCol_Text, on ? kText : kTextWeak);
        const bool open = ImGui::TreeNodeEx("##row", ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_FramePadding,
                                            "%s", mod.name.c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("%s", (p.id + "." + mod.id).c_str());
        if (mod.changes_game && on) {
            const char* words[] = {tr("Off"), tr("Normal games"), tr("+ Ironman"), tr("+ Multiplayer"), tr("Everywhere")};
            const char* word = words[static_cast<int>(where(mod.saved))];
            const float word_w = std::max(ImGui::CalcTextSize(tr("On")).x, ImGui::CalcTextSize(tr("Off")).x);
            const float limit = std::min(g_toggle_left, right_edge - toggle_track_width(true) - ImGui::GetStyle().ItemSpacing.x * 2 - word_w);
            if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + ImGui::CalcTextSize(word).x < limit) {
                ImGui::SameLine();
                weak(word);
            }
        }
        tags(right_edge);
        right_toggle(right_edge);
        if (open) {
            body(false);
            ImGui::TreePop();
        }
        ImGui::PopID();
        return;
    }
    {
        // C, C1, C2: a card per module.
        const float fs = ImGui::GetFontSize();
        const bool compact = v >= 1;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, compact && !on ? ImVec4(0, 0, 0, 0) : kRaised);
        ImGui::PushStyleColor(ImGuiCol_Border, kLine);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, compact ? ImVec2(fs * 0.6f, fs * 0.3f) : ImVec2(fs * 0.75f, fs * 0.5f));
        if (compact) ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, ImGui::GetStyle().ItemSpacing.y * 0.75f));
        ImGui::BeginChild("card", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        if (v == 2 && on) {
            // C2: the ember edge of a card that is on, like a banner's edge.
            const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(wp.x + 1, wp.y + 5), ImVec2(wp.x + 4, wp.y + ws.y - 5), ImGui::GetColorU32(kEmber), 1.5f);
        }
        const float right_edge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        title(!compact || on ? kText : kTextWeak);
        tags(right_edge);
        right_toggle(right_edge);
        body(v == 1);
        ImGui::EndChild();
        ImGui::PopStyleVar(compact ? 3 : 2);
        ImGui::PopStyleColor(2);
        if (compact) {
            ImGui::Dummy(ImVec2(0, fs * 0.1f));
        } else {
            ImGui::Spacing();
        }
        ImGui::PopID();
        return;
    }
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
    // What follows wraps to its own line when the panel is too narrow for it.
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    if (host.in_game()) {
        // Settings apply at once in the game unless they say otherwise (ui-guidelines §3).
        if (locked || snap.restart) {
            const std::string note = locked ? tr("locked in a multiplayer game") : tr("next start");
            same_line_if_fits(text_width(note));
            weak(note);
        }
        const bool pending = setting_pending(snap);
        if (pending || !snap.overridden.empty()) {
            const std::string now = trf("now: {0}", {host.shown_value(i, s, pending ? snap.in_effect : snap.overridden)});
            same_line_if_fits(text_width(now));
            ImGui::TextColored(kInfo, "%s", now.c_str());
        }
    }
    if (setting_changed(snap)) {
        const std::string changed = trf("changed · default: {0}", {host.shown_value(i, s, snap.default_value)});
        same_line_if_fits(text_width(changed) + gap + button_width(tr("Reset")));
        ImGui::TextColored(kInfo, "%s", changed.c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(disabled || locked);
        if (text_button(tr("Reset"))) host.apply(reset_setting(p, snap.name));
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
    c.title = trn("Turn on {0} plugin|Turn on {0} plugins", static_cast<long long>(edits.size()));
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
