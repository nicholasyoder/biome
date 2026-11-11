// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Sets the DRM "Broadcast RGB" connector property from the output's rgb_range.

#pragma once

#include "core/output_config.h"
#include "core/server.h"

// No-op on non-DRM backends or connectors without the property.
void output_apply_rgb_range(BiomeServer *server, wlr_output *output, OutputConfig::RgbRange range);
