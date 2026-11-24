// SPDX-License-Identifier: LGPL-3.0-or-later

#include "touch_identify.h"

#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QPainter>

#include <cmath>

namespace biome_decoration {

namespace {

QImage blank(int logical_w, int logical_h, double scale) {
    QImage image(static_cast<int>(std::ceil(logical_w * scale)), static_cast<int>(std::ceil(logical_h * scale)),
        QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    return image;
}

} // namespace

RenderedFrame render_touch_target(double scale) {
    constexpr int kSize = 120;
    QImage image = blank(kSize, kSize, scale);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(scale, scale);
    const QPointF c(kSize / 2.0, kSize / 2.0);
    p.setPen(QPen(Qt::white, 4));
    p.setBrush(QColor(255, 255, 255, 40));
    p.drawEllipse(c, 56, 56);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, 32, 32);
    p.setBrush(Qt::white);
    p.drawEllipse(c, 8, 8);
    p.end();
    return {image, kSize, kSize};
}

RenderedFrame render_touch_prompt(const char *device_name, double scale) {
    QFont title = QGuiApplication::font();
    title.setPixelSize(36);
    title.setBold(true);
    QFont body = QGuiApplication::font();
    body.setPixelSize(20);
    const QString line1 = QStringLiteral("Tap the target on this screen");
    const QString line2 = QStringLiteral("if it's the touchscreen “%1”").arg(QString::fromUtf8(device_name));
    const QString line3 = QStringLiteral("Press Esc or click to skip");

    const QFontMetrics tm(title), bm(body);
    constexpr int kPad = 32, kGap = 12;
    const int w = std::max({tm.horizontalAdvance(line1), bm.horizontalAdvance(line2), bm.horizontalAdvance(line3)}) +
        2 * kPad;
    const int h = tm.height() + 2 * bm.height() + 2 * kGap + 2 * kPad;

    QImage image = blank(w, h, scale);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.scale(scale, scale);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 180));
    p.drawRoundedRect(QRectF(0, 0, w, h), 16, 16);
    int y = kPad;
    auto line = [&](const QFont &font, const QString &text, const QColor &color) {
        p.setFont(font);
        p.setPen(color);
        const QFontMetrics m(font);
        p.drawText(QRect(0, y, w, m.height()), Qt::AlignHCenter | Qt::AlignVCenter, text);
        y += m.height() + kGap;
    };
    line(title, line1, Qt::white);
    line(body, line2, QColor(230, 230, 230));
    line(body, line3, QColor(170, 170, 170));
    p.end();
    return {image, w, h};
}

} // namespace biome_decoration
