// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include "layout.h" // Region

#include <cstdint>
#include <vector>

namespace biome_decoration {

// A rendered decoration frame (titlebar + border, transparent hole where
// the client's own surface shows through), as tightly-packed premultiplied
// ARGB8888 pixels, top-to-bottom, row-major - ready to hand straight to a
// software wlr_buffer. Empty (width/height == 0) if content_width/height
// were non-positive.
struct RenderedFrame {
    int width = 0;
    int height = 0;
    int stride = 0; // bytes per row
    std::vector<uint8_t> pixels;
};

// A resolved window icon - square ARGB32-premultiplied pixel data at
// whatever fixed size desktop/app_icon.h rasterizes to (decoration/ widgets
// scale it down to fit their own QSS-declared slot, the same way the SVG
// button glyphs are scale-independent of their rendered size). size == 0
// (pixels empty) means no icon was found - callers hide the icon widget
// entirely rather than showing a generic placeholder.
struct IconImage {
    int size = 0;
    std::vector<uint8_t> pixels;
};

// content_width/content_height: size of the client's own surface. Renders
// theme.h's persistent, QSS-styled DecorationFrame widget (borders,
// radius, padding, button hover/press all come from decoration/theme/
// biome-dark.qss) - requires load_decoration_theme() to have already been
// called, and a QApplication to already exist.
//
// hovered_region/pressed_region select which button (if any) gets a live
// QSS :hover/:pressed state - Region::None for neither. maximized drives the
// #biomeFrame[biomeMaximized=...] QSS state (see DecorationFrame::
// setMaximizedState()), letting a theme style a maximized window
// differently (e.g. no corner radius, thinner/no side borders). icon is the
// window's resolved icon (see IconImage above) - an empty one hides the
// titlebar icon slot.
RenderedFrame render_decoration(int content_width, int content_height,
    bool focused, bool maximized, const char *title, const IconImage &icon,
    Region hovered_region, Region pressed_region);

} // namespace biome_decoration
