// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Output/monitor plumbing: the scene graph + output layout live here since
// they're created alongside the first output listener, and the per-output
// frame/request_state/destroy signal handlers.

#pragma once

#include "core/server.h"

// Creates output_layout, scene, and scene_layout, and wires the new_output
// listener. Must run before any other module that touches server->scene or
// server->output_layout.
void output_manager_init(BiomeServer *server);
