// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Creates BiomeServer::layers, the fixed scene-tree stack (order in server.h).

#pragma once

#include "core/server.h"

// Called by output_manager_init right after it creates the scene.
void scene_layers_init(BiomeServer *server);
