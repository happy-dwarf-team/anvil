// The components of Forge (docs/ui-design-system.md, "Components") that every
// view of the Manager and the overlay shares beyond the Plugins tab's own
// controls (plugin_widgets.hpp): banners, text buttons, tags, cards, facts,
// state rows, titles and the dialog's frame. A view draws a state, an action or
// a dialog with these, not with its own colours and spacings.
#pragma once

#include "ui/imgui_colors.hpp"
#include "ui/plugin_widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>

namespace anvil::ui {

// The titles' font, Segoe UI Semibold with the CJK fonts merged into it; null
// where the app has none (the overlay), and the text's font at a larger size stands in.
inline ImFont* g_title_font = nullptr;

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

// A secondary action that is a link rather than a step: "Page", "Copy",
// "Open the folder". Info text, no frame until hovered.
inline bool text_button(const char* label) {
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
    ImGui::TextWrapped("%s", text.c_str());
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
    ImGui::TextWrapped("%s", text.c_str());
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
    ImGui::TextDisabled("%s", name);
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", value.c_str());
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
