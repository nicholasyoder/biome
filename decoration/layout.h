// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Shared decoration geometry: the renderer (decoration/renderer.h) paints
// these regions, and core/main.cpp's pointer hit-testing must click the
// same regions it paints. Kept dependency-free (no Qt, no wlroots) so both
// sides can include it cheaply.

#pragma once

namespace biome_decoration {

// Decoration geometry, QSS-driven via decoration/theme.h's qproperty-*
// read-back (see decoration/theme/biome-dark.qss) - runtime values rather
// than constexpr so a theme file can change them. Set once at startup by
// theme.cpp's load_decoration_theme(), before any toplevel exists; default
// initializers below are the fallback if the embedded theme fails to load.
extern int kBorderWidth;      // left/right/bottom border thickness
extern int kTitlebarHeight;   // top titlebar height
extern int kButtonSize;
extern int kButtonSpacing;
extern int kButtonMarginRight;
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

struct TitleRect {
    int x, y, width, height;
};

struct EdgeRect {
    int x, y, width, height;
};

struct BorderRects {
    EdgeRect left, right, bottom;
};

// content_width/content_height: size of the client's own surface, excluding
// the border/titlebar frame around it.
int container_width(int content_width);
int container_height(int content_height);

// Buttons are right-aligned in the titlebar; y is shared by all three.
ButtonRects button_rects(int content_width);

// The title label fills the titlebar left of the buttons.
TitleRect title_rect(int content_width);

// The left/right/bottom border strips, below the titlebar - each painted
// by its own DecorationBorder widget (see frame_widget.h) rather than a
// single CSS border spanning the whole frame.
BorderRects border_rects(int content_width, int content_height);

// local_x/local_y are relative to the container's top-left corner (the
// scene_tree container position, same origin move/resize/focus already use).
Region hit_test(int local_x, int local_y, int content_width, int content_height);

} // namespace biome_decoration
