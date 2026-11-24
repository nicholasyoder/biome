// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Tap-to-identify overlay for a touchscreen core/touch.cpp can't place: a scrim,
// prompt and target on each output, the target at that output's slot
// (touch_identify_slot()). Input handling stays in core/touch.cpp.

#pragma once

#include "core/server.h"

#include <vector>

// Replaces any shown overlay; outputs[i] gets slot i.
void touch_identify_show(BiomeServer *server, const std::vector<wlr_output *> &outputs, const char *device_name);
void touch_identify_hide(BiomeServer *server);
