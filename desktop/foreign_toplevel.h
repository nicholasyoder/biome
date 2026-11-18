// SPDX-License-Identifier: LGPL-3.0-or-later
//
// wlr-foreign-toplevel-management and ext-foreign-toplevel-list for taskbars,
// one handle pair per mapped toplevel, plus per-window capture sources. State
// is re-pushed from BiomeToplevel; wlroots drops no-op updates.

#pragma once

#include "core/server.h"

struct BiomeToplevel;

// Also creates the screen-capture globals.
void foreign_toplevel_init(BiomeServer *server);
void foreign_toplevel_finish(BiomeServer *server);

// Creates this toplevel's handle and sends its initial title/app_id/output/
// state. Called once from toplevel_map.
void foreign_toplevel_create(BiomeToplevel *toplevel);

// Destroys the handle (sends `closed` to any client still holding it).
// Called once from toplevel_unmap.
void foreign_toplevel_destroy(BiomeToplevel *toplevel);

// Hides/shows every window's private capture scene; blank while the session is locked.
void foreign_toplevel_set_capture_blank(BiomeServer *server, bool blank);

// Re-sends title/app_id from the toplevel's current xdg_toplevel/
// xwayland_surface fields - called from the existing set_title listeners
// (xdg_toplevel_set_title/xwayland_toplevel_set_title).
void foreign_toplevel_update_title_app_id(BiomeToplevel *toplevel);

// Re-pushes maximized/minimized/activated from BiomeToplevel's own flags -
// called from set_toplevel_maximized/set_toplevel_minimized/
// set_toplevel_focused (desktop/toplevel.cpp).
void foreign_toplevel_sync_state(BiomeToplevel *toplevel);

// Sends output_enter/output_leave for every output the frame box does/doesn't
// overlap. Geometry-based, so minimized/other-workspace windows keep theirs.
void foreign_toplevel_update_outputs(BiomeToplevel *toplevel);

// Sends the toplevel's transient parent (xdg/X11 parent), or none if it has
// no parent or the parent has no handle (unmapped). Called at create and from
// the set_parent listeners.
void foreign_toplevel_update_parent(BiomeToplevel *toplevel);

// The live toplevel with this ext-foreign-toplevel-list identifier, or null.
BiomeToplevel *foreign_toplevel_find_by_identifier(BiomeServer *server, const char *identifier);

// This toplevel's ext-foreign-toplevel-list identifier; null while unmapped.
const char *foreign_toplevel_identifier(BiomeToplevel *toplevel);
