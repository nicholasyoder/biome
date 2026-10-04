// SPDX-License-Identifier: LGPL-3.0-or-later
//
// STOPGAP(idle-blank) - TEMPORARY. Hardcoded "blank all outputs after 10
// minutes of no keyboard/pointer input (or visible idle inhibitor), wake on
// the next input". Blanking belongs to a client (Forest's locker) driving
// wlr-output-power-management-unstable-v1, which Biome doesn't have yet.
//
// DELETE THIS WHOLE THING when output-power lands (docs/roadmap.md).
// `grep -rn "STOPGAP(idle-blank)" biome/` finds every touch point:
//   - this file and idle_blank.cpp
//   - the idle_blank_* / idle_blanked fields on BiomeServer (core/server.h)
//   - the idle_blank_init() call in core/main.cpp
//   - the idle_blank_notify_activity() calls in core/input.cpp,
//     core/cursor.cpp and core/output_management.cpp
//   - the idle_blank.cpp entry in core/CMakeLists.txt
//   - the idle_blanked check in core/output.cpp's server_new_output()

#pragma once

#include "core/server.h"

// Arms the idle timer. Call once at startup, after output_manager_init()
// (needs server->outputs) has run.
void idle_blank_init(BiomeServer *server);

// Call from any keyboard/pointer input handler. Wakes the outputs
// immediately if they're currently blanked, and always resets the idle
// timer back to the full timeout.
void idle_blank_notify_activity(BiomeServer *server);
