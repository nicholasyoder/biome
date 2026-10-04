// SPDX-License-Identifier: LGPL-3.0-or-later
//
// ext-idle-notify-v1 + idle-inhibit-unstable-v1. An inhibitor only counts
// while its surface is visible on some output - see docs/architecture-notes.md.

#pragma once

#include "core/server.h"

// Creates both globals.
void idle_init(BiomeServer *server);

// Call from every keyboard/pointer input handler.
void idle_notify_activity(BiomeServer *server);

// Re-evaluates inhibitor visibility. Called on every output frame: anything
// that shows or hides an inhibiting surface damages an output.
void idle_update_inhibited(BiomeServer *server);
