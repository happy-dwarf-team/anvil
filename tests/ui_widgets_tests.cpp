// The shared widgets of the Plugins tab (src/ui/plugin_widgets.hpp,
// src/ui/forge_widgets.hpp) and the tab itself (src/ui/plugins_view.cpp),
// drawn in a Dear ImGui context without a window or renderer: the mouse is
// posted and the frame's windows are read back. This test's Dear ImGui is
// built with the test engine's hooks (CMakeLists.txt), which report every
// item's rectangle and label as it is drawn.
#include "i18n/translate.hpp"
#include "ui/forge_widgets.hpp"
#include "ui/plugin_widgets.hpp"
#include "ui/plugins_view.hpp"

#include "test_support.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

// The items of the frame being drawn: their rectangle by id, and the label of
// those that name one (buttons, invisible buttons).
namespace {
std::map<ImGuiID, ImRect> g_item_rects;
std::map<ImGuiID, std::string> g_item_labels;
}  // namespace

void ImGuiTestEngineHook_ItemAdd(ImGuiContext*, ImGuiID id, const ImRect& bb, const ImGuiLastItemData*) {
    if (id != 0) g_item_rects[id] = bb;
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags) {
    if (id != 0 && label != nullptr) g_item_labels[id] = label;
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }

namespace {

// One frame with the mouse at `mouse`, `body` drawn in a fixed window of `size`.
void frame(ImVec2 mouse, const std::function<void()>& body, ImVec2 size = ImVec2(400, 200)) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.AddMousePosEvent(mouse.x, mouse.y);
    g_item_rects.clear();
    g_item_labels.clear();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
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

// The rectangles of the last frame's items labelled `label`.
std::vector<ImRect> items(const char* label) {
    std::vector<ImRect> out;
    for (const auto& [id, name] : g_item_labels) {
        if (name == label && g_item_rects.count(id) != 0) out.push_back(g_item_rects[id]);
    }
    return out;
}

bool overlap(const ImRect& a, const ImRect& b) {
    return a.Min.x < b.Max.x && b.Min.x < a.Max.x && a.Min.y < b.Max.y && b.Min.y < a.Max.y;
}

// A plugin of the player's with a changed setting, so its header has "Reset to
// defaults...", and the Manager's "Remove..." from the host.
struct Host final : anvil::ui::PluginsHost {
    std::vector<std::vector<anvil::ui::Edit>> applied;
    bool read_only() const override { return false; }
    void apply(const std::vector<anvil::ui::Edit>& edits, anvil::ui::OnRefusal) override { applied.push_back(edits); }
    bool known(std::size_t) const override { return true; }
    void draw_setting_control(std::size_t, std::size_t, bool) override { ImGui::TextUnformatted("control"); }
    std::string shown_value(std::size_t, std::size_t, const std::string& value) const override { return value; }
    bool has_header_buttons(std::size_t) const override { return true; }
    float header_buttons_width(std::size_t) const override { return anvil::ui::button_width("Remove..."); }
    void draw_header_buttons(std::size_t) override { ImGui::Button("Remove..."); }
};

anvil::ui::Snapshot header_snapshot() {
    anvil::ui::PluginSnap p;
    p.id = "tuning";
    p.name = "Tuning";
    p.version = "2.1.0";
    p.next.loads = true;
    anvil::ui::SettingSnap s;
    s.name = "budget";
    s.label = "Budget";
    s.default_value = "1";
    s.saved = "2";
    s.in_effect = "2";
    p.settings.push_back(s);
    anvil::ui::Snapshot snapshot;
    snapshot.plugins.push_back(p);
    return snapshot;
}

}  // namespace

int main() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800, 600);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;  // no renderer: the atlas is never uploaded
    io.Fonts->AddFontDefault();
    ImGui::GetCurrentContext()->TestEngineHookItems = true;

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

    run("strong_wrapped: a notice's title wraps short of its close button", [] {
        // Latin, German and Japanese titles: each line ends before the "x" at 150.
        const char* titles[] = {
            "A plugin is kept out of this ironman game because you did not allow it there",
            "Ein Plugin bleibt aus diesem Ironman-Spiel heraus, weil du es dort nicht erlaubt hast",
            "\xE3\x81\x93\xE3\x81\xAE\xE3\x83\x97\xE3\x83\xA9\xE3\x82\xB0\xE3\x82\xA4\xE3\x83\xB3\xE3\x81\xAF"
            "\xE3\x81\x93\xE3\x81\xAE\xE3\x82\xA2\xE3\x82\xA4\xE3\x82\xA2\xE3\x83\xB3\xE3\x83\x9E\xE3\x83\xB3"
            "\xE3\x81\xAE\xE3\x82\xB2\xE3\x83\xBC\xE3\x83\xA0\xE3\x81\x8B\xE3\x82\x89\xE5\xA4\x96\xE3\x81\x95"
            "\xE3\x82\x8C\xE3\x81\xA6\xE3\x81\x84\xE3\x81\xBE\xE3\x81\x99"};
        for (const char* text : titles) {
            ImVec2 min, max;
            frame(ImVec2(-100, -100), [&] {
                ImGui::SetCursorPosX(20);
                anvil::ui::strong_wrapped(text, 150);
                min = ImGui::GetItemRectMin();
                max = ImGui::GetItemRectMax();
            });
            CHECK(max.x <= 150.5f);
            CHECK(max.y - min.y > ImGui::GetTextLineHeight() * 1.5f);  // it did wrap
        }
    });

    run("plugin header: the switch never lies over Reset or Remove, at any pane width", [] {
        Host host;
        const anvil::ui::Snapshot snapshot = header_snapshot();
        for (const anvil::i18n::Language language : {anvil::i18n::Language::English, anvil::i18n::Language::German}) {
            anvil::i18n::set_language(language);
            const char* reset_label = anvil::i18n::tr("Reset to defaults...");
            for (float width = 260; width <= 780; width += 20) {
                anvil::ui::PluginsTab tab;
                const auto body = [&] {
                    tab.draw_panes(snapshot, host);
                    tab.draw_confirm(host);
                };
                frame(ImVec2(-100, -100), body, ImVec2(width, 400));
                frame(ImVec2(-100, -100), body, ImVec2(width, 400));
                const std::vector<ImRect> toggle = items("##on"), reset = items(reset_label), remove = items("Remove...");
                CHECK(toggle.size() == 1 && reset.size() == 1 && remove.size() == 1);
                if (toggle.size() != 1 || reset.size() != 1 || remove.size() != 1) {
                    std::printf("  at %g: %zu switches, %zu resets, %zu removes\n", width, toggle.size(), reset.size(), remove.size());
                    continue;
                }
                const bool ok = !overlap(toggle[0], reset[0]) && !overlap(toggle[0], remove[0]) && !overlap(reset[0], remove[0]);
                CHECK(ok);
                if (!ok) std::printf("  overlap at a window %g wide\n", width);
                // A click on the switch turns the plugin off; it opens no confirmation.
                const ImVec2 at = toggle[0].GetCenter();
                host.applied.clear();
                frame(at, body, ImVec2(width, 400));
                ImGui::GetIO().AddMouseButtonEvent(0, true);
                frame(at, body, ImVec2(width, 400));
                ImGui::GetIO().AddMouseButtonEvent(0, false);
                frame(at, body, ImVec2(width, 400));
                frame(ImVec2(-100, -100), body, ImVec2(width, 400));
                if (host.applied.size() != 1) std::printf("  at %g: %zu applies\n", width, host.applied.size());
                CHECK(host.applied.size() == 1 && host.applied[0].size() == 1 && host.applied[0][0].key() == "tuning" &&
                      !host.applied[0][0].on);
                CHECK(ImGui::GetCurrentContext()->OpenPopupStack.Size == 0);
            }
        }
        anvil::i18n::set_language(anvil::i18n::Language::English);
    });

    ImGui::DestroyContext();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
