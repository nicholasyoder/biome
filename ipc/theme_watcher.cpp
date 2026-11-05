// SPDX-License-Identifier: LGPL-3.0-or-later

#include "ipc/theme_watcher.h"

// Before any Qt header: wlroots uses `slots` as a field name.
#include "desktop/decoration_bridge.h"

#include "core/config.h"

#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QSettings>
#include <QTimer>

namespace {

struct ThemeWatcher {
    BiomeServer *server = nullptr;
    QFileSystemWatcher watcher;
    QTimer debounce;
    QString dir, name;

    void rearm() {
        if (!watcher.directories().isEmpty())
            watcher.removePaths(watcher.directories());
        QString d = QFileInfo(QSettings("Biome", "Biome").fileName()).path();
        while (!QFileInfo::exists(d) && d != QFileInfo(d).path())
            d = QFileInfo(d).path();
        watcher.addPath(d);
    }

    void reload() {
        rearm();
        const BiomeConfig fresh = load_biome_config();
        const QString new_dir = fresh.value("Theme/dir").toString();
        const QString new_name = fresh.value("Theme/name").toString();
        if (new_dir == dir && new_name == name)
            return;
        dir = new_dir;
        name = new_name;
        reload_decoration_theme(server, dir, name);
    }
};

} // namespace

void theme_watcher_init(BiomeServer *server) {
    static ThemeWatcher *w = new ThemeWatcher;
    w->server = server;
    w->dir = biome_config().value("Theme/dir").toString();
    w->name = biome_config().value("Theme/name").toString();

    // The directory is watched, not the file: QSettings writes by atomic rename.
    w->debounce.setSingleShot(true);
    w->debounce.setInterval(150);
    QObject::connect(&w->debounce, &QTimer::timeout, [] { w->reload(); });
    QObject::connect(&w->watcher, &QFileSystemWatcher::directoryChanged, [] { w->debounce.start(); });
    w->rearm();
}
