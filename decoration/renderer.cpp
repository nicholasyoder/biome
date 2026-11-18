// SPDX-License-Identifier: LGPL-3.0-or-later

#include "renderer.h"
#include "frame_widget.h"
#include "layout.h"

#include <QImage>
#include <QRect>
#include <QRegion>
#include <QString>
#include <QWidget>

#include <cmath>
#include <cstring>

namespace biome_decoration {

namespace {

// At fractional scales an image's last row/column can be only partly painted.
// Where that edge meets the client or another strip, fill it from its neighbour.
void bleed_edges(QImage &image, const QRect &source, double scale, bool right, bool bottom) {
    constexpr double kEpsilon = 1e-3;
    const int w = image.width(), h = image.height();
    if (right && w >= 2 && w > source.width() * scale + kEpsilon) {
        for (int y = 0; y < h; ++y) {
            auto *row = reinterpret_cast<uint32_t *>(image.scanLine(y));
            row[w - 1] = row[w - 2];
        }
    }
    if (bottom && h >= 2 && h > source.height() * scale + kEpsilon) {
        std::memcpy(image.scanLine(h - 1), image.constScanLine(h - 2), static_cast<size_t>(w) * 4);
    }
}

} // namespace

RenderedFrame render_widget(QWidget *widget, double scale, const QRect &source) {
    const QRect rect = source.isNull() ? widget->rect() : source;
    int width = static_cast<int>(std::lround(rect.width() * scale));
    int height = static_cast<int>(std::lround(rect.height() * scale));
    if (width <= 0 || height <= 0) {
        return {};
    }

    RenderedFrame frame;
    frame.image = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
    frame.image.setDevicePixelRatio(scale);
    frame.image.fill(Qt::transparent);
    IconButton::render_scale = scale;
    widget->render(&frame.image, QPoint(), QRegion(rect));
    IconButton::render_scale = 1.0;
    frame.logical_width = rect.width();
    frame.logical_height = rect.height();
    return frame;
}

bool update_decoration(DecorationFrame *widget, int content_width, int content_height,
        bool focused, bool urgent, bool maximized, const char *title, const IconImage &icon,
        Region hovered_region, Region pressed_region) {
    if (widget == nullptr || content_width <= 0 || content_height <= 0) {
        return false;
    }
    widget->setMaximizedState(maximized);
    widget->layoutFor(content_width, content_height);
    widget->setFocusedState(focused);
    widget->setUrgentState(urgent);
    widget->setTitle(QString::fromUtf8(title != nullptr ? title : ""));
    widget->setIcon(icon);
    widget->setHoveredRegion(hovered_region);
    widget->setPressedRegion(pressed_region);
    return true;
}

RenderedFrame render_decoration_strip(DecorationFrame *widget, Strip strip, double scale) {
    const QRect source = widget->stripRect(strip);
    if (source.isEmpty()) {
        return {};
    }
    RenderedFrame frame = render_widget(widget, scale, source);
    if (!frame.image.isNull()) {
        // Outer edges keep their antialiasing; inner ones meet the client or another strip.
        bleed_edges(frame.image, source, scale, strip == Strip::Left,
            strip == Strip::Top || strip == Strip::Left || strip == Strip::Right);
    }
    return frame;
}

} // namespace biome_decoration
