// The shared widgets of the Plugins tab (src/ui/plugin_widgets.hpp), drawn in
// a Dear ImGui context without a window or renderer: the mouse is posted and
// the frame's windows are read back.
#include "ui/plugin_widgets.hpp"

#include "test_support.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>
#include <cstring>
#include <functional>

namespace {

// One frame with the mouse at `mouse`, `body` drawn in a fixed window.
void frame(ImVec2 mouse, const std::function<void()>& body) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.AddMousePosEvent(mouse.x, mouse.y);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(400, 200));
    ImGui::Begin("widgets", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    body();
    ImGui::End();
    ImGui::Render();
}

// The tooltip of the last frame, or "" without one.
std::string shown_tooltip() {
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows) {
        if (w->Active && std::strncmp(w->Name, "##Tooltip_", 10) == 0 && w->DrawList->VtxBuffer.Size > 0) return w->Name;
    }
    return {};
}

}  // namespace

int main() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800, 600);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;  // no renderer: the atlas is never uploaded
    io.Fonts->AddFontDefault();

    run("toggle: the refusal shows over the track and the word, also disabled", [] {
        const char* tip = "The overlay needs it, so it stays on here.";
        ImVec2 cell, word_min, word_max;
        float frame_h = 0;
        bool with_tip = true;
        const auto body = [&] {
            ImGui::SetCursorScreenPos(ImVec2(20, 20));
            cell = ImGui::GetCursorScreenPos();
            frame_h = ImGui::GetFrameHeight();
            anvil::ui::toggle("##on", true, "On", true, false, with_tip ? tip : nullptr);
            word_min = ImGui::GetItemRectMin();
            word_max = ImGui::GetItemRectMax();
        };
        // Two frames at each point: the first moves the mouse, the second sees the window under it.
        const auto at = [&](ImVec2 mouse) {
            frame(mouse, body);
            frame(mouse, body);
            return !shown_tooltip().empty();
        };
        CHECK(!at(ImVec2(300, 150)));
        const float track_w = frame_h * 1.9f;
        CHECK(word_min.x > cell.x + track_w);  // the word is the last item, right of the track
        CHECK(at(ImVec2(cell.x + track_w * 0.5f, cell.y + frame_h * 0.5f)));          // the track
        CHECK(at(ImVec2((word_min.x + word_max.x) * 0.5f, cell.y + frame_h * 0.5f)));  // the word
        CHECK(!at(ImVec2(word_max.x + 30, cell.y + frame_h * 0.5f)));                  // right of the word
        CHECK(!at(ImVec2(cell.x + track_w * 0.5f, cell.y + frame_h + 20)));            // under the track
        with_tip = false;
        CHECK(!at(ImVec2(cell.x + track_w * 0.5f, cell.y + frame_h * 0.5f)));
    });

    run("toggle: without a word the tip shows over the track", [] {
        ImVec2 cell;
        float frame_h = 0;
        const auto body = [&] {
            ImGui::SetCursorScreenPos(ImVec2(20, 60));
            cell = ImGui::GetCursorScreenPos();
            frame_h = ImGui::GetFrameHeight();
            anvil::ui::toggle("##bool", false, nullptr, true, true, "why");
        };
        frame(ImVec2(300, 150), body);
        const ImVec2 track(cell.x + std::round(ImGui::GetFontSize()) * 0.95f, cell.y + frame_h * 0.5f);
        frame(track, body);
        frame(track, body);
        CHECK(!shown_tooltip().empty());
    });

    ImGui::DestroyContext();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
