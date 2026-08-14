// SPDX-License-Identifier: LGPL-3.0-or-later

#include "switcher.h"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPen>
#include <QString>

namespace biome_decoration {

namespace {

constexpr int kPanelWidth = 360;
constexpr int kPanelPadding = 10;
constexpr int kRowHeight = 32;
constexpr int kBorderThickness = 2;

QColor to_qcolor(const float rgba[4]) {
    QColor c;
    c.setRgbF(rgba[0], rgba[1], rgba[2], rgba[3]);
    return c;
}

} // namespace

RenderedFrame render_switcher(
        const std::vector<SwitcherEntry> &entries, int selected_index, const DecorationColors &colors) {
    RenderedFrame frame;
    if (entries.empty()) {
        return frame;
    }

    int width = kPanelWidth;
    int height = 2 * kPanelPadding + static_cast<int>(entries.size()) * kRowHeight;

    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QColor bg = to_qcolor(colors.titlebar_bg);
    QColor accent = to_qcolor(colors.border_focused);
    QColor fg = to_qcolor(colors.titlebar_fg);

    painter.fillRect(QRectF(0, 0, width, height), bg);
    painter.setPen(QPen(accent, kBorderThickness));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF(kBorderThickness / 2.0, kBorderThickness / 2.0,
        width - kBorderThickness, height - kBorderThickness));

    QFont font = painter.font();
    font.setPointSizeF(font.pointSizeF() > 0 ? font.pointSizeF() : 10.0);
    painter.setFont(font);
    QFontMetrics metrics(font);

    for (size_t i = 0; i < entries.size(); i++) {
        int row_y = kPanelPadding + static_cast<int>(i) * kRowHeight;
        QRectF row_rect(kPanelPadding, row_y, width - 2.0 * kPanelPadding, kRowHeight);
        if (static_cast<int>(i) == selected_index) {
            painter.fillRect(row_rect, accent);
            painter.setPen(Qt::white);
        } else {
            painter.setPen(fg);
        }
        QString text = QString::fromUtf8(entries[i].label.c_str());
        QString elided = metrics.elidedText(text, Qt::ElideRight, static_cast<int>(row_rect.width() - 16));
        painter.drawText(row_rect.adjusted(8, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, elided);
    }

    painter.end();

    frame.width = width;
    frame.height = height;
    frame.stride = image.bytesPerLine();
    frame.pixels.assign(image.constBits(), image.constBits() + static_cast<size_t>(image.sizeInBytes()));
    return frame;
}

} // namespace biome_decoration
