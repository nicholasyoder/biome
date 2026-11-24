// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Pointer events (server->pointer) and interactive move/resize
// (server->grab). Routes each event to a client surface or a decoration
// region (desktop/decoration_bridge.h).

#pragma once

#include "core/server.h"

struct BiomeToplevel;

void cursor_init(BiomeServer *server);
void cursor_finish(BiomeServer *server);

// Sets a named cursor and remembers it so cursor_reload_theme can re-apply it.
void cursor_set_xcursor(BiomeServer *server, const char *name);

// Destroys and recreates server->cursor_mgr from the given theme/size (a
// NULL theme falls back to the literal name "default", resolved via
// ~/.icons/default/index.theme's Inherits= line - see wlroots'
// wlr_xcursor_theme_load) and reloads it at scale 1, the same
// recreate-on-change approach sway uses for its `seat * xcursor_theme`
// command. Only Biome's own compositor-drawn cursor (decorations, resize
// cursors, XWayland's default) and cursor-shape-v1 clients see this take
// effect immediately - a client that loaded its own cursor images at
// startup keeps them until restarted, which is a Wayland-wide limitation,
// not something this can work around. Called from ipc/cursor_theme_watcher.cpp.
void cursor_reload_theme(BiomeServer *server, const char *theme, uint32_t size);

void reset_cursor_mode(BiomeServer *server);

// Re-runs pointer focus/hover at the current position, e.g. after a warp.
void cursor_rebase(BiomeServer *server);

// A client-requested one (check_pointer_focus) follows a touch point instead
// of the pointer when one is down on the toplevel.
// check_pointer_focus should be true for client-requested moves/resizes
// (xdg-shell/Xwayland request_move/request_resize - an unfocused client
// could send these unprompted, so they're checked against actual pointer
// focus) and false for moves/resizes Biome itself initiates from a
// decoration click (decoration_toplevel_at already found this exact
// toplevel under the cursor, so there's nothing to spoof).
void begin_interactive(BiomeToplevel *toplevel, BiomeCursorMode mode, uint32_t edges,
    bool check_pointer_focus);

// Shows a drag's icon at lx/ly; it removes itself when the icon goes.
void drag_icon_create(BiomeServer *server, wlr_drag_icon *drag_icon, double lx, double ly);

// Pointer emulation for touch (core/touch.cpp): a warp plus the usual motion
// routing, and a left-button press/release.
void cursor_warp(BiomeServer *server, double lx, double ly, uint32_t time);
void cursor_emulate_button(BiomeServer *server, uint32_t time_msec, bool pressed);

// What a press on surface (null: empty space) does to keyboard focus and
// xdg-activation; toplevel is its BiomeToplevel, if any.
void focus_for_press(BiomeServer *server, BiomeToplevel *toplevel, wlr_surface *surface);
