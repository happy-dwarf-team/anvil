// The Manager's sidebar (docs/ui-design-system.md, "Shell", decision B1): one
// entry per page, with its line icon, and folded to a rail of icons when the
// window is narrow. The icons are drawn, not typed, so no font needs them.
#pragma once

#include <imgui.h>

#include <cstddef>
#include <string>

namespace anvil::manager::ui {

enum class NavIcon { Anvil, Plugins, Catalog, Benchmark, Crashes, Log, About };

// Draws `icon` in a square of side `size` with its top-left corner at `at`.
void draw_nav_icon(ImDrawList* draw, ImVec2 at, float size, NavIcon icon, ImU32 color);

// The play triangle of the rail's Play button, centred on `center`.
void draw_play_glyph(ImDrawList* draw, ImVec2 center, float size, ImU32 color);

struct NavEntry {
    const char* id;     // the page's English name: its id, and its label through tr()
    NavIcon icon;
    size_t count = 0;   // what waits on the page; drawn as a pill, 0 for none
    bool warn = false;  // the count needs a look (warning colour) rather than news (info colour)
    std::string tip;    // what the count means, "" for none
};

// One entry of the sidebar, the full width of its window; true when clicked.
// `rail`: the icon alone, its name in the tooltip, the count as a corner glyph.
bool nav_item(const NavEntry& entry, bool selected, bool rail);

// The rows of the sidebar: one entry's height, at the current font.
float nav_item_height();

}  // namespace anvil::manager::ui
