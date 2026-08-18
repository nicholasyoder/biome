// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Xwayland: managed toplevel surface lifecycle (mirrors xdg_shell.h's
// managed-toplevel handling) and override-redirect (unmanaged) surfaces
// (menus, tooltips, DnD icons - positioned by their own client, never part
// of server->toplevels).

#pragma once

#include "core/server.h"

// Starts Xwayland (lazily - the actual Xwayland process only spawns once a
// client tries to connect) and wires its listeners. Optional: if it fails
// to start (e.g. Xwayland isn't installed), Biome keeps running as a
// Wayland-only compositor - logs its own error and leaves server->xwayland
// null in that case.
void xwayland_init(BiomeServer *server, wlr_compositor *compositor);
