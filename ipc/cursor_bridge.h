// SPDX-License-Identifier: LGPL-3.0-or-later
//
// org.biome.Cursor - lets Forest push a cursor theme/size change to Biome's
// own compositor-drawn cursor (window decorations, resize-direction
// cursors, XWayland's default cursor, and any cursor-shape-v1 client - see
// core/cursor.h's cursor_reload_theme) live, while it's running. No
// standard Wayland protocol covers this - there is no live-cursor-theme-
// push mechanism anywhere in the Wayland ecosystem, compositor-owned or
// not (sway's own `seat * xcursor_theme` IPC command is the closest
// precedent, and it's sway-specific too) - so this is a bespoke addition
// for the same reason org.biome.Workspaces is (see docs/phase4-plan.md
// Workstream D): nothing standard exists to reuse instead.
//
// A client that loaded and cached its own cursor images at startup (most
// Qt/GTK apps not using cursor-shape-v1) keeps them until restarted - a
// known, ecosystem-wide Wayland limitation, not something this bridge can
// work around. See system/system-settings/cursorthemesettings.cpp's
// settings-UI note in forest.

#pragma once

#include "core/server.h"

#include <QObject>
#include <QString>

class CursorBridge : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.biome.Cursor")

public:
    explicit CursorBridge(BiomeServer *server, QObject *parent = nullptr);

public slots:
    // theme empty -> falls back to "default" (~/.icons/default/index.theme).
    // size <= 0 -> falls back to 24, same default core/cursor.cpp uses.
    void SetTheme(const QString &theme, int size);

private:
    BiomeServer *m_server;
};

// Registers CursorBridge on the session bus as org.biome at
// /org/biome/Cursor. Call after cursor_init() (core/cursor.h) so
// server->cursor_mgr already exists.
void cursor_bridge_init(BiomeServer *server);
