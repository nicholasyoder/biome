// SPDX-License-Identifier: LGPL-3.0-or-later
//
// wlr-output-management-unstable-v1: lets clients (wlr-randr, kanshi) change
// output mode/scale/position/transform/enabled at runtime.

#pragma once

#include "core/server.h"

// Creates the manager global. Call after output_layout exists.
void output_management_init(BiomeServer *server);

// Republishes the current output state to clients on the next idle tick
// (coalesced). Call after any change to an output's existence, enabled state,
// mode, scale, transform or position.
void output_management_schedule_publish(BiomeServer *server);
