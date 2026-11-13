// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Compositor-side window menu (restore/maximize, minimize, move, resize,
// always on top, move to workspace, close), drawn by decoration/menu.cpp.
// Modal while open: pointer and keyboard input are consumed here (hooks in
// core/cursor.cpp and core/input.cpp). Only one menu exists at a time.

#pragma once

#include "desktop/toplevel.h"

// Creates the hidden panel scene nodes. Called once at startup.
void window_menu_init(BiomeServer *server);

// Opens the menu for toplevel with its top-left at (x, y) in layout
// coordinates, flipping/clamping to stay on the output. anchor_height is the
// height of the thing it hangs off (0 for a click point), so a flip above
// lands flush against it.
void window_menu_open(BiomeToplevel *toplevel, int x, int y, int anchor_height = 0);

void window_menu_close(BiomeServer *server);
bool window_menu_active();

// Must run before a toplevel is freed or leaves the active workspace.
void window_menu_remove_toplevel(BiomeServer *server, BiomeToplevel *toplevel);

// Pointer hooks; call only while window_menu_active().
void window_menu_motion(BiomeServer *server, double lx, double ly);
// Returns true if consumed (always, while active).
bool window_menu_button(BiomeServer *server, uint32_t time_msec, uint32_t button, bool pressed);

// Returns true if the key event was consumed. Modifier keys are never consumed.
bool window_menu_key(BiomeServer *server, uint32_t keycode, xkb_keysym_t sym, bool pressed);
