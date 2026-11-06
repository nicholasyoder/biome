// SPDX-License-Identifier: LGPL-3.0-or-later
//
// wlr-gamma-control-unstable-v1: clients (wlsunset, gammastep) set per-output
// gamma ramps for night-light. wlroots applies the ramp via the DRM gamma LUT.

#pragma once

#include "core/server.h"

// Creates the manager global.
void gamma_init(BiomeServer *server);
