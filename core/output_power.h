// SPDX-License-Identifier: LGPL-3.0-or-later
//
// wlr-output-power-management-unstable-v1 (DPMS): a client (swayidle+wlopm,
// Forest's locker) turns outputs off/on. Powered-off outputs keep their layout
// place; only the wlr enabled state changes.

#pragma once

#include "core/server.h"

// Creates the manager global.
void output_power_init(BiomeServer *server);

// Powers every powered-off output back on; a modeset on an off DRM
// connector fails, so output-management calls this before applying.
void output_power_wake_all(BiomeServer *server);

// Call from output_destroy before freeing the output.
void output_power_handle_output_destroy(BiomeOutput *output);

// True if this connector was destroyed while powered off moments ago (an HPD
// bounce), so it should come back off. Consumes the record.
bool output_power_reconnected_off(BiomeServer *server, const char *name);
