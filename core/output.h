// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Output/monitor plumbing: the scene graph + output layout live here since
// they're created alongside the first output listener, and the per-output
// frame/request_state/destroy signal handlers.

#pragma once

#include "core/server.h"

#include <optional>
#include <utility>

// Creates output_layout, scene, and scene_layout, and wires the new_output
// listener. Must run before any other module that touches server->scene or
// server->output_layout.
void output_manager_init(BiomeServer *server);

// Finds the BiomeOutput wrapping a given wlr_output, or nullptr if it isn't
// (or isn't yet) one of server->outputs. A plain linear scan - only called
// from output-hotplug-rate code paths (a handful of times per session at
// most), not a hot path.
BiomeOutput *biome_output_from_wlr(BiomeServer *server, wlr_output *wlr_output);

// Adds/removes the output from output_layout (and its scene output) and
// syncs geometry. `position` nullopt = auto-arrange. Doesn't touch the wlr
// enabled state - callers commit that themselves. Disabling clears any
// pending session-lock frame wait.
void output_set_enabled(BiomeOutput *output, bool enabled,
                        std::optional<std::pair<int, int>> position = std::nullopt);

// Call once after a batch of output changes (apply, unplug, nested resize):
// moves layer surfaces off disabled outputs, pulls stranded windows back
// on-screen, and keeps the cursor inside the layout.
void output_layout_settled(BiomeServer *server);
