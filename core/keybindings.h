// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Keybinding dispatch: built-in bindings and GlobalShortcuts portal ones go
// through the same trigger parser and matcher (architecture-notes.md).
// Alt-Tab is handled separately (handle_switcher_key()).

#pragma once

#include "core/server.h"
#include "core/trigger.h"

#include <QString>

#include <xkbcommon/xkbcommon.h>

#include <functional>
#include <optional>

// Owns the portal bindings and held-key state (server->keys).
void keybindings_init(BiomeServer *server);
void keybindings_finish(BiomeServer *server);

struct BiomeKeybinding {
    ParsedTrigger trigger;
    // Returns true if it ran and the key event should be considered
    // handled (not forwarded to the focused client).
    std::function<bool(BiomeServer *, uint32_t modifiers)> invoke;
};

// Every key press. keyboard/keycode identify the physical key for the
// matching release; sym is translated, level0 unshifted.
bool handle_key_press(BiomeServer *server, wlr_keyboard *keyboard, uint32_t keycode,
    xkb_keysym_t sym, xkb_keysym_t level0, uint32_t modifiers);

// Fires the release of every binding still held on a keyboard being removed.
void release_keyboard_bindings(BiomeServer *server, wlr_keyboard *keyboard);

// Every key release: runs the release of a portal binding this key fired.
// True to swallow it, matching the swallowed press.
bool handle_key_release(BiomeServer *server, wlr_keyboard *keyboard, uint32_t keycode);

// Every key event: feeds server->keys->tap (ModifierTap, core/trigger.h) and
// fires a bare-modifier portal binding on a completed tap. consumed: Biome
// swallowed this press. True to swallow the event.
bool handle_modifier_tap(BiomeServer *server, xkb_keysym_t sym, uint32_t modifiers, bool pressed,
    bool consumed);

// Swallows a Tab release while the switcher is up, matching its swallowed press.
bool handle_switcher_key_release(BiomeServer *server, xkb_keysym_t sym);

// Portal bindings, keyed by owner (the session handle path). Built-ins win a
// collision. release runs when the key comes up (right after invoke for a
// tap) or when the session ends mid-hold.
void add_portal_keybinding(BiomeServer *server, const QString &owner, ParsedTrigger trigger,
    std::function<bool(BiomeServer *, uint32_t modifiers)> invoke,
    std::function<void()> release);
void remove_session_keybindings(BiomeServer *server, const QString &owner);

// True if a built-in or portal binding already uses this trigger.
bool trigger_in_use(BiomeServer *server, const ParsedTrigger &trigger);
