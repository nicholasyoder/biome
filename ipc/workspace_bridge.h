// SPDX-License-Identifier: LGPL-3.0-or-later
//
// org.biome.Workspaces - the one Biome-specific DBus addition Workstream D
// needed. ext-workspace-v1 (desktop/ext_workspace.cpp) covers workspace
// switching/listing on its own, but neither it nor
// wlr-foreign-toplevel-management-unstable-v1 ties a toplevel to a
// workspace at all - see docs/phase4-plan.md's Workstream D for the full
// research. This interface exists purely for that one relationship:
//
//   - GetWindowWorkspaces/WindowWorkspacesChanged: which workspace each
//     open window is on. deskswitch tallies this into per-desktop counts
//     for its dot indicators; windowlist filters its button list down to
//     just the active workspace's windows.
//   - MoveToplevelToWorkspace: windowlist's "move to desktop" context-menu
//     action.
//
// Toplevels are named by their ext-foreign-toplevel-list-v1 identifier
// (BiomeServer::ext_foreign_toplevel_list, desktop/foreign_toplevel.cpp) -
// the only string identity a toplevel has that a DBus caller in a
// different process can reference at all.

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
    // identifier (string) -> workspace index (int). One entry per
    // currently-mapped toplevel that already has an identifier - a
    // just-mapped window's ext-foreign-toplevel-list-v1 handle briefly
    // lacks one until wlroots assigns it (see
    // foreign_toplevel_identifier()), so callers should treat a missing
    // entry as "not yet known" rather than "not open".
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
// (desktop/foreign_toplevel.h) so server->ext_foreign_toplevel_list already
// exists.
void workspace_bridge_init(BiomeServer *server);
