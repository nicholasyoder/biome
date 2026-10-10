// SPDX-License-Identifier: LGPL-3.0-or-later
//
// xdg-activation-v1: honors a token only while the user hasn't moved on;
// everything else marks the window urgent. Policy in docs/architecture-notes.md.

#pragma once

#include "core/server.h"

void xdg_activation_init(BiomeServer *server);

// Call for every button/key press. recipient is the client it went to, or
// null for Biome itself (consumed hotkeys, clicks on nothing).
void xdg_activation_note_press(BiomeServer *server, wl_client *recipient);
