// SPDX-License-Identifier: LGPL-3.0-or-later
//
// The one module that knows both "wlroots scene graph" and "Qt-rendered
// decoration" - every direct use of biome_decoration:: is confined to this
// file and its .cpp. core/cursor.cpp dispatches pointer events here
// (decoration_toplevel_at/update_decoration_hover/set_decoration_pressed)
// but never inspects a Region itself.

#pragma once

#include "core/server.h"
#include "decoration/layout.h" // biome_decoration::Region
#include "desktop/toplevel.h"

// Creates the switcher overlay scene node. Called once from main() during
// server setup, before any toplevel exists.
void decoration_bridge_init(BiomeServer *server);

// Content-size-independent decoration metrics, read live off the toplevel's
// own QSS-styled widget tree (BiomeToplevel::decoration_frame) instead of a
// separately-duplicated constant. Returns 0 if toplevel opted out of
// Biome's decoration entirely - either an Xwayland client with
// _MOTIF_WM_HINTS asking for no border/title, or an xdg-shell client whose
// decoration negotiation settled on CLIENT_SIDE (see toplevel_decorated).
//
// maximized selects which QSS [biomeMaximized=...] state to read the metric
// from, since a theme is free to size the maximized border/titlebar
// differently - pass the toplevel's own `maximized` flag, or the target
// state when computing geometry for a transition not yet committed.
int decoration_border_width(const BiomeToplevel *toplevel, bool maximized);
int decoration_titlebar_height(const BiomeToplevel *toplevel, bool maximized);
// Right/bottom, kept separate since border strips are independently
// QSS-sized (not assumed symmetric).
int decoration_border_right_width(const BiomeToplevel *toplevel, bool maximized);
int decoration_border_bottom_height(const BiomeToplevel *toplevel, bool maximized);

// Creates toplevel's own DecorationFrame widget (biome_decoration::
// create_decoration_frame()) plus the (initially empty) decoration_buffer
// scene node as a child of toplevel->scene_tree, positioned at its origin.
// The buffer is filled in by render_toplevel_decoration once geometry is
// known.
void create_toplevel_decoration(BiomeToplevel *toplevel);

// Tears down what create_toplevel_decoration() built - called from both
// toplevel destroy handlers, before free(). decoration_buffer is a scene
// node, destroyed recursively along with scene_tree itself and needs no
// separate handling here; decoration_frame is a plain heap-allocated Qt
// widget with no scene-graph tie, so it does.
void destroy_toplevel_decoration(BiomeToplevel *toplevel);

// Re-renders the full decoration frame and uploads it. Called whenever a
// toplevel's content geometry, focus, title, or hover/press state changes.
void render_toplevel_decoration(BiomeToplevel *toplevel);

// Like desktop_toplevel_at (desktop/toplevel.h), but for Biome's own
// decoration_buffer nodes instead of client surfaces - the topmost-node
// lookup respects real stacking order. Returns nullptr (region left
// untouched) if the point isn't over any toplevel's decoration.
BiomeToplevel *decoration_toplevel_at(
    BiomeServer *server, double lx, double ly, biome_decoration::Region *out_region);

// Updates whichever toplevel's decoration button is under the pointer so
// its QSS :hover state stays in sync, re-rendering only the toplevel(s)
// whose hover actually changed. toplevel may be nullptr and region may be a
// non-button region, both meaning "no button is hovered".
void update_decoration_hover(BiomeServer *server, BiomeToplevel *toplevel,
    biome_decoration::Region region);

// Same shape as update_decoration_hover, but for the pressed QSS state a
// left button-down/up on a decoration button drives.
void set_decoration_pressed(BiomeServer *server, BiomeToplevel *toplevel,
    biome_decoration::Region region);

// Re-hit-tests at the pointer's current position and syncs hover to match -
// for whenever decoration geometry just changed under a stationary cursor
// (so no motion event will arrive to trigger the usual update_decoration_hover
// call in server_cursor_motion). Callers: the decoration button release
// itself, and - the case that actually matters, since xdg-shell maximize/
// restore only moves the scene node once the client's matching-size buffer
// lands - wherever maximize_reposition_pending gets resolved.
void refresh_decoration_hover(BiomeServer *server);

// Called from both toplevel destroy handlers, before free(). Unlike
// BiomeServer::last_left_click_toplevel (only ever compared, never
// dereferenced), hovered_/pressed_decoration_toplevel ARE dereferenced when
// clearing hover/press state, so a dangling pointer here is a real
// use-after-free: press the close button, and the client can tear its
// surface down before the button-release event that would otherwise clear
// pressed_decoration_toplevel ever arrives.
void clear_decoration_tracking(BiomeServer *server, BiomeToplevel *toplevel);

const char *resize_cursor_name(biome_decoration::Region region);

// Handles a left-button *press* over a decoration region (see core/
// cursor.cpp's server_cursor_button) - the click has already focused/raised
// toplevel by this point. Titlebar/resize regions act immediately (they
// begin an interactive grab that has to start on press). Button regions
// (min/max/close) do nothing here - real button semantics dictate they only
// commit on a matching release, see handle_decoration_release below, so the
// QSS :pressed state is actually visible and dragging off cancels the click.
void handle_decoration_press(BiomeToplevel *toplevel, biome_decoration::Region region);

// Handles a left-button *release*, called before the pressed-region tracking
// (set_decoration_pressed) is cleared for this event. If a button was armed
// by a preceding press (server->pressed_decoration_toplevel/pressed_region),
// commits its action only when the release lands back on that same
// toplevel/region - otherwise the press is silently cancelled.
void handle_decoration_release(BiomeServer *server);

// Shows/refreshes/hides the Alt-Tab switcher overlay to match
// server->switcher_active and the current server->toplevels order. Called
// after every Tab press and on Alt release. Only adds a visual layer on top
// of the existing MRU logic - doesn't change which window Tab selects.
void update_switcher_overlay(BiomeServer *server);
