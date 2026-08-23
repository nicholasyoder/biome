// SPDX-License-Identifier: LGPL-3.0-or-later
//
// wlr-layer-shell-unstable-v1: lets a client (panel, wallpaper, launcher,
// OSD) place a surface in one of four fixed layers
// (background/bottom/top/overlay - see core/server.h's BiomeServer::layers
// doc comment for how these fit into the compositor's overall scene-layer
// stack) with an optional exclusive-zone strut reservation, anchors, and
// margins. wlroots' own wlr_scene_layer_surface_v1_configure() does all the
// anchor/margin/exclusive-zone box math per surface; this module's job is
// just resolving each surface's target output/layer and calling that
// helper, in order, for every mapped surface on a given output whenever
// something about the arrangement could have changed.

#pragma once

#include "core/server.h"

// Creates the wlr_layer_shell_v1 global and wires its new_surface listener.
void layer_shell_init(BiomeServer *server);

// Re-arranges every mapped layer surface on this output: iterates its four
// layers (overlay -> top -> bottom -> background, so a higher layer's
// exclusive-zone claim is resolved against the still-full area before a
// lower one's), calling wlr_scene_layer_surface_v1_configure() for each and
// leaving the leftover box in output->usable_area. Called on output
// add/resolution-change (core/output.cpp) and on any layer surface's own
// commit/map/unmap/destroy (this file).
void arrange_layers(BiomeOutput *output);
