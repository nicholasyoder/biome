// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Keyboard/seat plumbing: BiomeKeyboard signal handlers and new-input-
// device/seat-request wiring. Keybinding matching/dispatch itself (built-in
// compositor actions plus portal-registered shortcuts) lives in
// core/keybindings.h - see handle_key_press().

#pragma once

#include "core/server.h"

void input_init(BiomeServer *server);
