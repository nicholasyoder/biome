// SPDX-License-Identifier: LGPL-3.0-or-later

#include "renderer.h"
#include "frame_widget.h"
#include "layout.h"

#include <QImage>
#include <QString>
#include <QWidget>

#include <cmath>

namespace biome_decoration {

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

    return render_widget(widget, scale);
}

} // namespace biome_decoration
