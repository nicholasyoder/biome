// SPDX-License-Identifier: LGPL-3.0-or-later

#include "theme_colors.h"

#include <QColor>
#include <QFile>
#include <QPixmap>
#include <QSettings>
#include <QStyle>
#include <QWidget>

#include <algorithm>
#include <iterator>

namespace {

constexpr const char *kThemesDir = "/usr/share/forest/themes/";

// Biome's own Phase 2 flat colors, used whenever Forest's theme files can't
// be found (e.g. running standalone without Forest installed).
constexpr float kFallbackTitlebarBg[4] = {0.16f, 0.16f, 0.16f, 1.0f};
constexpr float kFallbackTitlebarFg[4] = {0.90f, 0.90f, 0.90f, 1.0f};
constexpr float kFallbackBorderFocused[4] = {0.20f, 0.52f, 0.89f, 1.0f};
constexpr float kFallbackBorderUnfocused[4] = {0.35f, 0.35f, 0.35f, 1.0f};

void to_rgba(const QColor &color, float out[4]) {
    out[0] = static_cast<float>(color.redF());
    out[1] = static_cast<float>(color.greenF());
    out[2] = static_cast<float>(color.blueF());
    out[3] = static_cast<float>(color.alphaF());
}

// Mirrors forest/library/fstyleloader/fstyleloader.h's loadstyle(): resolves
// the active theme's parent_themes cascade and concatenates each theme's
// forest.css. Reads Forest's theme *files* directly rather than including
// its C++ header, so Biome stays independently buildable without a
// cross-repo source dependency.
QString load_forest_stylesheet() {
    QSettings app_settings("Forest", "Forest");
    QString theme = app_settings.value("theme", "Round-Dark").toString();

    QString theme_conf_path = QString(kThemesDir) + theme + "/theme.conf";
    QSettings theme_settings(theme_conf_path, QSettings::IniFormat);
    QStringList chain = theme_settings.value("parent_themes").toStringList();
    chain.append(theme);

    QString stylesheet;
    for (const QString &t : chain) {
        QFile css(QString(kThemesDir) + t + "/forest.css");
        if (css.open(QIODevice::ReadOnly | QIODevice::Text)) {
            stylesheet += QString::fromUtf8(css.readAll()) + "\n";
        }
    }
    return stylesheet;
}

// Renders a small probe widget tagged the same way Forest's own code tags
// its surface/pane widgets, through the real QSS cascade, and samples the
// resulting pixel color - rather than hand-parsing "background:" out of the
// CSS text.
bool probe_color(const QString &stylesheet, const char *fss_color, QColor *out) {
    if (stylesheet.isEmpty()) {
        return false;
    }
    QWidget probe;
    probe.setAttribute(Qt::WA_StyledBackground, true);
    probe.resize(4, 4);
    probe.setStyleSheet(stylesheet);
    probe.setProperty("FSS-color", fss_color);
    probe.style()->unpolish(&probe);
    probe.style()->polish(&probe);

    QPixmap pixmap(4, 4);
    pixmap.fill(Qt::transparent);
    probe.render(&pixmap);
    QColor sampled = pixmap.toImage().pixelColor(2, 2);
    if (sampled.alpha() == 0) {
        return false;
    }
    *out = sampled;
    return true;
}

} // namespace

namespace biome_decoration {

DecorationColors load_decoration_colors() {
    DecorationColors colors;
    std::copy(std::begin(kFallbackTitlebarBg), std::end(kFallbackTitlebarBg), colors.titlebar_bg);
    std::copy(std::begin(kFallbackTitlebarFg), std::end(kFallbackTitlebarFg), colors.titlebar_fg);
    std::copy(std::begin(kFallbackBorderFocused), std::end(kFallbackBorderFocused), colors.border_focused);
    std::copy(std::begin(kFallbackBorderUnfocused), std::end(kFallbackBorderUnfocused), colors.border_unfocused);

    QString stylesheet = load_forest_stylesheet();

    QColor surface;
    if (probe_color(stylesheet, "surface", &surface)) {
        to_rgba(surface, colors.titlebar_bg);
        // Perceptual-ish luminance to pick a readable title text color -
        // Forest's QSS has no title-text rule to source this from.
        double luminance = 0.299 * surface.redF() + 0.587 * surface.greenF() + 0.114 * surface.blueF();
        QColor fg = luminance > 0.5 ? QColor(20, 20, 20) : QColor(235, 235, 235);
        to_rgba(fg, colors.titlebar_fg);
    }

    QColor pane;
    if (probe_color(stylesheet, "pane", &pane)) {
        to_rgba(pane, colors.border_unfocused);
    }

    // Focused border stays Biome's own accent - Forest's QSS doesn't define
    // an accent/highlight color to source this from.
    return colors;
}

} // namespace biome_decoration
