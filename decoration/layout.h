// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Shared decoration geometry: the renderer (decoration/renderer.h) paints
// these regions, and core/main.cpp's pointer hit-testing must click the
// same regions it paints. Kept dependency-free (no Qt, no wlroots) so both
// sides can include it cheaply.

#pragma once

namespace biome_decoration {

constexpr int kBorderWidth = 2;      // left/right/bottom border thickness
constexpr int kTitlebarHeight = 28;  // top titlebar height
constexpr int kButtonSize = 16;
constexpr int kButtonSpacing = 6;
constexpr int kButtonMarginRight = 6;
constexpr int kResizeCornerSize = 12; // corner hit region, both axes

enum class Region {
    None,
    Titlebar,
    ButtonMinimize,
    ButtonMaximize,
    ButtonClose,
    ResizeN,
    ResizeS,
    ResizeE,
    ResizeW,
    ResizeNE,
    ResizeNW,
    ResizeSE,
    ResizeSW,
};

struct ButtonRects {
    int minimize_x, maximize_x, close_x;
    int y;
};

// content_width/content_height: size of the client's own surface, excluding
// the border/titlebar frame around it.
int container_width(int content_width);
int container_height(int content_height);

// Buttons are right-aligned in the titlebar; y is shared by all three.
ButtonRects button_rects(int content_width);

// local_x/local_y are relative to the container's top-left corner (the
// scene_tree container position, same origin move/resize/focus already use).
Region hit_test(int local_x, int local_y, int content_width, int content_height);

} // namespace biome_decoration
