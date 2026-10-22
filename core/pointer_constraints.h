// SPDX-License-Identifier: LGPL-3.0-or-later
//
// relative-pointer-unstable-v1 + pointer-constraints-unstable-v1. A
// constraint is active only while its surface's root has keyboard focus, so
// anything that moves focus (Alt+Tab, launcher, session lock) releases it.

#pragma once

#include "core/server.h"

// After input_init(): needs the seat.
void pointer_constraints_init(BiomeServer *server);

// Clamps a pending cursor delta to the active constraint.
void pointer_constraints_clamp_motion(BiomeServer *server, double *dx, double *dy);
