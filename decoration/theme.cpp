// SPDX-License-Identifier: LGPL-3.0-or-later

#include "theme.h"

#include <QColor>
#include <QFile>
#include <QIODevice>
#include <QImage>
#include <QPixmap>
#include <QString>

// Q_INIT_RESOURCE must be called from outside any C++ namespace - it
// expands to a call to the qrc compiler's global ::qInitResources_theme().
// Without this, the embedded resource never registers: Qt resources
// compiled into a *static* library get dropped by the linker unless
// something forces a reference to their translation unit, since nothing
// else in biome_decoration calls anything from it. (Verified directly:
// QFile(":/biome/decoration/biome-dark.qss").exists() was false without
// this call, silently leaving every decoration widget completely
// unstyled - transparent background/border, no button hover/press - since
// setStyleSheet() was applying an empty string.)
static void ensure_decoration_resources_registered() {
    Q_INIT_RESOURCE(theme);
}

namespace biome_decoration {

namespace {

constexpr const char *kStylesheetPath = ":/biome/decoration/biome-dark.qss";

void to_rgba(const QColor &color, float out[4]) {
    out[0] = static_cast<float>(color.redF());
    out[1] = static_cast<float>(color.greenF());
    out[2] = static_cast<float>(color.blueF());
    out[3] = static_cast<float>(color.alphaF());
}

// Samples a single pixel out of widget's real QSS-painted appearance.
QColor sample_pixel(QWidget *widget, int x, int y) {
    QImage image = widget->grab().toImage();
    if (x < 0 || y < 0 || x >= image.width() || y >= image.height()) {
        return QColor(Qt::transparent);
    }
    return image.pixelColor(x, y);
}

DecorationFrame *g_frame = nullptr;

} // namespace

DecorationColors load_decoration_theme() {
    ensure_decoration_resources_registered();

    QString stylesheet;
    QFile file(kStylesheetPath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        stylesheet = QString::fromUtf8(file.readAll());
    }

    g_frame = new DecorationFrame();
    g_frame->setStyleSheet(stylesheet);
    repolish_tree(g_frame);

    // decoration/switcher.cpp's Alt-Tab panel is a separate hand-painted
    // overlay (not part of the QSS-styled widget tree), so it still needs
    // flat colors - but every one of them is read back from the real
    // widgets below via pixel/palette sampling, not duplicated as a
    // hand-copied literal that could drift out of sync with the QSS.
    g_frame->layoutFor(200, 200);
    int titlebar_height = g_frame->titlebarHeight();
    int border_sample_y = titlebar_height + 10; // clear of the bottom radius

    g_frame->setFocusedState(true);
    QColor titlebar_bg = sample_pixel(g_frame->titlebarWidget(), 20, titlebar_height / 2);
    QColor titlebar_fg = g_frame->titleColor();
    QColor border_focused = sample_pixel(g_frame, 0, border_sample_y);

    g_frame->setFocusedState(false);
    QColor border_unfocused = sample_pixel(g_frame, 0, border_sample_y);

    g_frame->setFocusedState(true); // the default a fresh toplevel expects

    DecorationColors colors;
    to_rgba(titlebar_bg, colors.titlebar_bg);
    to_rgba(titlebar_fg, colors.titlebar_fg);
    to_rgba(border_focused, colors.border_focused);
    to_rgba(border_unfocused, colors.border_unfocused);
    return colors;
}

DecorationFrame *decoration_frame() {
    return g_frame;
}

} // namespace biome_decoration
