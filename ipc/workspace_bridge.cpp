// SPDX-License-Identifier: LGPL-3.0-or-later

#include "ipc/workspace_bridge.h"

#include "desktop/foreign_toplevel.h"
#include "desktop/toplevel.h"
#include "desktop/workspace.h"

#include <QDBusConnection>
#include <QDBusError>

namespace {

// Set as BiomeServer::window_workspaces_changed by workspace_bridge_init()
// below - a plain function pointer rather than a capturing callback since
// BiomeServer's field is a bare C function pointer (desktop/ doesn't link
// Qt - see that field's doc comment in core/server.h).
WorkspaceBridge *g_bridge = nullptr;

void notify_window_workspaces_changed(BiomeServer *server) {
    (void)server;
    if (g_bridge != nullptr) {
        emit g_bridge->WindowWorkspacesChanged(g_bridge->GetWindowWorkspaces());
    }
}

} // namespace

WorkspaceBridge::WorkspaceBridge(BiomeServer *server, QObject *parent) : QObject(parent), m_server(server) {
}

QVariantMap WorkspaceBridge::GetWindowWorkspaces() {
    QVariantMap windowWorkspaces;
    BiomeToplevel *pos;
    wl_list_for_each(pos, &m_server->toplevels, link) {
        const char *identifier = foreign_toplevel_identifier(pos);
        if (identifier != nullptr) {
            windowWorkspaces.insert(QString::fromUtf8(identifier), pos->workspace);
        }
    }
    return windowWorkspaces;
}

void WorkspaceBridge::MoveToplevelToWorkspace(const QString &identifier, int workspace) {
    BiomeToplevel *toplevel =
        foreign_toplevel_find_by_identifier(m_server, identifier.toUtf8().constData());
    if (toplevel == nullptr) {
        return;
    }
    move_toplevel_to_workspace(toplevel, workspace);
}

void workspace_bridge_init(BiomeServer *server) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        wlr_log(WLR_ERROR, "Biome: no D-Bus session bus available, org.biome.Workspaces disabled");
        return;
    }

    static WorkspaceBridge *bridge = new WorkspaceBridge(server);
    g_bridge = bridge;

    if (!bus.registerObject(QStringLiteral("/org/biome/Workspaces"), bridge,
            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals)) {
        wlr_log(WLR_ERROR, "Biome: failed to register org.biome.Workspaces object: %s",
            qPrintable(bus.lastError().message()));
        return;
    }
    if (!bus.registerService(QStringLiteral("org.biome"))) {
        wlr_log(WLR_ERROR, "Biome: failed to register org.biome bus name: %s",
            qPrintable(bus.lastError().message()));
    }

    server->window_workspaces_changed = notify_window_workspaces_changed;
}
