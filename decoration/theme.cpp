// SPDX-License-Identifier: LGPL-3.0-or-later

#include "theme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
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

// Same algorithm as Forest's fstyleloader: parent_themes layers, then the theme itself.
QString external_stylesheet(const QString &dir, const QString &name) {
    const QString theme_dir = QDir(dir).filePath(name);
    if (!QFileInfo::exists(QDir(theme_dir).filePath("theme.conf"))) {
        qWarning("biome: theme '%s' not found in %s", qPrintable(name), qPrintable(dir));
        return {};
    }
    QSettings conf(QDir(theme_dir).filePath("theme.conf"), QSettings::IniFormat);
    QStringList layers = conf.value("parent_themes").toStringList();
    layers.append(name);

    QString sheet;
    for (const QString &layer : layers) {
        const QString path = QDir(dir).filePath(layer + "/biome.css");
        if (!QFileInfo::exists(path)) {
            continue;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qWarning("biome: cannot read %s", qPrintable(path));
            continue;
        }
        sheet += QString::fromUtf8(file.readAll()) + '\n';
    }
    if (sheet.trimmed().isEmpty()) {
        qWarning("biome: theme '%s' has no biome.css layers", qPrintable(name));
        return {};
    }
    return sheet;
}

} // namespace

void load_decoration_theme(const QString &dir, const QString &name) {
    ensure_decoration_resources_registered();

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
