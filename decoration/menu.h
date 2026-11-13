// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Renders a context menu (the window menu) as a real, QSS-styled QMenu, so it
// picks up the same shared.css rules as Forest's own menus. Input handling is
// the compositor's: this only draws and reports where each item landed.

#pragma once

#include "renderer.h" // RenderedFrame

#include <string>
#include <vector>

namespace biome_decoration {

struct MenuEntry {
    int id = 0; // caller-defined; unique within one menu
    std::string label;
    bool enabled = true;
    bool checked = false; // shows the theme's indicator
    bool separator = false;
    bool submenu = false; // shows the submenu arrow
};

// Logical pixels, relative to the rendered frame's top-left.
struct MenuItemRect {
    int id = 0;
    int x = 0, y = 0, width = 0, height = 0;
};

struct RenderedMenu {
    RenderedFrame frame;
    std::vector<MenuItemRect> items; // separators omitted
    // Transparent QSS margin around the visible frame, same as Forest's
    // menuanchor: placement aligns the visible corner, not the buffer's.
    int margin = 0;
};

// panel selects one of two persistent widgets, so a submenu can be shown
// beside its parent. hovered_id (-1 for none) gets the :selected state.
RenderedMenu render_menu(int panel, const std::vector<MenuEntry> &entries, int hovered_id, double scale);

} // namespace biome_decoration
