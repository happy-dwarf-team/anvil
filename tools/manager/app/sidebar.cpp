#include "sidebar.hpp"

#include "i18n/translate.hpp"
#include "ui/imgui_colors.hpp"
#include "ui/plugin_widgets.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace anvil::manager::ui {
namespace {

using namespace ::anvil::ui;

// The icons are drawn on a grid of 32 by 32, as in the prototype's SVG
// (tools/manager/prototype/app-shell.html on the branch prototype/app-shell).
struct Grid {
    ImVec2 at;
    float unit;
    ImVec2 operator()(float x, float y) const { return ImVec2(at.x + x * unit, at.y + y * unit); }
};

}  // namespace

void draw_nav_icon(ImDrawList* draw, ImVec2 at, float size, NavIcon icon, ImU32 color) {
    const Grid p{at, size / 32.0f};
    const float t = std::max(1.25f, size * 0.085f);
    const float dot = std::max(1.0f, size * 0.06f);
    switch (icon) {
        case NavIcon::Anvil: {
            // The anvil mark of the window's icon: horn, face, waist and foot.
            const ImVec2 points[] = {p(2, 10.5f), p(9, 8),     p(28, 8),    p(28, 13),   p(22, 15),
                                     p(22, 19),   p(25.5f, 23), p(25.5f, 26), p(8.5f, 26), p(8.5f, 23),
                                     p(12, 19),   p(12, 15),   p(7.5f, 14.3f), p(4.5f, 12.8f)};
            draw->AddConcavePolyFilled(points, static_cast<int>(std::size(points)), color);
            break;
        }
        case NavIcon::Plugins:
            for (const ImVec2 cell : {ImVec2(6, 6), ImVec2(18, 6), ImVec2(6, 18), ImVec2(18, 18)}) {
                draw->AddRect(p(cell.x, cell.y), p(cell.x + 8, cell.y + 8), color, 1.0f, t);
            }
            break;
        case NavIcon::Catalog: {
            const ImVec2 box[] = {p(6, 10), p(16, 5), p(26, 10), p(26, 22), p(16, 27), p(6, 22)};
            draw->AddPolyline(box, 6, color, t, ImDrawFlags_Closed);
            const ImVec2 lid[] = {p(6, 10), p(16, 15), p(26, 10)};
            draw->AddPolyline(lid, 3, color, t);
            draw->AddLine(p(16, 15), p(16, 27), color, t);
            break;
        }
        case NavIcon::Benchmark:
            draw->PathArcTo(p(16, 22), 11 * p.unit, 3.14159265f, 2 * 3.14159265f);
            draw->PathStroke(color, t);
            draw->AddLine(p(16, 22), p(22, 15), color, t);
            draw->AddCircleFilled(p(16, 22), dot * 1.3f, color);
            break;
        case NavIcon::Crashes: {
            const ImVec2 sign[] = {p(16, 5), p(28, 26), p(4, 26)};
            draw->AddPolyline(sign, 3, color, t, ImDrawFlags_Closed);
            draw->AddLine(p(16, 12.5f), p(16, 19), color, t);
            draw->AddCircleFilled(p(16, 22.5f), dot, color);
            break;
        }
        case NavIcon::Log:
            draw->AddLine(p(7, 8), p(25, 8), color, t);
            draw->AddLine(p(7, 13), p(25, 13), color, t);
            draw->AddLine(p(7, 18), p(19, 18), color, t);
            draw->AddLine(p(7, 23), p(22, 23), color, t);
            break;
        case NavIcon::About:
            draw->AddCircle(p(16, 16), 11 * p.unit, color, 0, t);
            draw->AddLine(p(16, 15), p(16, 22), color, t);
            draw->AddCircleFilled(p(16, 10.5f), dot, color);
            break;
    }
}

void draw_play_glyph(ImDrawList* draw, ImVec2 center, float size, ImU32 color) {
    const float h = size * 0.5f;
    draw->AddTriangleFilled(ImVec2(center.x - h * 0.75f, center.y - h), ImVec2(center.x + h, center.y),
                            ImVec2(center.x - h * 0.75f, center.y + h), color);
}

float nav_item_height() { return std::round(ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.y * 1.5f); }

bool nav_item(const NavEntry& entry, bool selected, bool rail) {
    const float px = ImGui::GetFontSize() / 17.0f;  // the Manager's scale: its text is 17 px at 100 %
    const float h = nav_item_height();
    ImGui::PushID(entry.id);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const bool clicked = ImGui::Selectable("##nav", selected, ImGuiSelectableFlags_None, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const char* label = i18n::tr(entry.id);
    if (selected) draw->AddRectFilled(at, ImVec2(at.x + std::round(3 * px), at.y + h), ImGui::GetColorU32(kEmber));
    const ImU32 icon_color = ImGui::GetColorU32(selected || hovered ? kText : kTextWeak);
    const ImVec4 count_color = entry.warn ? kWarn : kInfo;
    if (rail) {
        const float size = std::round(18 * px);
        draw_nav_icon(draw, ImVec2(at.x + std::round((w - size) * 0.5f), at.y + std::round((h - size) * 0.5f)), size, entry.icon,
                      icon_color);
        if (entry.count > 0) {
            // The count's glyph in the icon's corner; the number is in the tooltip.
            const float g = std::round(9 * px);
            const ImVec2 corner(at.x + w - std::round(10 * px) - g, at.y + std::round(4 * px));
            if (entry.warn) {
                draw_glyph(draw, corner, g, Glyph::Attention);
            } else {
                draw_pending_dot(draw, ImVec2(corner.x + g * 0.5f, corner.y + g * 0.5f), g * 0.36f);
            }
        }
        if (hovered) {
            ImGui::SetTooltip("%s", entry.tip.empty() ? label : entry.tip.c_str());
        }
    } else {
        const float pad = std::round(12 * px);
        const float size = std::round(16 * px);
        draw_nav_icon(draw, ImVec2(at.x + pad, at.y + std::round((h - size) * 0.5f)), size, entry.icon, icon_color);
        float right = at.x + w - pad;
        if (entry.count > 0) {
            // A pill on the right: the state's glyph and the number, in the state's colour.
            const std::string number = std::to_string(entry.count);
            const float tw = ImGui::CalcTextSize(number.c_str()).x;
            const float g = std::round(ImGui::GetTextLineHeight() * 0.62f);
            const float gap = std::round(4 * px);
            const float y = at.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
            draw->AddText(ImVec2(right - tw, y), ImGui::GetColorU32(count_color), number.c_str());
            const ImVec2 glyph_at(right - tw - gap - g, at.y + std::round((h - g) * 0.5f));
            if (entry.warn) {
                draw_glyph(draw, glyph_at, g, Glyph::Attention);
            } else {
                draw_pending_dot(draw, ImVec2(glyph_at.x + g * 0.5f, glyph_at.y + g * 0.5f), g * 0.36f);
            }
            right = glyph_at.x - gap;
        }
        const float x = at.x + pad + size + std::round(10 * px);
        const ImVec4 clip(x, at.y, right, at.y + h);
        draw->AddText(nullptr, 0.0f, ImVec2(x, at.y + (h - ImGui::GetTextLineHeight()) * 0.5f),
                      ImGui::GetColorU32(kText), label, nullptr, 0.0f, &clip);
        if (hovered && !entry.tip.empty()) ImGui::SetTooltip("%s", entry.tip.c_str());
    }
    ImGui::PopID();
    return clicked;
}

}  // namespace anvil::manager::ui
