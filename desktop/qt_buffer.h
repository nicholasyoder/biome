// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Qt-rendered frames (decoration/renderer.h) as wlr_scene buffers.

#pragma once

#include "core/wlroots.hpp"
#include "decoration/renderer.h"

// Shows frame's pixels at its logical size. Returns false, leaving
// scene_buffer untouched, for an empty frame.
bool set_scene_frame(wlr_scene_buffer *scene_buffer, biome_decoration::RenderedFrame &&frame);

// Compositor UI: sets the frame, then positions, enables and raises the node;
// an empty frame hides it instead. Returns whether it is shown.
bool osd_node_show(wlr_scene_buffer *node, biome_decoration::RenderedFrame &&frame, int x, int y);
