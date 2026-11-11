// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Forces the DRM "Broadcast RGB" connector property to Full. "Automatic"
// sends limited range (16-235) for TV-style timings such as 1920x1080@60,
// which many PC monitors then show washed out.

#pragma once

#include "core/server.h"

// No-op on non-DRM backends or connectors without the property.
void output_set_full_range(BiomeServer *server, wlr_output *output);
