// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Pointer/cursor plumbing: motion/button/axis/frame signal handlers and
// interactive move/resize state (BiomeServer::cursor_mode/grabbed_toplevel/
// grab_*). Dispatches a button/motion event to either a client surface
// (desktop/toplevel.h's desktop_toplevel_at) or a decoration region
// (desktop/decoration_bridge.h's decoration_toplevel_at) but never
// interprets a Region itself - see desktop/decoration_bridge.h for why.

#pragma once

#include "core/server.h"

struct BiomeToplevel;

void cursor_init(BiomeServer *server);

void reset_cursor_mode(BiomeServer *server);

// check_pointer_focus should be true for client-requested moves/resizes
// (xdg-shell/Xwayland request_move/request_resize - an unfocused client
// could send these unprompted, so they're checked against actual pointer
// focus) and false for moves/resizes Biome itself initiates from a
// decoration click (decoration_toplevel_at already found this exact
// toplevel under the cursor, so there's nothing to spoof).
void begin_interactive(BiomeToplevel *toplevel, BiomeCursorMode mode, uint32_t edges,
    bool check_pointer_focus);

// Adds drag_icon's surface (and any sub-surfaces) to the scene graph and
// positions it at the current cursor location - called from
// seat_start_drag (core/input.cpp) once a client-initiated drag with an
// icon actually begins. Tracks its own teardown via drag_icon_tree's
// destroy signal, so nothing else needs to call a matching destroy.
void drag_icon_create(BiomeServer *server, wlr_drag_icon *drag_icon);
