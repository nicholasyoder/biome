// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Window decorations between the scene graph and decoration/'s Qt widgets:
// metrics, rendering, hit-testing and button hover/press/click.

#pragma once

#include "core/server.h"
#include "decoration/layout.h" // biome_decoration::Region
#include "desktop/toplevel.h"

#include <QString>

// Edge sizes from the theme (0 if undecorated). maximized: which
// [biomeMaximized] state, i.e. the current or target one.
int decoration_border_width(const BiomeToplevel *toplevel, bool maximized);
int decoration_titlebar_height(const BiomeToplevel *toplevel, bool maximized);
int decoration_border_right_width(const BiomeToplevel *toplevel, bool maximized);
int decoration_border_bottom_height(const BiomeToplevel *toplevel, bool maximized);

// The frame widget and an empty decoration_buffer node in scene_tree.
void create_toplevel_decoration(BiomeToplevel *toplevel);

// Call before scene_tree is destroyed.
void destroy_toplevel_decoration(BiomeToplevel *toplevel);

// Re-renders after a size, focus, title, icon or hover/press change.
void render_toplevel_decoration(BiomeToplevel *toplevel);

// Applies theme `name` from `dir` (empty = built-in) to the running compositor:
// restyles, then re-lays-out and re-renders every toplevel's frame, keeping
// each window's content in place and re-fitting maximized/fullscreen ones.
void reload_decoration_theme(BiomeServer *server, const QString &dir, const QString &name);

// For a node from scene_node_at (desktop/toplevel.h).
BiomeToplevel *decoration_toplevel_at_node(
    wlr_scene_node *node, double sx, double sy, biome_decoration::Region *out_region);

// The toplevel whose decoration is topmost at the point; null (region
// untouched) if none.
BiomeToplevel *decoration_toplevel_at(
    BiomeServer *server, double lx, double ly, biome_decoration::Region *out_region);

// Moves the :hover state; null toplevel or a non-button region = none.
void update_decoration_hover(BiomeServer *server, BiomeToplevel *toplevel,
    biome_decoration::Region region);

// Same for :pressed.
void set_decoration_pressed(BiomeServer *server, BiomeToplevel *toplevel,
    biome_decoration::Region region);

// Re-syncs hover after geometry changed under a still pointer (button
// release, a deferred reposition landing).
void refresh_decoration_hover(BiomeServer *server);

// Frees the press-and-hold timer at teardown.
void decoration_finish(BiomeServer *server);

// Drops server->decor's (dereferenced) pointers to a dying toplevel.
void clear_decoration_tracking(BiomeServer *server, BiomeToplevel *toplevel);

// Opens the snap-layout menu (window_menu_open_snap) under the pointer, hanging
// off the titlebar. armed: the button is still held, so releasing over an item picks it.
void open_snap_menu(BiomeToplevel *toplevel, bool armed);

const char *resize_cursor_name(biome_decoration::Region region);

// Left press: titlebar/resize regions start their grab now; buttons only arm
// (they act on a matching release).
void handle_decoration_press(BiomeToplevel *toplevel, biome_decoration::Region region);

// Left release, before the pressed state is cleared: fires an armed button
// if released over the same toplevel and region.
void handle_decoration_release(BiomeServer *server);
