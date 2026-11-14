#pragma once

#include <imgui.h>

#include <filesystem>
#include <string>
#include <vector>

#include "ui/imgui_colors.hpp"

// Small drawing helpers the Manager's tabs share.
namespace anvil::manager::ui {

// The state colours of the shared style (src/ui/imgui_colors.hpp).
inline constexpr ImVec4 kGood = ::anvil::ui::kGood;
inline constexpr ImVec4 kWarn = ::anvil::ui::kWarn;
inline constexpr ImVec4 kBad = ::anvil::ui::kBad;
using ::anvil::ui::primary_button;

// The page titles' font (main.cpp, load_fonts); null when Windows lacks Segoe UI Semibold.
inline ImFont* g_title_font = nullptr;

inline std::string u8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

inline void wrapped(const std::string& text) { ImGui::TextWrapped("%s", text.c_str()); }

inline void colored_wrapped(const ImVec4& color, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    wrapped(text);
    ImGui::PopStyleColor();
}

inline void weak(const std::string& text) { ImGui::TextDisabled("%s", text.c_str()); }

inline void weak_wrapped(const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    wrapped(text);
    ImGui::PopStyleColor();
}

inline void bullet(const std::string& text) {
    ImGui::Bullet();
    ImGui::SameLine();
    wrapped(text);
}

// Modals: centred on every frame, a fixed width and the height of their contents.
inline void next_modal(float width_in_fonts) {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * width_in_fonts, 0), ImGuiCond_Always);
}

inline std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) out += (out.empty() ? "" : ", ") + item;
    return out;
}

}  // namespace anvil::manager::ui
