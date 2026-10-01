// SPDX-License-Identifier: LGPL-3.0-or-later

#include "ipc/cursor_theme_watcher.h"

#include "core/cursor.h"

#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QSettings>
#include <QTimer>

#include <cstdlib>

namespace {

constexpr uint32_t kDefaultCursorSize = 24;

struct CursorThemeWatcher {
    BiomeServer *server = nullptr;
    QString index_path;
    QFileSystemWatcher watcher;
    QTimer debounce;
    QString theme;
    uint32_t size = kDefaultCursorSize;

    // A missing file or directory can't be watched, and atomic replaces drop
    // the file watch, so re-arm on every event: the file plus its nearest
    // existing ancestor directory.
    void rearm() {
        if (!watcher.files().isEmpty())
            watcher.removePaths(watcher.files());
        if (!watcher.directories().isEmpty())
            watcher.removePaths(watcher.directories());

        if (QFileInfo::exists(index_path))
            watcher.addPath(index_path);
        QString dir = QFileInfo(index_path).path();
        while (!QFileInfo::exists(dir) && dir != QFileInfo(dir).path())
            dir = QFileInfo(dir).path();
        watcher.addPath(dir);
    }

    void reload() {
        rearm();
        if (!QFileInfo::exists(index_path))
            return;

        QSettings index(index_path, QSettings::IniFormat);
        index.beginGroup(QStringLiteral("Icon Theme"));
        const QString new_theme = index.value(QStringLiteral("Inherits")).toStringList().value(0).trimmed();
        const int parsed_size = index.value(QStringLiteral("Size")).toInt();
        const uint32_t new_size = parsed_size > 0 ? static_cast<uint32_t>(parsed_size) : kDefaultCursorSize;
        if (new_theme == theme && new_size == size)
            return;

        theme = new_theme;
        size = new_size;
        const QByteArray theme_utf8 = theme.toUtf8();
        cursor_reload_theme(server, theme.isEmpty() ? nullptr : theme_utf8.constData(), size);
    }
};

} // namespace

void cursor_theme_watcher_init(BiomeServer *server) {
    static CursorThemeWatcher *w = new CursorThemeWatcher;
    w->server = server;
    w->index_path = QDir::homePath() + QStringLiteral("/.icons/default/index.theme");

    // Match what cursor_init() loaded so startup doesn't trigger a reload.
    w->theme = QString::fromLocal8Bit(getenv("XCURSOR_THEME"));
    const char *size_env = getenv("XCURSOR_SIZE");
    const int parsed_size = size_env ? atoi(size_env) : 0;
    w->size = parsed_size > 0 ? static_cast<uint32_t>(parsed_size) : kDefaultCursorSize;

    // Settings UIs rewrite the file on every keystroke/click.
    w->debounce.setSingleShot(true);
    w->debounce.setInterval(150);
    QObject::connect(&w->debounce, &QTimer::timeout, [] { w->reload(); });
    QObject::connect(&w->watcher, &QFileSystemWatcher::fileChanged, [] { w->debounce.start(); });
    QObject::connect(&w->watcher, &QFileSystemWatcher::directoryChanged, [] { w->debounce.start(); });

    w->rearm();
}
