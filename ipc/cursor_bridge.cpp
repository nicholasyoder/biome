// SPDX-License-Identifier: LGPL-3.0-or-later

#include "ipc/cursor_bridge.h"

#include "core/cursor.h"

#include <QDBusConnection>
#include <QDBusError>

CursorBridge::CursorBridge(BiomeServer *server, QObject *parent) : QObject(parent), m_server(server) {
}

void CursorBridge::SetTheme(const QString &theme, int size) {
    QByteArray theme_utf8 = theme.toUtf8();
    cursor_reload_theme(m_server, theme.isEmpty() ? nullptr : theme_utf8.constData(), size > 0 ? static_cast<uint32_t>(size) : 24);
}

void cursor_bridge_init(BiomeServer *server) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        wlr_log(WLR_ERROR, "Biome: no D-Bus session bus available, org.biome.Cursor disabled");
        return;
    }

    static CursorBridge *bridge = new CursorBridge(server);

    if (!bus.registerObject(QStringLiteral("/org/biome/Cursor"), bridge, QDBusConnection::ExportAllSlots)) {
        wlr_log(WLR_ERROR, "Biome: failed to register org.biome.Cursor object: %s",
            qPrintable(bus.lastError().message()));
        return;
    }
    if (!bus.registerService(QStringLiteral("org.biome"))) {
        wlr_log(WLR_ERROR, "Biome: failed to register org.biome bus name: %s",
            qPrintable(bus.lastError().message()));
    }
}
