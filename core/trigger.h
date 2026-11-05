// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Shortcuts-spec trigger parsing/formatting, split from keybindings.h so it
// can be unit-tested without the compositor.

#pragma once

#include <QString>

#include <optional>

// Not core/wlroots.hpp: this only needs the WLR_MODIFIER_* constants, and the
// full wrapper drags in generated protocol headers the unit test lacks.
#ifndef WLR_USE_UNSTABLE
#define WLR_USE_UNSTABLE
#endif
extern "C" {
#include <wlr/types/wlr_keyboard.h>
}
#include <xkbcommon/xkbcommon.h>

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
// handle_modifier_tap() (keybindings.h), not trigger_matches().
struct ParsedTrigger {
    uint32_t modmask = 0;
    xkb_keysym_t keysym = XKB_KEY_NoSymbol;
};

std::optional<ParsedTrigger> parse_trigger(const QString &trigger);

// Human-readable form for the portal's `trigger_description`, e.g.
// "Ctrl+Alt+Return"; a bare-modifier trigger reads "Meta (tap)".
QString describe_trigger(const ParsedTrigger &trigger);
