// The controls of the Plugins tab that ImGui does not have, drawn the same way
// in the Manager and the overlay (docs/ui-design-system.md, "Components"): the
// status glyphs, the plugin's toggle and the "where it runs" segmented control.
// The glyphs are drawn, not typed, so no font needs them.
#pragma once

#include "ui/imgui_colors.hpp"
#include "ui/plugin_model.hpp"

#include <imgui.h>

#include <algorithm>
#include <string>

namespace anvil::ui {

// The colour of a state (docs/ui-guidelines.md §3).
inline ImVec4 glyph_color(Glyph glyph) {
    switch (glyph) {
        case Glyph::On: return kGood;
        case Glyph::Off: return kTextDim;
        case Glyph::Attention: return kWarn;
        case Glyph::CannotLoad: return kBad;
    }
    return kText;
}

// Draws `glyph` in a square of side `size` with its top-left corner at `at`:
// ✓ on, ○ off, ▲ needs attention, ✕ cannot load. Each has its own shape.
inline void draw_glyph(ImDrawList* draw, ImVec2 at, float size, Glyph glyph) {
    const ImU32 color = ImGui::GetColorU32(glyph_color(glyph));
    const float s = size;
    const float t = std::max(1.5f, s * 0.12f);
    const ImVec2 c(at.x + s * 0.5f, at.y + s * 0.5f);
    switch (glyph) {
        case Glyph::On: {
            const ImVec2 points[] = {ImVec2(at.x + s * 0.18f, at.y + s * 0.52f), ImVec2(at.x + s * 0.42f, at.y + s * 0.76f),
                                     ImVec2(at.x + s * 0.84f, at.y + s * 0.26f)};
            draw->AddPolyline(points, 3, color, t, ImDrawFlags_None);
            break;
        }
        case Glyph::Off: draw->AddCircle(c, s * 0.32f, color, 0, t); break;
        case Glyph::Attention:
            draw->AddTriangleFilled(ImVec2(c.x, at.y + s * 0.14f), ImVec2(at.x + s * 0.88f, at.y + s * 0.84f),
                                    ImVec2(at.x + s * 0.12f, at.y + s * 0.84f), color);
            break;
        case Glyph::CannotLoad:
            draw->AddLine(ImVec2(at.x + s * 0.22f, at.y + s * 0.22f), ImVec2(at.x + s * 0.78f, at.y + s * 0.78f), color, t);
            draw->AddLine(ImVec2(at.x + s * 0.78f, at.y + s * 0.22f), ImVec2(at.x + s * 0.22f, at.y + s * 0.78f), color, t);
            break;
    }
}

// A glyph as an item on the current line, the height of the text.
inline void glyph_item(Glyph glyph) {
    const float size = ImGui::GetTextLineHeight();
    draw_glyph(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), size, glyph);
    ImGui::Dummy(ImVec2(size, size));
}

// The dot of a change that waits for the next start (info colour).
inline void draw_pending_dot(ImDrawList* draw, ImVec2 center, float radius) {
    draw->AddCircleFilled(center, radius, ImGui::GetColorU32(kInfo));
}

// The plugin's own switch: a toggle, bigger than a checkbox and drawn apart
// from the modules' ones, with its state as a word beside it. True when clicked.
inline bool plugin_toggle(const char* id, bool on, const char* word, bool disabled) {
    const float h = ImGui::GetFrameHeight();
    const float w = h * 1.9f;
    ImGui::BeginDisabled(disabled);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float alpha = disabled ? ImGui::GetStyle().DisabledAlpha : 1.0f;
    const ImVec4 track = on ? (hovered ? kEmberHovered : kEmber) : (hovered ? kFrameActive : kFrameHovered);
    draw->AddRectFilled(at, ImVec2(at.x + w, at.y + h), ImGui::GetColorU32(with_alpha(track, alpha)), h * 0.5f);
    draw->AddRect(at, ImVec2(at.x + w, at.y + h), ImGui::GetColorU32(with_alpha(on ? kEmber : kBorder, alpha)), h * 0.5f);
    const float r = h * 0.5f - 3.0f;
    const ImVec2 knob(on ? at.x + w - h * 0.5f : at.x + h * 0.5f, at.y + h * 0.5f);
    draw->AddCircleFilled(knob, r, ImGui::GetColorU32(with_alpha(on ? kOnEmber : kText, alpha)));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(word);
    ImGui::EndDisabled();
    return clicked && !disabled;
}

// A segmented control: one button per choice, the current one in the accent.
// `enabled[i]` false greys a choice; `tips[i]` (may be null) is its tooltip,
// shown also while it is disabled. Returns the choice clicked, or -1.
inline int segmented(const char* id, const char* const* labels, const bool* enabled, const char* const* tips, int count,
                     int current, bool disabled) {
    int chosen = -1;
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, ImGui::GetStyle().ItemSpacing.y));
    for (int i = 0; i < count; ++i) {
        if (i > 0) ImGui::SameLine();
        ImGui::PushID(i);
        const bool selected = i == current;
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, with_alpha(kEmber, 0.28f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, with_alpha(kEmber, 0.36f));
            ImGui::PushStyleColor(ImGuiCol_Border, kEmber);
        }
        ImGui::BeginDisabled(disabled || !enabled[i]);
        if (ImGui::Button(labels[i]) && !selected) chosen = i;
        ImGui::EndDisabled();
        if (tips != nullptr && tips[i] != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", tips[i]);
        }
        if (selected) ImGui::PopStyleColor(3);
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    return chosen;
}

}  // namespace anvil::ui
