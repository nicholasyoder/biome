// SPDX-License-Identifier: LGPL-3.0-or-later

#include "theme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPalette>
#include <QStyle>
#include <forest-theme/foresttheme.h>
#include <QStringList>
#include <QIODevice>
#include <QString>

// Q_INIT_RESOURCE must be called from outside any C++ namespace - it expands
// to a call to the qrc compiler's global ::qInitResources_theme(). Without
// this, the embedded resource never registers: Qt resources compiled into a
// *static* library get dropped by the linker unless something forces a
// reference to their translation unit.
static void ensure_decoration_resources_registered() {
    Q_INIT_RESOURCE(theme);
}

namespace biome_decoration {

namespace {

constexpr const char *kBuiltinPath = ":/biome/decoration/builtin.qss";

QString read_file(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

// Applies the theme's [Palette] to the app and returns QSS defaults derived
// from it, prepended so a layer's biome.css can still override them.
QString apply_palette(const QString &dir, const QStringList &layers) {
    bool found = false;
    const QPalette pal = foresttheme::palette(dir, layers, QApplication::style()->standardPalette(), &found);
    if (!found) {
        return {};
    }
    QApplication::setPalette(pal);
    const QColor hl = pal.color(QPalette::Highlight);
    return QString("QFrame#biomeSwitcherHighlight { background: rgba(%1, %2, %3, 15%); border-color: rgb(%1, %2, %3); }\n")
        .arg(hl.red()).arg(hl.green()).arg(hl.blue());
}

QString external_stylesheet(const QString &dir, const QString &name) {
    if (!QFileInfo::exists(QDir(dir).filePath(name + "/theme.conf"))) {
        qWarning("biome: theme '%s' not found in %s", qPrintable(name), qPrintable(dir));
        return {};
    }
    const QStringList layers = foresttheme::chain(dir, name);
    // shared.css is shared with Forest's own menus (fstyleloader).
    const QString sheet = foresttheme::stylesheet(dir, layers, {"biome", "shared"});
    if (sheet.trimmed().isEmpty()) {
        qWarning("biome: theme '%s' has no biome.css/shared.css layers", qPrintable(name));
        return {};
    }
    return apply_palette(dir, layers) + sheet;
}

} // namespace

void load_decoration_theme(const QString &dir, const QString &name) {
    ensure_decoration_resources_registered();

    QApplication::setPalette(QApplication::style()->standardPalette()); // drop a previous theme's palette
    QString stylesheet;
    if (!dir.isEmpty() && !name.isEmpty()) {
        stylesheet = external_stylesheet(dir, name);
    }
    if (stylesheet.isEmpty()) {
        stylesheet = read_file(kBuiltinPath);
    }
    qApp->setStyleSheet(stylesheet);
}

DecorationFrame *create_decoration_frame() {
    auto *frame = new DecorationFrame();
    // Gives the widget tree a valid initial layout before any real content
    // size is known - without this, borderWidth()/titlebarHeight() would
    // read content_spacer_'s pre-layout (0, 0) position if queried before
    // the first render_decoration() call, which re-runs layoutFor() with
    // the real content size on every repaint.
    frame->layoutFor(200, 200);
    return frame;
}

} // namespace biome_decoration
