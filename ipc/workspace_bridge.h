// SPDX-License-Identifier: LGPL-3.0-or-later
//
// org.biome.Workspaces: the window-to-workspace relationship no Wayland
// protocol carries (architecture-notes.md "Workspace protocol"). Windows are
// named by their ext-foreign-toplevel-list identifier; workspaces by the
// ext-workspace id parsed as an integer.

#pragma once

#include "core/server.h"

#include <QObject>
#include <QString>
#include <QVariantMap>

class WorkspaceBridge : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.biome.Workspaces")

public:
    explicit WorkspaceBridge(BiomeServer *server, QObject *parent = nullptr);

public slots:
    // identifier -> workspace index, one entry per mapped toplevel.
    QVariantMap GetWindowWorkspaces();

    // No-op if `identifier` doesn't match any live toplevel (already
    // closed, or stale) - matches how workspace_handle_v1's own requests
    // silently ignore anything they can't honor (see ext_workspace.cpp).
    void MoveToplevelToWorkspace(const QString &identifier, int workspace);

signals:
    void WindowWorkspacesChanged(const QVariantMap &windowWorkspaces);

private:
    BiomeServer *m_server;
};

// Registers WorkspaceBridge on the session bus as org.biome at
// /org/biome/Workspaces, and wires BiomeServer::window_workspaces_changed
// to re-emit WindowWorkspacesChanged. Call after foreign_toplevel_init()
// (desktop/foreign_toplevel.h) so server->ftl.ext_list already
// exists.
void workspace_bridge_init(BiomeServer *server);
