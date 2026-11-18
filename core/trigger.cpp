// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/trigger.h"

#include <QByteArray>
#include <QStringList>

std::optional<ParsedTrigger> parse_trigger(const QString &trigger) {
    const QStringList parts = trigger.split('+');
    for (const QString &part : parts) {
        if (part.isEmpty()) {
            return std::nullopt;
        }
    }

    // A trigger that's just a single recognized modifier name on its own
    // (no other modifiers, no key) is a bare-tap trigger - e.g. forest's
    // Meta-only "Show menu" binding, which XGrabKey could express directly
    // but the shortcuts-spec grammar has no dedicated syntax for.
    // XKB_KEY_NoSymbol is the sentinel handle_modifier_tap() checks for.
    if (parts.size() == 1) {
        if (parts[0] == "CTRL") {
            return ParsedTrigger{WLR_MODIFIER_CTRL, XKB_KEY_NoSymbol};
        } else if (parts[0] == "ALT") {
            return ParsedTrigger{WLR_MODIFIER_ALT, XKB_KEY_NoSymbol};
        } else if (parts[0] == "SHIFT") {
            return ParsedTrigger{WLR_MODIFIER_SHIFT, XKB_KEY_NoSymbol};
        } else if (parts[0] == "LOGO") {
            return ParsedTrigger{WLR_MODIFIER_LOGO, XKB_KEY_NoSymbol};
        } else if (parts[0] == "NUM") {
            return ParsedTrigger{WLR_MODIFIER_MOD2, XKB_KEY_NoSymbol};
        }
    }

    uint32_t modmask = 0;
    for (qsizetype i = 0; i < parts.size() - 1; i++) {
        const QString &mod = parts[i];
        if (mod == "CTRL") {
            modmask |= WLR_MODIFIER_CTRL;
        } else if (mod == "ALT") {
            modmask |= WLR_MODIFIER_ALT;
        } else if (mod == "SHIFT") {
            modmask |= WLR_MODIFIER_SHIFT;
        } else if (mod == "LOGO") {
            modmask |= WLR_MODIFIER_LOGO;
        } else if (mod == "NUM") {
            modmask |= WLR_MODIFIER_MOD2;
        } else {
            return std::nullopt;
        }
    }

    const QByteArray key_name = parts.last().toUtf8();
    xkb_keysym_t keysym = xkb_keysym_from_name(key_name.constData(), XKB_KEYSYM_CASE_INSENSITIVE);
    if (keysym == XKB_KEY_NoSymbol) {
        return std::nullopt;
    }

    return ParsedTrigger{modmask, xkb_keysym_to_lower(keysym)};
}

bool trigger_matches(const ParsedTrigger &trigger, xkb_keysym_t translated, xkb_keysym_t level0,
        uint32_t modifiers) {
    if (trigger.keysym == XKB_KEY_NoSymbol
        || trigger.modmask != (modifiers & kMatchableModifierMask)) {
        return false;
    }
    return trigger.keysym == xkb_keysym_to_lower(translated)
        || trigger.keysym == xkb_keysym_to_lower(level0);
}

bool trigger_allowed(const ParsedTrigger &trigger) {
    if (trigger.keysym == XKB_KEY_NoSymbol
        || (trigger.modmask & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO))) {
        return true;
    }
    const xkb_keysym_t sym = trigger.keysym;
    return (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F35) || (sym >= 0x1008FF00 && sym <= 0x1008FFFF)
        || sym == XKB_KEY_Print || sym == XKB_KEY_Pause || sym == XKB_KEY_Break
        || sym == XKB_KEY_Sys_Req;
}

QString describe_trigger(const ParsedTrigger &trigger) {
    QStringList parts;
    if (trigger.modmask & WLR_MODIFIER_CTRL) {
        parts << QStringLiteral("Ctrl");
    }
    if (trigger.modmask & WLR_MODIFIER_ALT) {
        parts << QStringLiteral("Alt");
    }
    if (trigger.modmask & WLR_MODIFIER_SHIFT) {
        parts << QStringLiteral("Shift");
    }
    if (trigger.modmask & WLR_MODIFIER_LOGO) {
        parts << QStringLiteral("Meta");
    }
    if (trigger.modmask & WLR_MODIFIER_MOD2) {
        parts << QStringLiteral("Num");
    }

    if (trigger.keysym == XKB_KEY_NoSymbol) {
        return parts.join('+') + QStringLiteral(" (tap)");
    }

    char name[64];
    if (xkb_keysym_get_name(trigger.keysym, name, sizeof(name)) > 0) {
        parts << QString::fromUtf8(name);
    }
    return parts.join('+');
}

uint32_t modifier_bit_for_keysym(xkb_keysym_t sym) {
    switch (sym) {
    case XKB_KEY_Shift_L:
    case XKB_KEY_Shift_R:
        return WLR_MODIFIER_SHIFT;
    case XKB_KEY_Control_L:
    case XKB_KEY_Control_R:
        return WLR_MODIFIER_CTRL;
    case XKB_KEY_Alt_L:
    case XKB_KEY_Alt_R:
        return WLR_MODIFIER_ALT;
    case XKB_KEY_Super_L:
    case XKB_KEY_Super_R:
        return WLR_MODIFIER_LOGO;
    default:
        return 0;
    }
}

ModifierTap::Result ModifierTap::step(xkb_keysym_t sym, uint32_t modifiers, bool pressed, bool consumed) {
    // wlroots emits the key before applying it to xkb_state, so patch in this key's own bit.
    const uint32_t own_bit = modifier_bit_for_keysym(sym);
    uint32_t mods = modifiers & kMatchableModifierMask;
    if (own_bit != 0) {
        mods = pressed ? (mods | own_bit) : (mods & ~own_bit);
    }

    if (pressed) {
        if (own_bit != 0 && mods != 0 && (mods & (mods - 1)) == 0) {
            // This modifier's own press, held alone: (re)arm.
            *this = ModifierTap{mods, false, false};
        } else if (candidate != 0) {
            // Another key or modifier mid-hold: a combo, not a tap.
            interrupted = true;
            forwarded |= !consumed;
        }
        return {};
    }
    if (own_bit == 0) {
        return {};
    }

    const ModifierTap held = *this;
    if (mods == 0) {
        *this = ModifierTap{}; // the hold is over, fired or not
    }
    if (held.candidate != 0 && !held.interrupted && mods == 0) {
        return {held.candidate, false};
    }
    // If every interrupting key was swallowed (e.g. Alt+Tab) the client saw a
    // lone modifier press; swallow its release too so it doesn't read a tap.
    return {0, held.candidate != 0 && held.interrupted && !held.forwarded && mods == 0};
}
