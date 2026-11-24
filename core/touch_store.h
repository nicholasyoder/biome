// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Tap-to-identify answers, per touch device name, in
// $XDG_STATE_HOME/biome/touch.conf: Biome's own state, not user config.

#pragma once

#include "core/touch_map.h"

TouchRemembered touch_store_load(const char *device_name);
void touch_store_save(const char *device_name, const TouchRemembered &answer);
