// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/trigger.h"

#include <QByteArray>
#include <QStringList>

std::optional<ParsedTrigger> parse_trigger(const QString &trigger) {
    const QStringList parts = trigger.split('+', Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return std::nullopt;
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
    xkb_keysym_t keysym = xkb_keysym_from_name(key_name.constData(), XKB_KEYSYM_NO_FLAGS);
    if (keysym == XKB_KEY_NoSymbol) {
        return std::nullopt;
    }

    return ParsedTrigger{modmask, keysym};
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
