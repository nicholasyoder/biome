// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Keyboard/seat plumbing: BiomeKeyboard signal handlers, the Alt-held
// keybinding table (workspace switch, Alt-Tab, close, VT switch), and new-
// input-device/seat-request wiring.

#pragma once

#include "core/server.h"

void input_init(BiomeServer *server);
