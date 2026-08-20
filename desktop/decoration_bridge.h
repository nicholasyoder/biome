// SPDX-License-Identifier: LGPL-3.0-or-later
//
// The one module that knows both "wlroots scene graph" and "Qt-rendered
// decoration" - every direct use of biome_decoration:: (Region hit-testing,
// RenderedFrame, the switcher, decoration_frame()) is confined to this file
// and desktop/decoration_bridge.cpp. core/cursor.cpp dispatches pointer
// events here (decoration_toplevel_at/update_decoration_hover/
// set_decoration_pressed) but never inspects a Region itself; it only knows
// "this point might be over a decoration, ask decoration_bridge".

#pragma once

#include "core/server.h"
#include "decoration/layout.h" // biome_decoration::Region
#include "desktop/toplevel.h"

// Creates the switcher overlay scene node. Called once from main() during
// server setup, before any toplevel exists.
void decoration_bridge_init(BiomeServer *server);

// Content-size-independent decoration metrics, read live off the real
// QSS-styled widget tree (decoration/theme/biome-dark.qss) instead of a
// separately-duplicated constant - see DecorationFrame::borderWidth()/
// titlebarHeight() (decoration/frame_widget.h). Thin call-site aliases only
// so the offset arithmetic sprinkled through move/resize/place/maximize
// doesn't have to spell out biome_decoration::decoration_frame()->... at
// every use.
//
// maximized selects which QSS [maximized=...] state to read the metric
// from, since a theme is free to size the maximized border/titlebar
// differently (decoration/theme/biome-dark.qss). This matters because these
// wrap a single shared, mutable widget instance (decoration_frame()) - the
// caller must say which state's metrics it wants, rather than getting back
// whatever state some unrelated toplevel's last render happened to leave it
// in. Pass the toplevel's own `maximized` flag (or, when computing the
// geometry for a state transition that hasn't been committed yet, the state
// being transitioned to).
int decoration_border_width(bool maximized);
int decoration_titlebar_height(bool maximized);
// Right border width / bottom border height - kept separate from the two
// above since border strips are independently QSS-sized (not assumed
// symmetric). See DecorationFrame::rightBorderWidth()/bottomBorderHeight().
int decoration_border_right_width(bool maximized);
int decoration_border_bottom_height(bool maximized);

// Creates the (initially empty) decoration_buffer scene node as a child of
// toplevel->scene_tree (the container), positioned at its origin. Filled in
// by render_toplevel_decoration once geometry is known.
void create_toplevel_decoration(BiomeToplevel *toplevel);

// Re-renders the full decoration frame (titlebar + border, using
// toplevel->focused/hovered_region/pressed_region against the QSS-styled
// widget tree in decoration/theme.h) and uploads it. Called whenever a
// toplevel's content geometry, focus, title, or hover/press state changes.
void render_toplevel_decoration(BiomeToplevel *toplevel);

// Like desktop_toplevel_at (desktop/toplevel.h), but for Biome's own
// decoration_buffer nodes instead of client surfaces - the topmost-node
// lookup is what makes this respect real stacking order (a focused
// window's decoration correctly occludes a window behind it). Returns
// nullptr (region left untouched) if the point isn't over any toplevel's
// decoration - including when it's over a client surface, or over the
// empty interior gap DecorationFrame::hitTest() leaves for
// desktop_toplevel_at to handle instead.
BiomeToplevel *decoration_toplevel_at(
    BiomeServer *server, double lx, double ly, biome_decoration::Region *out_region);

// Updates whichever toplevel's decoration button is under the pointer so
// its QSS :hover state stays in sync, re-rendering only the toplevel(s)
// whose hover actually changed. toplevel/region come straight from
// decoration_toplevel_at - toplevel may be nullptr (pointer isn't over any
// decoration) and region may be a non-button region (titlebar/border), both
// of which mean "no button is hovered".
void update_decoration_hover(BiomeServer *server, BiomeToplevel *toplevel,
    biome_decoration::Region region);

// Same shape as update_decoration_hover, but for the pressed QSS state a
// left button-down/up on a decoration button drives, rather than pointer
// motion.
void set_decoration_pressed(BiomeServer *server, BiomeToplevel *toplevel,
    biome_decoration::Region region);

// Called from both toplevel destroy handlers, before free(). Unlike
// BiomeServer::last_left_click_toplevel (only ever compared via ==, never
// dereferenced, so a stale pointer there is harmless),
// hovered_/pressed_decoration_toplevel ARE dereferenced when clearing
// hover/press state (render_toplevel_decoration) - so a dangling pointer
// here is a real use-after-free. Concretely: press the close button, and
// the client can tear its surface down (freeing the toplevel) before the
// button-release event that would otherwise clear
// pressed_decoration_toplevel ever arrives.
void clear_decoration_tracking(BiomeServer *server, BiomeToplevel *toplevel);

const char *resize_cursor_name(biome_decoration::Region region);

// Dispatches a left-click hit-test result from decoration_toplevel_at (see
// core/cursor.cpp's server_cursor_button) - the click has already focused/
// raised toplevel by this point.
void handle_decoration_click(BiomeToplevel *toplevel, biome_decoration::Region region);

// Shows/refreshes/hides the Alt-Tab switcher overlay to match
// server->switcher_active and the current server->toplevels order. Called
// after every Tab press (core/input.cpp's handle_keybinding) and on Alt
// release (keyboard_handle_modifiers). The window list and cycling order
// are exactly the existing MRU logic - this only adds a visual layer on
// top, it doesn't change which window Tab actually selects.
void update_switcher_overlay(BiomeServer *server);
