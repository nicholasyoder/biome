// SPDX-License-Identifier: LGPL-3.0-or-later

#include "layout.h"

namespace biome_decoration {

int container_width(int content_width) {
    return content_width + 2 * kBorderWidth;
}

int container_height(int content_height) {
    return content_height + kTitlebarHeight + kBorderWidth;
}

ButtonRects button_rects(int content_width) {
    int width = container_width(content_width);
    ButtonRects rects;
    rects.y = (kTitlebarHeight - kButtonSize) / 2;
    rects.close_x = width - kBorderWidth - kButtonMarginRight - kButtonSize;
    rects.maximize_x = rects.close_x - kButtonSpacing - kButtonSize;
    rects.minimize_x = rects.maximize_x - kButtonSpacing - kButtonSize;
    return rects;
}

Region hit_test(int local_x, int local_y, int content_width, int content_height) {
    int width = container_width(content_width);
    int height = container_height(content_height);

    if (local_x < 0 || local_y < 0 || local_x >= width || local_y >= height) {
        return Region::None;
    }

    bool near_left = local_x < kResizeCornerSize;
    bool near_right = local_x >= width - kResizeCornerSize;
    bool near_top = local_y < kResizeCornerSize;
    bool near_bottom = local_y >= height - kResizeCornerSize;

    // Corners take priority over the plain edges below - a WM convention
    // that gives diagonal resize a large-enough hit target near the frame's
    // corners instead of it being a single-pixel coincidence of two edges.
    if (near_top && near_left) {
        return Region::ResizeNW;
    }
    if (near_top && near_right) {
        return Region::ResizeNE;
    }
    if (near_bottom && near_left) {
        return Region::ResizeSW;
    }
    if (near_bottom && near_right) {
        return Region::ResizeSE;
    }

    // No plain ResizeN: the titlebar occupies the whole top edge outside the
    // corners, and is for moving, not resizing (matches most WMs - the top
    // corners are the only way to resize vertically from above).
    if (local_y >= height - kBorderWidth) {
        return Region::ResizeS;
    }
    if (local_x < kBorderWidth) {
        return Region::ResizeW;
    }
    if (local_x >= width - kBorderWidth) {
        return Region::ResizeE;
    }

    if (local_y < kTitlebarHeight) {
        ButtonRects buttons = button_rects(content_width);
        if (local_y >= buttons.y && local_y < buttons.y + kButtonSize) {
            if (local_x >= buttons.close_x && local_x < buttons.close_x + kButtonSize) {
                return Region::ButtonClose;
            }
            if (local_x >= buttons.maximize_x && local_x < buttons.maximize_x + kButtonSize) {
                return Region::ButtonMaximize;
            }
            if (local_x >= buttons.minimize_x && local_x < buttons.minimize_x + kButtonSize) {
                return Region::ButtonMinimize;
            }
        }
        return Region::Titlebar;
    }

    // Interior/content area - not part of the decoration, left to
    // desktop_toplevel_at's normal surface hit-testing.
    return Region::None;
}

} // namespace biome_decoration
