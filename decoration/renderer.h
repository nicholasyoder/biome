// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include "theme_colors.h"

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

// content_width/content_height: size of the client's own surface. Requires
// a QApplication to already exist (uses QPainter/QImage).
RenderedFrame render_decoration(int content_width, int content_height,
    const DecorationColors &colors, bool focused, const char *title);

} // namespace biome_decoration
