// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Creates the six fixed, persistent scene trees making up
// BiomeServer::layers - see that field's doc comment in server.h for the
// stacking order and why it exists.

#pragma once

#include "core/server.h"

// Must run after server->scene exists (output_manager_init creates it right
// before calling this) and before any output/toplevel/layer-shell/
// session-lock code touches server->layers.
void scene_layers_init(BiomeServer *server);
