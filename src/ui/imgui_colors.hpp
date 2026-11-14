// The look of the overlay and the Manager, "Forge" (docs/ui-design-system.md):
// warm graphite surfaces, one ember accent for what the player acts on (the
// keyboard cursor, the current tab, a checked box, the main action of a view),
// and the state colours, which keep their meanings (docs/ui-guidelines.md §3).
// Both apps call use_anvil_style() once and scale it with ScaleAllSizes; neither
// sets its own colours or sizes.
#pragma once

#include <imgui.h>

namespace anvil::ui {

constexpr ImVec4 rgb(int hex, float alpha = 1.0f) {
    return ImVec4(static_cast<float>((hex >> 16) & 0xff) / 255.0f, static_cast<float>((hex >> 8) & 0xff) / 255.0f,
                  static_cast<float>(hex & 0xff) / 255.0f, alpha);
}

// Surfaces, darkest first.
inline constexpr ImVec4 kBackground = rgb(0x0d0c0b);  // behind the window: the Manager's clear colour
inline constexpr ImVec4 kWindow = rgb(0x121110);
inline constexpr ImVec4 kPanel = rgb(0x181716);  // child windows
inline constexpr ImVec4 kRaised = rgb(0x1f1d1b);  // popups, the current tab, table headers
inline constexpr ImVec4 kFrame = rgb(0x242220);
inline constexpr ImVec4 kFrameHovered = rgb(0x2e2b28);
inline constexpr ImVec4 kFrameActive = rgb(0x38342f);
inline constexpr ImVec4 kLine = rgb(0x2c2926);  // separators, light table borders
inline constexpr ImVec4 kBorder = rgb(0x4a4540);  // outlines of windows and frames
inline constexpr ImVec4 kBorderHovered = rgb(0x5c564f);
// Text: contrast on kPanel 14.7, 6.7 and 4.9 to 1.
inline constexpr ImVec4 kText = rgb(0xece8e1);
inline constexpr ImVec4 kTextWeak = rgb(0xa39e96);
inline constexpr ImVec4 kTextDim = rgb(0x8a857e);  // TextDisabled
// The accent, and the text on it (white on ember would reach only 2.7 to 1).
inline constexpr ImVec4 kEmber = rgb(0xe8853d);
inline constexpr ImVec4 kEmberHovered = rgb(0xf09a57);
inline constexpr ImVec4 kEmberActive = rgb(0xd0702a);
inline constexpr ImVec4 kOnEmber = rgb(0x1a1209);
// States: good (loads, allowed), warning (needs a look), bad (failed), info (waits for the next start).
inline constexpr ImVec4 kGood = rgb(0x6cc48a);
inline constexpr ImVec4 kWarn = rgb(0xe5c04b);
inline constexpr ImVec4 kBad = rgb(0xef6a5e);
inline constexpr ImVec4 kInfo = rgb(0x7aa7e0);

inline ImVec4 with_alpha(ImVec4 color, float alpha) {
    color.w = alpha;
    return color;
}

// Sets the whole style, colours and sizes, at a scale of 1.
inline void use_anvil_style() {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    style.WindowPadding = ImVec2(12, 12);
    style.FramePadding = ImVec2(8, 4);
    style.ItemSpacing = ImVec2(8, 6);
    style.WindowRounding = 6;
    style.ChildRounding = 6;
    style.PopupRounding = 8;
    style.FrameRounding = 4;
    style.GrabRounding = 4;
    style.TabRounding = 4;
    style.WindowBorderSize = 1;
    style.FrameBorderSize = 1;
    style.ScrollbarSize = 12;
    style.ScrollbarRounding = 12;
    style.TabBarOverlineSize = 2;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kTextDim;
    c[ImGuiCol_WindowBg] = with_alpha(kWindow, 0.97f);
    c[ImGuiCol_ChildBg] = kPanel;
    c[ImGuiCol_PopupBg] = with_alpha(kRaised, 0.98f);
    c[ImGuiCol_Border] = kBorder;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = kFrame;
    c[ImGuiCol_FrameBgHovered] = kFrameHovered;
    c[ImGuiCol_FrameBgActive] = kFrameActive;
    c[ImGuiCol_TitleBg] = kBackground;
    c[ImGuiCol_TitleBgActive] = kRaised;
    c[ImGuiCol_TitleBgCollapsed] = with_alpha(kBackground, 0.85f);
    c[ImGuiCol_MenuBarBg] = kRaised;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = rgb(0x3d3935);
    c[ImGuiCol_ScrollbarGrabHovered] = kBorderHovered;
    c[ImGuiCol_ScrollbarGrabActive] = kEmber;
    // A checked box: an ember tick on an ember-tinted box. The radio button's dot
    // is drawn in CheckMark on FrameBg, so CheckMark stays light.
    c[ImGuiCol_CheckMark] = kEmber;
    c[ImGuiCol_CheckboxSelectedBg] = rgb(0x5b3e28);  // kEmber at 28 % over kFrame
    c[ImGuiCol_SliderGrab] = kEmber;
    c[ImGuiCol_SliderGrabActive] = kEmberHovered;
    // Buttons are secondary by default; primary_button() draws the one main action.
    c[ImGuiCol_Button] = rgb(0x2a2724);
    c[ImGuiCol_ButtonHovered] = rgb(0x36322e);
    c[ImGuiCol_ButtonActive] = rgb(0x423d37);
    // Selectables, tree nodes, collapsing headers: the ember tint of a selection.
    c[ImGuiCol_Header] = with_alpha(kEmber, 0.16f);
    c[ImGuiCol_HeaderHovered] = with_alpha(kEmber, 0.22f);
    c[ImGuiCol_HeaderActive] = with_alpha(kEmber, 0.28f);
    c[ImGuiCol_Separator] = kLine;
    c[ImGuiCol_SeparatorHovered] = with_alpha(kEmber, 0.60f);
    c[ImGuiCol_SeparatorActive] = kEmber;
    c[ImGuiCol_ResizeGrip] = with_alpha(kBorder, 0.40f);
    c[ImGuiCol_ResizeGripHovered] = with_alpha(kEmber, 0.60f);
    c[ImGuiCol_ResizeGripActive] = kEmber;
    c[ImGuiCol_InputTextCursor] = kEmber;
    // Tabs: flat text; the current one raised, with an ember overline.
    c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabHovered] = kFrameHovered;
    c[ImGuiCol_TabSelected] = kRaised;
    c[ImGuiCol_TabSelectedOverline] = kEmber;
    c[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabDimmedSelected] = kRaised;
    c[ImGuiCol_TabDimmedSelectedOverline] = kEmber;
    c[ImGuiCol_PlotLines] = kInfo;
    c[ImGuiCol_PlotLinesHovered] = kEmberHovered;
    c[ImGuiCol_PlotHistogram] = kEmber;  // progress bars
    c[ImGuiCol_PlotHistogramHovered] = kEmberHovered;
    c[ImGuiCol_TableHeaderBg] = kRaised;
    c[ImGuiCol_TableBorderStrong] = kBorder;
    c[ImGuiCol_TableBorderLight] = kLine;
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
    c[ImGuiCol_TextLink] = kInfo;
    c[ImGuiCol_TextSelectedBg] = with_alpha(kEmber, 0.35f);
    c[ImGuiCol_TreeLines] = kBorder;
    c[ImGuiCol_DragDropTarget] = kEmber;
    c[ImGuiCol_DragDropTargetBg] = with_alpha(kEmber, 0.16f);
    c[ImGuiCol_UnsavedMarker] = kText;
    c[ImGuiCol_NavCursor] = kEmber;
    c[ImGuiCol_NavWindowingHighlight] = with_alpha(kEmber, 0.70f);
    c[ImGuiCol_NavWindowingDimBg] = with_alpha(kBackground, 0.60f);
    c[ImGuiCol_ModalWindowDimBg] = with_alpha(kBackground, 0.66f);
}

// The one main action of a view or a dialog ("Approve", "Install", "Restart now"):
// ember with dark text. Everything else is an ordinary, secondary button.
inline bool primary_button(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, kEmber);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kEmberHovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kEmberActive);
    ImGui::PushStyleColor(ImGuiCol_Border, kEmber);
    ImGui::PushStyleColor(ImGuiCol_Text, kOnEmber);
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(5);
    return pressed;
}

}  // namespace anvil::ui
