// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Unified keybinding dispatch: one trigger-matching mechanism shared by
// Biome's own fixed compositor actions (workspace switch, quit - a
// compiled-in default list) and shortcuts dynamically registered at
// runtime via ipc/global_shortcuts_portal.cpp's
// org.freedesktop.impl.portal.GlobalShortcuts implementation. Both kinds
// are matched the same way, through the same trigger parser - see
// docs/architecture-notes.md's "GlobalShortcuts portal architecture
// (Workstream C)" section for why (a real Forest-configured
// hotkey, once that lands, is just another entry registered through the
// portal path, layered on top of these compiled-in defaults, not a
// separate mechanism).
//
// The Alt-Tab/Alt-Shift-Tab switcher is deliberately NOT one of these
// table entries - see handle_switcher_key() in keybindings.cpp.

#pragma once

#include "core/server.h"
#include "core/trigger.h"

#include <QString>

#include <xkbcommon/xkbcommon.h>

#include <functional>
#include <optional>

struct BiomeKeybinding {
    ParsedTrigger trigger;
    // Returns true if it ran and the key event should be considered
    // handled (not forwarded to the focused client).
    std::function<bool(BiomeServer *, uint32_t modifiers)> invoke;
};

// The single entry point called from core/input.cpp's keyboard_handle_key()
// for every key press, regardless of which modifiers are held - portal-
// registered shortcuts aren't restricted to Alt-chords. `keyboard`/`keycode`
// identify the physical key, so a portal binding's release can be matched.
bool handle_key_press(BiomeServer *server, wlr_keyboard *keyboard, uint32_t keycode,
    xkb_keysym_t sym, uint32_t modifiers);

// Called from keyboard_handle_key() for every key release. Runs the release
// callback of a portal binding this key triggered (modifiers don't matter,
// only the triggering key). Returns true to swallow the release, matching
// the swallowed press.
bool handle_key_release(wlr_keyboard *keyboard, uint32_t keycode);

// Also called from keyboard_handle_key(), but for every key event - press
// AND release alike (handle_key_press() above is press-only), since a bare-
// modifier trigger fires on release. `modifiers` is the already-updated
// mask (xkb state reflects this exact event by the time wlroots delivers
// it). Mirrors the old X11 XGrabKey-era logic (forest's pre-port
// hotkey.cpp: XCB_KEY_RELEASE + lastkeypressed) generalized to any single
// recognized modifier: arms a tap candidate the instant exactly one
// matchable modifier is held alone, drops it (without firing) if any other
// key goes down meanwhile, and fires the matching portal-registered
// bare-modifier binding on release of that same lone modifier if nothing
// interrupted it. `consumed`: Biome already swallowed this press. Returns
// true if the release should be considered handled (not forwarded to the
// focused client).
bool handle_modifier_tap(BiomeServer *server, xkb_keysym_t sym, uint32_t modifiers, bool pressed,
    bool consumed);

// Called from keyboard_handle_key() for every key release. Swallows a
// Tab/ISO_Left_Tab release while the Alt-Tab switcher is active, mirroring
// handle_switcher_key()'s press-side swallow - the release would otherwise
// have no matching press-forward to pair with on the client that's still
// focused at that point (the press was already swallowed), leaking a
// spurious unmatched key event. Returns true if the release should be
// considered handled (not forwarded to the focused client).
bool handle_switcher_key_release(BiomeServer *server, xkb_keysym_t sym);

// Registration API for ipc/global_shortcuts_portal.cpp. `owner` is an
// opaque per-session key (the portal's session_handle object path) used
// only to remove every binding for that session again.
// Built-in bindings always take priority over a portal-registered one on an
// exact-trigger collision - see handle_key_press()'s own comment on that choice.
// `release` runs when the triggering key comes up (straight after `invoke`
// for a bare-modifier tap), or when the session ends mid-hold.
void add_portal_keybinding(const QString &owner, ParsedTrigger trigger,
    std::function<bool(BiomeServer *, uint32_t modifiers)> invoke,
    std::function<void()> release);
void remove_session_keybindings(const QString &owner);

// True if a built-in or portal binding already uses this trigger.
bool trigger_in_use(const ParsedTrigger &trigger);
