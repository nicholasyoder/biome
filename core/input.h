// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Keyboard/seat plumbing: BiomeKeyboard signal handlers and new-input-
// device/seat-request wiring. Keybinding matching/dispatch itself (built-in
// compositor actions plus portal-registered shortcuts) lives in
// core/keybindings.h - see handle_key_press().

#pragma once

#include "core/server.h"

struct BiomeToplevel;

void input_init(BiomeServer *server);

// Removes toplevel from the Alt-Tab switcher's frozen MRU snapshot
// (server->switcher_order), if present, and keeps switcher_preview_index/
// switcher_active consistent with the shorter list - called from both
// toplevel destroy handlers before free(). A toplevel can be destroyed
// (client crash, kill, self-close) while the switcher is holding it in its
// snapshot from an in-progress Alt-hold; without this, update_switcher_overlay
// and the Alt-release commit in keyboard_handle_modifiers would dereference a
// freed pointer.
void remove_toplevel_from_switcher(BiomeServer *server, BiomeToplevel *toplevel);
