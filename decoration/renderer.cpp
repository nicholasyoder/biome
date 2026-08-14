// SPDX-License-Identifier: LGPL-3.0-or-later

#include "renderer.h"
#include "layout.h"

#include <QColor>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QPen>
#include <QString>

namespace biome_decoration {

namespace {

QColor to_qcolor(const float rgba[4], float alpha_scale = 1.0f) {
    QColor c;
    c.setRgbF(rgba[0], rgba[1], rgba[2], rgba[3] * alpha_scale);
    return c;
}

void draw_buttons(QPainter &painter, int content_width, const QColor &fg) {
    ButtonRects buttons = button_rects(content_width);
    painter.setPen(QPen(fg, 1.4));
    painter.setBrush(Qt::NoBrush);

    auto glyph_box = [](int x, int y) {
        constexpr int kMargin = 4;
        return QRectF(x + kMargin, y + kMargin, kButtonSize - 2 * kMargin, kButtonSize - 2 * kMargin);
    };

    // Minimize: a single horizontal line near the bottom of the glyph box.
    QRectF min_box = glyph_box(buttons.minimize_x, buttons.y);
    painter.drawLine(QPointF(min_box.left(), min_box.bottom()), QPointF(min_box.right(), min_box.bottom()));

    // Maximize: a plain square outline.
    painter.drawRect(glyph_box(buttons.maximize_x, buttons.y));

    // Close: an X.
    QRectF close_box = glyph_box(buttons.close_x, buttons.y);
    painter.drawLine(close_box.topLeft(), close_box.bottomRight());
    painter.drawLine(close_box.topRight(), close_box.bottomLeft());
}

} // namespace

RenderedFrame render_decoration(int content_width, int content_height,
        const DecorationColors &colors, bool focused, const char *title) {
    RenderedFrame frame;
    int width = container_width(content_width);
    int height = container_height(content_height);
    if (width <= 0 || height <= 0) {
        return frame;
    }

    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Unfocused windows get a dimmer titlebar text/border, same convention
    // Phase 2's flat border already used (focused = full color).
    float alpha_scale = focused ? 1.0f : 0.75f;

    painter.fillRect(QRectF(0, 0, width, kTitlebarHeight), to_qcolor(colors.titlebar_bg));

    const float *border_rgba = focused ? colors.border_focused : colors.border_unfocused;
    QColor border = to_qcolor(border_rgba);
    painter.fillRect(QRectF(0, height - kBorderWidth, width, kBorderWidth), border);
    painter.fillRect(QRectF(0, kTitlebarHeight, kBorderWidth, height - kTitlebarHeight - kBorderWidth), border);
    painter.fillRect(QRectF(width - kBorderWidth, kTitlebarHeight, kBorderWidth, height - kTitlebarHeight - kBorderWidth), border);
    // Thin separator between the titlebar and the client's own content.
    painter.fillRect(QRectF(0, kTitlebarHeight - 1, width, 1), border);

    QColor fg = to_qcolor(colors.titlebar_fg, alpha_scale);
    painter.setPen(fg);
    QFont font = painter.font();
    font.setPointSizeF(font.pointSizeF() > 0 ? font.pointSizeF() : 10.0);
    painter.setFont(font);

    ButtonRects buttons = button_rects(content_width);
    int text_right_margin = width - buttons.minimize_x + 8;
    QRectF text_rect(8, 0, width - 8 - text_right_margin, kTitlebarHeight);
    painter.drawText(text_rect, Qt::AlignVCenter | Qt::AlignLeft,
        QString::fromUtf8(title != nullptr ? title : ""));

    draw_buttons(painter, content_width, fg);
    painter.end();

    frame.width = width;
    frame.height = height;
    frame.stride = image.bytesPerLine();
    frame.pixels.assign(image.constBits(), image.constBits() + static_cast<size_t>(image.sizeInBytes()));
    return frame;
}

} // namespace biome_decoration
