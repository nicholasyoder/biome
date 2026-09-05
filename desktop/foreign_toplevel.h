// SPDX-License-Identifier: LGPL-3.0-or-later
//
// wlr-foreign-toplevel-management-unstable-v1: exposes every open
// BiomeToplevel to external clients (a taskbar like Forest's windowlist
// plugin, or Waybar's wlr/taskbar module) for listing and basic control
// (activate/maximize/minimize/close). One BiomeForeignToplevel wrapper per
// BiomeToplevel, created/destroyed alongside it in toplevel_map/
// toplevel_unmap (desktop/toplevel.cpp) - same create-once-per-visible-
// lifetime shape as decoration_bridge.h's create_toplevel_decoration/
// destroy_toplevel_decoration. State (maximized/minimized/activated) is
// never tracked independently here - foreign_toplevel_sync_state() just
// re-pushes BiomeToplevel's own flags, and wlroots itself dedups a no-op
// set against the handle's current state, so callers don't need to guard
// against redundant calls either.
//
// No fullscreen support: Biome has none anywhere (xdg/xwayland fullscreen
// requests are unconditionally denied - see xdg_toplevel_request_fullscreen/
// xwayland_toplevel_request_fullscreen in xdg_shell.cpp/xwayland_shell.cpp),
// so the protocol's fullscreen bit and set_fullscreen/unset_fullscreen
// requests are permanently unset/no-ops here.

#pragma once

#include "core/server.h"

struct BiomeToplevel;

// Creates the zwlr_foreign_toplevel_manager_v1 global.
void foreign_toplevel_init(BiomeServer *server);

// Creates this toplevel's handle and sends its initial title/app_id/output/
// state. Called once from toplevel_map.
void foreign_toplevel_create(BiomeToplevel *toplevel);

// Destroys the handle (sends `closed` to any client still holding it).
// Called once from toplevel_unmap.
void foreign_toplevel_destroy(BiomeToplevel *toplevel);

// Re-sends title/app_id from the toplevel's current xdg_toplevel/
// xwayland_surface fields - called from the existing set_title listeners
// (xdg_toplevel_set_title/xwayland_toplevel_set_title).
void foreign_toplevel_update_title_app_id(BiomeToplevel *toplevel);

// Re-pushes maximized/minimized/activated from BiomeToplevel's own flags -
// called from set_toplevel_maximized/set_toplevel_minimized/
// set_toplevel_focused (desktop/toplevel.cpp).
void foreign_toplevel_sync_state(BiomeToplevel *toplevel);

// Finds the toplevel whose ext-foreign-toplevel-list-v1 identifier matches
// (see BiomeServer::ext_foreign_toplevel_list's doc comment in
// core/server.h) - used by ipc/workspace_bridge.cpp's
// MoveToplevelToWorkspace to resolve the identifier a DBus caller passed
// back to a real BiomeToplevel. Returns nullptr if no live toplevel has
// that identifier (already closed, or a stale/malformed identifier).
BiomeToplevel *foreign_toplevel_find_by_identifier(BiomeServer *server, const char *identifier);

// Returns this toplevel's ext-foreign-toplevel-list-v1 identifier, or
// nullptr if it doesn't have one (no ext_foreign_toplevel_list global, or
// wlroots hasn't assigned one yet). Used by ipc/workspace_bridge.cpp to key
// its per-window workspace map by the same identifier Forest's windowlist
// pairs against - see foreign_toplevel_find_by_identifier's doc comment
// above for the reverse direction.
const char *foreign_toplevel_identifier(BiomeToplevel *toplevel);
