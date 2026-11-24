// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Touchscreens (server->touch): output mapping, native wl_touch routing, and
// pointer emulation for whatever can't take touch.

#pragma once

#include "core/server.h"

void touch_init(BiomeServer *server);
void touch_finish(BiomeServer *server);

void touch_new_device(BiomeServer *server, wlr_input_device *device);
bool touch_has_devices(BiomeServer *server);

// The newest native point down on root's surface tree, or -1.
int32_t touch_point_on(BiomeServer *server, wlr_surface *root);
// Makes point id drive the cursor for a move/resize grab about to begin.
void touch_begin_grab(BiomeServer *server, int32_t id);
// Skips an open tap-to-identify prompt (Esc, or any pointer press); true if one was open.
bool touch_identify_dismiss(BiomeServer *server);

bool touch_point_position(BiomeServer *server, int32_t id, double *lx, double *ly);
