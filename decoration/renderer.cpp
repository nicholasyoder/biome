// SPDX-License-Identifier: LGPL-3.0-or-later

#include "renderer.h"
#include "frame_widget.h"
#include "layout.h"

#include <QImage>
#include <QString>

namespace biome_decoration {

RenderedFrame render_decoration(DecorationFrame *widget, int content_width, int content_height,
        bool focused, bool maximized, const char *title, const IconImage &icon,
        Region hovered_region, Region pressed_region) {
    RenderedFrame frame;
    if (widget == nullptr || content_width <= 0 || content_height <= 0) {
        return frame;
    }

    widget->setMaximizedState(maximized);
    widget->layoutFor(content_width, content_height);
    widget->setFocusedState(focused);
    widget->setTitle(QString::fromUtf8(title != nullptr ? title : ""));
    widget->setIcon(icon);
    widget->setHoveredRegion(hovered_region);
    widget->setPressedRegion(pressed_region);

    int width = widget->width();
    int height = widget->height();
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    widget->render(&image);

    frame.width = width;
    frame.height = height;
    frame.stride = image.bytesPerLine();
    frame.pixels.assign(image.constBits(), image.constBits() + static_cast<size_t>(image.sizeInBytes()));
    return frame;
}

} // namespace biome_decoration
