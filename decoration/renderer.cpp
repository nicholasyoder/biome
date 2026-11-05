// SPDX-License-Identifier: LGPL-3.0-or-later

#include "renderer.h"
#include "frame_widget.h"
#include "layout.h"

#include <QImage>
#include <QRect>
#include <QString>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace biome_decoration {

namespace {

// At fractional scales a hole edge (or a seam between two opaque strips) lands
// mid-pixel and antialiasing leaves that row partly transparent. Fill it from
// its opaque neighbour; the client draws over it.
void bleed_into_hole(RenderedFrame &frame, const QRect &hole, double scale) {
    auto px = [&](int x, int y) {
        return reinterpret_cast<uint32_t *>(frame.pixels.data() + static_cast<size_t>(y) * frame.stride) + x;
    };
    const double fx0 = hole.left() * scale, fx1 = (hole.left() + hole.width()) * scale;
    const double fy0 = hole.top() * scale, fy1 = (hole.top() + hole.height()) * scale;
    const int x0 = static_cast<int>(std::floor(fx0)), x1 = static_cast<int>(std::ceil(fx1));
    const int y0 = static_cast<int>(std::floor(fy0)), y1 = static_cast<int>(std::ceil(fy1));
    auto in = [&](int x, int y) { return x >= 0 && y >= 0 && x < frame.width && y < frame.height; };

    // Full width: the same seam sits where the titlebar meets the side strips,
    // and where they meet the bottom strip.
    for (int x = 0; x < frame.width; ++x) {
        if (fy0 != y0 && in(x, y0 - 1)) *px(x, y0) = *px(x, y0 - 1);
        if (fy1 != y1 && in(x, y1)) *px(x, y1 - 1) = *px(x, y1);
    }
    for (int y = std::max(y0, 0); y < std::min(y1, frame.height); ++y) {
        if (fx0 != x0 && in(x0 - 1, y)) *px(x0, y) = *px(x0 - 1, y);
        if (fx1 != x1 && in(x1, y)) *px(x1 - 1, y) = *px(x1, y);
    }
}

} // namespace

RenderedFrame render_widget(QWidget *widget, double scale) {
    RenderedFrame frame;
    int logical_width = widget->width();
    int logical_height = widget->height();
    int width = static_cast<int>(std::lround(logical_width * scale));
    int height = static_cast<int>(std::lround(logical_height * scale));
    if (width <= 0 || height <= 0) {
        return frame;
    }

    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(scale);
    image.fill(Qt::transparent);
    IconButton::render_scale = scale;
    widget->render(&image);
    IconButton::render_scale = 1.0;

    frame.width = width;
    frame.height = height;
    frame.logical_width = logical_width;
    frame.logical_height = logical_height;
    frame.stride = image.bytesPerLine();
    frame.pixels.assign(image.constBits(), image.constBits() + static_cast<size_t>(image.sizeInBytes()));
    return frame;
}

RenderedFrame render_decoration(DecorationFrame *widget, int content_width, int content_height,
        bool focused, bool urgent, bool maximized, const char *title, const IconImage &icon,
        Region hovered_region, Region pressed_region, double scale) {
    if (widget == nullptr || content_width <= 0 || content_height <= 0) {
        return {};
    }

    widget->setMaximizedState(maximized);
    widget->layoutFor(content_width, content_height);
    widget->setFocusedState(focused);
    widget->setUrgentState(urgent);
    widget->setTitle(QString::fromUtf8(title != nullptr ? title : ""));
    widget->setIcon(icon);
    widget->setHoveredRegion(hovered_region);
    widget->setPressedRegion(pressed_region);

    RenderedFrame frame = render_widget(widget, scale);
    if (frame.width > 0) {
        bleed_into_hole(frame, widget->contentRect(), scale);
    }
    return frame;
}

} // namespace biome_decoration
