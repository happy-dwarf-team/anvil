// The components of Forge (docs/ui-design-system.md, "Components") that every
// view of the Manager and the overlay shares beyond the Plugins tab's own
// controls (plugin_widgets.hpp): banners, text buttons, tags, chips, cards,
// facts, state rows, list rows, checkboxes, titles, the monospace text, the
// code view and the dialog's frame. A view draws a state, an action or a
// dialog with these, not with its own colours and spacings.
#pragma once

#include "ui/imgui_colors.hpp"
#include "ui/plugin_widgets.hpp"
#include "ui/text_wrap.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <initializer_list>
#include <string>
#include <string_view>

namespace anvil::ui {

// The titles' font, Segoe UI Semibold with the CJK fonts merged into it; null
// where Windows has none, and the text's font at a larger size stands in.
inline ImFont* g_title_font = nullptr;
// Logs and reports: Consolas with the CJK fonts merged into it; null where the
// app has none, and the text's font stands in.
inline ImFont* g_mono_font = nullptr;

// The four states a banner or a state row says (docs/ui-guidelines.md §3).
enum class Tone { Good, Warn, Bad, Info };

inline ImVec4 tone_color(Tone tone) {
    switch (tone) {
        case Tone::Good: return kGood;
        case Tone::Warn: return kWarn;
        case Tone::Bad: return kBad;
        case Tone::Info: return kInfo;
    }
    return kText;
}

// Each tone's glyph: a shape of its own, so the state reads without its colour.
inline void tone_glyph(ImDrawList* draw, ImVec2 at, float size, Tone tone) {
    switch (tone) {
        case Tone::Good: draw_glyph(draw, at, size, Glyph::On); break;
        case Tone::Warn: draw_glyph(draw, at, size, Glyph::Attention); break;
        case Tone::Bad: draw_glyph(draw, at, size, Glyph::CannotLoad); break;
        case Tone::Info: draw_pending_dot(draw, ImVec2(at.x + size * 0.5f, at.y + size * 0.5f), size * 0.22f); break;
    }
}

inline float button_width(const char* label) {
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;
}

// A view's or a dialog's title: semibold, 1.18 times the text.
inline void title(const char* text) {
    ImFont* font = g_title_font;
    ImGui::PushFont(font, font != nullptr ? font->LegacySize : ImGui::GetFontSize() * 1.18f);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y));
}

// A name in semibold at the text's size: a list entry's or a card's.
inline void strong(const char* text) {
    if (g_title_font != nullptr) ImGui::PushFont(g_title_font, ImGui::GetFontSize());
    ImGui::TextUnformatted(text);
    if (g_title_font != nullptr) ImGui::PopFont();
}

// The same, wrapped: a notice's title.
inline void strong_wrapped(const char* text) {
    if (g_title_font != nullptr) ImGui::PushFont(g_title_font, ImGui::GetFontSize());
    text_wrapped(text);
    if (g_title_font != nullptr) ImGui::PopFont();
}

// Text drawn at `at` without an item, in `font` at the text's size, cut with
// "…" where it would pass `max_x` (a list row's second line); its width, and
// in `cut` whether it was cut.
inline float draw_text_fit(ImDrawList* draw, ImFont* font, ImVec2 at, float max_x, ImU32 color, std::string_view text,
                           bool* cut = nullptr) {
    const float size = ImGui::GetFontSize();
    const auto measure = [&](std::string_view t) { return font->CalcTextSizeA(size, FLT_MAX, 0.0f, t.data(), t.data() + t.size()).x; };
    const float room = max_x - at.x;
    const float full = measure(text);
    if (cut != nullptr) *cut = full > room;
    if (full <= room) {
        draw->AddText(font, size, at, color, text.data(), text.data() + text.size());
        return full;
    }
    constexpr std::string_view kEllipsis = "\xE2\x80\xA6";
    const float ellipsis = measure(kEllipsis);
    size_t end = 0;
    float width = 0;
    for (size_t k = 0; k < text.size();) {
        size_t next = k + 1;
        while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xC0) == 0x80) ++next;
        width += measure(text.substr(k, next - k));
        if (width + ellipsis > room) break;
        end = next;
        k = next;
    }
    while (end > 0 && text[end - 1] == ' ') --end;
    const std::string shown = std::string(text.substr(0, end)) + std::string(kEllipsis);
    draw->AddText(font, size, at, color, shown.c_str());
    return measure(shown);
}

// A name in semibold drawn at `at` without an item, in a list row, cut at `max_x`; its width.
inline float draw_strong(ImDrawList* draw, ImVec2 at, ImU32 color, const char* text, float max_x = FLT_MAX, bool* cut = nullptr) {
    return draw_text_fit(draw, g_title_font != nullptr ? g_title_font : ImGui::GetFont(), at, max_x, color, text, cut);
}

// The game build a player recognises: "eu4-1.37.5-steam-win64" gives "Steam 1.37.5",
// "eu4-1.37.5-gog-win64" "GOG 1.37.5"; another id stays as it is.
inline std::string build_label(std::string_view build) {
    constexpr std::string_view prefix = "eu4-";
    if (!build.starts_with(prefix)) return std::string(build);
    const size_t dash = build.find('-', prefix.size());
    if (dash == std::string_view::npos) return std::string(build);
    const std::string_view version = build.substr(prefix.size(), dash - prefix.size());
    const std::string_view rest = build.substr(dash + 1);
    const std::string_view store = rest.starts_with("steam") ? "Steam" : rest.starts_with("gog") ? "GOG" : std::string_view();
    return store.empty() ? std::string(build) : std::string(store) + " " + std::string(version);
}

// Text in the monospace font between these two: a log or a report.
inline void begin_mono() {
    ImGui::PushFont(g_mono_font, g_mono_font != nullptr ? g_mono_font->LegacySize : ImGui::GetFontSize());
}
inline void end_mono() { ImGui::PopFont(); }

// The code view: a log or a report in the monospace font on `bg0` with a
// `line` hairline. Read-only, but its text can be selected and copied.
inline void code_text(const char* id, const std::string& text, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kBackground);
    ImGui::PushStyleColor(ImGuiCol_Border, kLine);
    begin_mono();
    ImGui::InputTextMultiline(id, const_cast<char*>(text.c_str()), text.size() + 1, size, ImGuiInputTextFlags_ReadOnly);
    end_mono();
    ImGui::PopStyleColor(2);
}

// A checkbox whose outline shows it can be ticked: `control_outline`, 3:1 on
// the panels, where the frames' `border` is decorative.
inline bool checkbox(const char* label, bool* value) {
    ImGui::PushStyleColor(ImGuiCol_Border, kControlOutline);
    const bool changed = ImGui::Checkbox(label, value);
    ImGui::PopStyleColor();
    return changed;
}

// A weak line under a checkbox, aligned with its label: what the choice covers.
inline void checkbox_note(const std::string& text) {
    const float indent = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
    ImGui::Indent(indent);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextWeak);
    text_wrapped(text);
    ImGui::PopStyleColor();
    ImGui::Unindent(indent);
}

// A filter chip: a small pill that toggles, ember while on. True when clicked.
inline bool chip(const char* label, bool on) {
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, with_alpha(kEmber, 0.28f));
        ImGui::PushStyleColor(ImGuiCol_Border, kEmber);
    }
    const bool clicked = ImGui::SmallButton(label);
    if (on) ImGui::PopStyleColor(2);
    return clicked;
}

// A secondary action that is a link rather than a step: "Page", "Copy",
// "Open the folder". Info text, no frame until hovered. At the start of a line
// its text lines up with the text above it; its hover frame reaches into the margin.
inline bool text_button(const char* label) {
    const ImGuiWindow* window = ImGui::GetCurrentWindowRead();
    if (window->DC.CursorPos.x <= window->Pos.x + window->DC.Indent.x + window->DC.ColumnsOffset.x + 0.5f) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() - ImGui::GetStyle().FramePadding.x);
    }
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kFrameHovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kFrameActive);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, kInfo);
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleColor(5);
    return pressed;
}

// A pill with a hairline: a fact about an item ("changes the game"), in the
// tone's colour when it warns, else weak.
inline void tag(const char* text, bool warn = false) {
    const ImVec2 size = ImGui::CalcTextSize(text);
    const float pad_x = std::round(ImGui::GetFontSize() * 0.4f), pad_y = std::round(ImGui::GetFontSize() * 0.08f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 box(size.x + pad_x * 2, size.y + pad_y * 2);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec4 color = warn ? kWarn : kTextWeak;
    draw->AddRect(at, ImVec2(at.x + box.x, at.y + box.y), ImGui::GetColorU32(warn ? with_alpha(kWarn, 0.6f) : kBorder), box.y * 0.5f);
    draw->AddText(ImVec2(at.x + pad_x, at.y + pad_y), ImGui::GetColorU32(color), text);
    ImGui::Dummy(box);
}

// A glyph and a line of text beside it, wrapped under the text and not under the glyph.
inline void state_row(Glyph glyph, const std::string& text, const ImVec4& color = kText) {
    const float line = ImGui::GetTextLineHeight();
    const float indent = line + ImGui::GetStyle().ItemInnerSpacing.x * 2;
    draw_glyph(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), line, glyph);
    ImGui::Indent(indent);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    text_wrapped(text);
    ImGui::PopStyleColor();
    ImGui::Unindent(indent);
}

// A weak line under a state row, aligned with its text: what to do about it.
inline void state_note(const std::string& text) {
    const float indent = ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemInnerSpacing.x * 2;
    ImGui::Indent(indent);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextWeak);
    text_wrapped(text);
    ImGui::PopStyleColor();
    ImGui::Unindent(indent);
}

// A banner: `raised`, a 3 px bar in the state's colour at its left edge and the
// state's glyph before its first line; the text stays `text`, so a long
// explanation reads at length. Its contents (text, a list, its actions) go
// between begin_banner and end_banner, indented past the glyph.
inline void begin_banner(const char* id, Tone tone) {
    const float font = ImGui::GetFontSize();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kRaised);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f * font / 17.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(std::round(font * 0.75f), std::round(font * 0.5f)));
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::PushID(static_cast<int>(tone));
    const float line = ImGui::GetTextLineHeight();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    tone_glyph(ImGui::GetWindowDrawList(), ImVec2(at.x, at.y + (ImGui::GetFrameHeight() - line) * 0.5f), line, tone);
    ImGui::Indent(line + ImGui::GetStyle().ItemInnerSpacing.x * 2);
    ImGui::AlignTextToFramePadding();
}

inline void end_banner(Tone tone) {
    ImGui::Unindent(ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemInnerSpacing.x * 2);
    ImGui::PopID();
    ImGui::EndChild();
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const float bar = std::round(3.0f * ImGui::GetFontSize() / 17.0f);
    ImGui::GetWindowDrawList()->AddRectFilled(min, ImVec2(min.x + bar, max.y), ImGui::GetColorU32(tone_color(tone)),
                                              4.0f * ImGui::GetFontSize() / 17.0f, ImDrawFlags_RoundLeft);
}

// A banner of one wrapped text.
inline void banner(const char* id, Tone tone, const std::string& text) {
    begin_banner(id, tone);
    text_wrapped(text);
    end_banner(tone);
}

// A card: a `panel` box with a hairline, as high as its contents.
inline void begin_card(const char* id) {
    const float font = ImGui::GetFontSize();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
    ImGui::PushStyleColor(ImGuiCol_Border, kLine);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(std::round(font * 0.75f), std::round(font * 0.6f)));
    ImGui::BeginChild(id, ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

inline void end_card() { ImGui::EndChild(); }

// A list row (docs/ui-design-system.md, "List row"): a selectable `lines`
// lines high, hover `raised`, the selection ember at 16 % with a 3 px ember
// bar at its left edge, as the sidebar draws its page. The caller draws the
// glyph, the name (draw_strong) and the lines under it from row_text_x.
inline bool list_row(const char* id, bool selected, int lines) {
    const float pad = ImGui::GetStyle().FramePadding.y;
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, selected ? ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered) : kRaised);
    const bool clicked = ImGui::Selectable(id, selected, ImGuiSelectableFlags_None,
                                           ImVec2(0, ImGui::GetTextLineHeight() * static_cast<float>(lines) + pad * 2));
    ImGui::PopStyleColor();
    if (selected) {
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        const float bar = std::round(3.0f * ImGui::GetFontSize() / 17.0f);
        ImGui::GetWindowDrawList()->AddRectFilled(min, ImVec2(min.x + bar, max.y), ImGui::GetColorU32(kEmber));
    }
    return clicked;
}

// Where a row's glyph and text start: the glyph past the bar, the text past the glyph.
inline float row_glyph_x(ImVec2 row) { return row.x + std::round(6.0f * ImGui::GetFontSize() / 17.0f); }
inline float row_text_x(ImVec2 row) {
    return row_glyph_x(row) + ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemInnerSpacing.x * 2;
}

// Facts as names and values: the name weak in its own column.
inline bool begin_facts(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_NoSavedSettings)) return false;
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

inline void fact(const char* name, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextColored(kTextWeak, "%s", name);
    ImGui::TableNextColumn();
    text_wrapped(value);
}

inline void end_facts() { ImGui::EndTable(); }

// Dialogs (docs/ui-design-system.md, "Dialog"): no title bar, the title in
// semibold inside, and the buttons in a row at the right in Windows' order,
// the primary first and Cancel last.
inline bool begin_dialog(const char* id, const std::string& heading) {
    if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar)) return false;
    title(heading.c_str());
    return true;
}

// Moves the cursor so that buttons with these labels, on one line, end at the right edge.
inline void dialog_buttons(std::initializer_list<const char*> labels) {
    ImGui::Spacing();
    float width = 0;
    for (const char* label : labels) width += button_width(label) + ImGui::GetStyle().ItemSpacing.x;
    width -= ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - width));
}

}  // namespace anvil::ui
