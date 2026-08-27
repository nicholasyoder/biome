// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Unified keybinding dispatch: one trigger-matching mechanism shared by
// Biome's own fixed compositor actions (workspace switch, quit - a
// compiled-in default list) and shortcuts dynamically registered at
// runtime via ipc/global_shortcuts_portal.cpp's
// org.freedesktop.impl.portal.GlobalShortcuts implementation. Both kinds
// are matched the same way, through the same trigger parser - see
// docs/phase4-plan.md's Workstream C for why (a real Forest-configured
// hotkey, once that lands, is just another entry registered through the
// portal path, layered on top of these compiled-in defaults, not a
// separate mechanism).
//
// The Alt-Tab/Alt-Shift-Tab switcher is deliberately NOT one of these
// table entries - see handle_key_press()'s own comment for why.

#pragma once

#include "core/server.h"

#include <QString>

#include <xkbcommon/xkbcommon.h>

#include <functional>
#include <optional>

// A trigger parsed from the shortcuts-spec grammar
// (https://specifications.freedesktop.org/shortcuts/latest/): modifiers
// CTRL/ALT/SHIFT/NUM/LOGO (matched against WLR_MODIFIER_*), '+'-joined,
// followed by a key name resolved via xkb_keysym_from_name() - e.g.
// "CTRL+ALT+Return". NUM parses successfully (WLR_MODIFIER_MOD2) but isn't
// currently honored at match time - see keybindings.cpp's
// kMatchableModifierMask comment.
//
// keysym == XKB_KEY_NoSymbol is a sentinel for a bare-modifier ("tap")
// trigger - a trigger string that's just a single recognized modifier name
// on its own, e.g. "LOGO" for forest's Meta-only "Show menu" binding, which
// the shortcuts-spec grammar has no dedicated syntax for. Matched by
// handle_modifier_tap() below, not trigger_matches().
struct ParsedTrigger {
    uint32_t modmask = 0;
    xkb_keysym_t keysym = XKB_KEY_NoSymbol;
};

std::optional<ParsedTrigger> parse_trigger(const QString &trigger);

struct BiomeKeybinding {
    ParsedTrigger trigger;
    // Returns true if it ran and the key event should be considered
    // handled (not forwarded to the focused client).
    std::function<bool(BiomeServer *, uint32_t modifiers)> invoke;
};

// The single entry point called from core/input.cpp's keyboard_handle_key()
// for every key press, regardless of which modifiers are held (unlike the
// old Alt-only-gated handle_keybinding() this replaces - portal-registered
// shortcuts aren't restricted to Alt-chords).
bool handle_key_press(BiomeServer *server, xkb_keysym_t sym, uint32_t modifiers);

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
// interrupted it. Returns true if a tap fired and the release should be
// considered handled (not forwarded to the focused client).
bool handle_modifier_tap(BiomeServer *server, xkb_keysym_t sym, uint32_t modifiers, bool pressed);

// Registration API for ipc/global_shortcuts_portal.cpp. `owner` is an
// opaque per-session key (the portal's session_handle object path) used
// only to remove every binding for that session again in
// remove_session_keybindings() once its session ends. Built-in bindings
// always take priority over a portal-registered one on an exact-trigger
// collision - see handle_key_press()'s own comment on that choice.
void add_portal_keybinding(const QString &owner, ParsedTrigger trigger,
    std::function<bool(BiomeServer *, uint32_t modifiers)> invoke);
void remove_session_keybindings(const QString &owner);
