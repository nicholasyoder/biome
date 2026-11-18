// SPDX-License-Identifier: LGPL-3.0-or-later
//
// wlr-layer-shell-unstable-v1 (panels, wallpaper, OSDs). wlroots does the
// per-surface box math; this module picks each surface's output and layer
// tree and re-arranges an output whenever its layout may have changed.

#pragma once

#include "core/server.h"

void layer_shell_init(BiomeServer *server);
void layer_shell_finish(BiomeServer *server);

// The mapped top/overlay `exclusive` surface that must hold keyboard focus
// (overlay first, then newest), or nullptr. Always nullptr while locked.
wlr_surface *layer_shell_exclusive_focus(BiomeServer *server);

// Gives layer_shell_exclusive_focus() keyboard focus unless its client
// already holds it. Returns false when there's no such surface.
bool layer_shell_claim_exclusive_focus(BiomeServer *server);

// Configures exclusive-zone surfaces first (overlay -> background), then the
// rest, leaving the remainder in output->usable_area.
void arrange_layers(BiomeOutput *output);

// Steps this output's fades (core/fade_config.h). True while any is still
// running: the caller must schedule another frame.
bool update_layer_surface_fades(BiomeOutput *output);

// Closes every layer surface bound to this output (`closed` event) and drops
// its in-flight fades. Called when the output is disabled or destroyed (the
// latter before it is freed, or a surface would dangle on it).
void layer_shell_close_output_surfaces(BiomeOutput *output);
