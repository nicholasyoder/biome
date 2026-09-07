// SPDX-License-Identifier: LGPL-3.0-or-later
//
// STOPGAP(idle-blank) - TEMPORARY. Hardcoded "blank all outputs after 10
// minutes of no keyboard/pointer input, wake on the next input" - nothing
// more (no lock screen, no protocol exposed to clients). This exists only
// because Biome has no display power management yet and the real design
// (ext-idle-notify-v1 driving a session-locker client, output power handled
// separately via wlr-output-management-unstable-v1/KScreen) is Phase 6 work
// that hasn't started - see docs/plan.md, "Phase 6 - New capabilities".
//
// DELETE THIS WHOLE THING once Phase 6 lands. Everything belonging to this
// stopgap is tagged with the exact string "STOPGAP(idle-blank)" so
// `grep -rn "STOPGAP(idle-blank)" biome/` finds every touch point:
//   - this file and idle_blank.cpp
//   - the idle_blank_timer/idle_blanked fields on BiomeServer (core/server.h)
//   - the idle_blank_init() call in core/main.cpp
//   - the idle_blank_notify_activity() calls in core/input.cpp and
//     core/cursor.cpp
//   - the idle_blank.cpp entry in core/CMakeLists.txt

#pragma once

#include "core/server.h"

// Arms the idle timer. Call once at startup, after output_manager_init()
// (needs server->outputs) has run.
void idle_blank_init(BiomeServer *server);

// Call from any keyboard/pointer input handler. Wakes the outputs
// immediately if they're currently blanked, and always resets the idle
// timer back to the full timeout.
void idle_blank_notify_activity(BiomeServer *server);
