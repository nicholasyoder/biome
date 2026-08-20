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

// Content-size-independent decoration metrics, read live off the real
// QSS-styled widget tree instead of a separately-duplicated constant - thin
// aliases so move/resize/place/maximize don't have to spell out
// biome_decoration::decoration_frame()->... at every use.
//
// maximized selects which QSS [maximized=...] state to read the metric
// from, since a theme is free to size the maximized border/titlebar
// differently. This matters because these wrap a single shared, mutable
// widget instance - the caller must say which state's metrics it wants
// rather than getting back whatever state some unrelated toplevel's last
// render left it in. Pass the toplevel's own `maximized` flag, or the
// target state when computing geometry for a transition not yet committed.
int decoration_border_width(bool maximized);
int decoration_titlebar_height(bool maximized);
// Right/bottom, kept separate since border strips are independently
// QSS-sized (not assumed symmetric).
int decoration_border_right_width(bool maximized);
int decoration_border_bottom_height(bool maximized);

// Same as the four above, but scoped to a specific toplevel: returns 0 if
// that toplevel opted out of Biome's decoration entirely (see
// toplevel_decorated - currently only possible for an Xwayland client).
inline int decoration_border_width(const BiomeToplevel *toplevel, bool maximized) {
    return toplevel_decorated(toplevel) ? decoration_border_width(maximized) : 0;
}
inline int decoration_titlebar_height(const BiomeToplevel *toplevel, bool maximized) {
    return toplevel_decorated(toplevel) ? decoration_titlebar_height(maximized) : 0;
}
inline int decoration_border_right_width(const BiomeToplevel *toplevel, bool maximized) {
    return toplevel_decorated(toplevel) ? decoration_border_right_width(maximized) : 0;
}
inline int decoration_border_bottom_height(const BiomeToplevel *toplevel, bool maximized) {
    return toplevel_decorated(toplevel) ? decoration_border_bottom_height(maximized) : 0;
}

// Creates the (initially empty) decoration_buffer scene node as a child of
// toplevel->scene_tree, positioned at its origin. Filled in by
// render_toplevel_decoration once geometry is known.
void create_toplevel_decoration(BiomeToplevel *toplevel);

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

// Called from both toplevel destroy handlers, before free(). Unlike
// BiomeServer::last_left_click_toplevel (only ever compared, never
// dereferenced), hovered_/pressed_decoration_toplevel ARE dereferenced when
// clearing hover/press state, so a dangling pointer here is a real
// use-after-free: press the close button, and the client can tear its
// surface down before the button-release event that would otherwise clear
// pressed_decoration_toplevel ever arrives.
void clear_decoration_tracking(BiomeServer *server, BiomeToplevel *toplevel);

const char *resize_cursor_name(biome_decoration::Region region);

// Dispatches a left-click hit-test result from decoration_toplevel_at (see
// core/cursor.cpp's server_cursor_button) - the click has already focused/
// raised toplevel by this point.
void handle_decoration_click(BiomeToplevel *toplevel, biome_decoration::Region region);

// Shows/refreshes/hides the Alt-Tab switcher overlay to match
// server->switcher_active and the current server->toplevels order. Called
// after every Tab press and on Alt release. Only adds a visual layer on top
// of the existing MRU logic - doesn't change which window Tab selects.
void update_switcher_overlay(BiomeServer *server);
