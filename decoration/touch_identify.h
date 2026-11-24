// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Tap-to-identify prompt pieces (desktop/touch_identify.h), painted directly:
// rare, transient UI, so no QSS selectors for themes to track.

#pragma once

#include "renderer.h" // RenderedFrame

namespace biome_decoration {

// Concentric-ring target; its center is the frame's center.
RenderedFrame render_touch_target(double scale);

// Centered instructions naming the device.
RenderedFrame render_touch_prompt(const char *device_name, double scale);

} // namespace biome_decoration
