// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/trigger.h"

#include <cstdio>

namespace {

int failures = 0;

void expect(bool condition, const char *what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

QString roundtrip(const char *trigger) {
    const auto parsed = parse_trigger(QString::fromLatin1(trigger));
    return parsed ? describe_trigger(*parsed) : QStringLiteral("<unparseable>");
}

// Feeds (sym, pressed, consumed) events, tracking the modifier mask the way
// wlroots reports it (before the event is applied).
struct TapSequence {
    ModifierTap tap;
    uint32_t mods = 0;
    ModifierTap::Result key(xkb_keysym_t sym, bool pressed, bool consumed = false) {
        ModifierTap::Result r = tap.step(sym, mods, pressed, consumed);
        uint32_t bit = modifier_bit_for_keysym(sym);
        mods = pressed ? (mods | bit) : (mods & ~bit);
        return r;
    }
};

void test_modifier_tap() {
    {
        TapSequence s;
        expect(s.key(XKB_KEY_Super_L, true).fire == 0, "tap: press doesn't fire");
        expect(s.key(XKB_KEY_Super_L, false).fire == WLR_MODIFIER_LOGO, "tap: release fires");
    }
    {
        TapSequence s;
        s.key(XKB_KEY_Super_L, true);
        s.key(XKB_KEY_c, true);
        s.key(XKB_KEY_c, false);
        ModifierTap::Result r = s.key(XKB_KEY_Super_L, false);
        expect(r.fire == 0 && !r.swallow, "combo: forwarded key, release forwarded");
    }
    {
        TapSequence s;
        s.key(XKB_KEY_Alt_L, true);
        s.key(XKB_KEY_Tab, true, true);
        s.key(XKB_KEY_Tab, false);
        ModifierTap::Result r = s.key(XKB_KEY_Alt_L, false);
        expect(r.fire == 0 && r.swallow, "combo: swallowed key, release swallowed");
    }
    {
        TapSequence s;
        s.key(XKB_KEY_Control_L, true);
        s.key(XKB_KEY_Shift_L, true);
        expect(s.key(XKB_KEY_Shift_L, false).fire == 0, "two modifiers: first release");
        expect(s.key(XKB_KEY_Control_L, false).fire == 0, "two modifiers: no tap");
        s.key(XKB_KEY_Control_L, true);
        expect(s.key(XKB_KEY_Control_L, false).fire == WLR_MODIFIER_CTRL, "rearmed after a combo");
    }
    {
        TapSequence s;
        expect(s.key(XKB_KEY_a, false).fire == 0, "non-modifier release ignored");
    }
}

} // namespace

int main() {
    expect(roundtrip("CTRL+ALT+Return") == "Ctrl+Alt+Return", "chord");
    expect(roundtrip("LOGO+SHIFT+CTRL+f") == "Ctrl+Shift+Meta+f", "modifier order is fixed");
    expect(roundtrip("F11") == "F11", "bare key");
    expect(roundtrip("LOGO") == "Meta (tap)", "bare-modifier tap");
    expect(roundtrip("NUM+KP_1") == "Num+KP_1", "num modifier");
    expect(!parse_trigger(QStringLiteral("BOGUS+a")), "unknown modifier rejected");
    expect(!parse_trigger(QString()), "empty rejected");

    expect(!parse_trigger(QStringLiteral("CTRL+")), "trailing + rejected");
    expect(!parse_trigger(QStringLiteral("CTRL++a")), "empty middle part rejected");
    expect(parse_trigger(QStringLiteral("CTRL+A"))->keysym == XKB_KEY_a, "keysym lowercased");

    const auto m = [](const char *t, xkb_keysym_t translated, xkb_keysym_t level0, uint32_t mods) {
        return trigger_matches(*parse_trigger(QString::fromLatin1(t)), translated, level0, mods);
    };
    expect(m("LOGO+l", XKB_KEY_L, XKB_KEY_l, WLR_MODIFIER_LOGO | WLR_MODIFIER_CAPS), "caps lock letter");
    expect(m("CTRL+SHIFT+a", XKB_KEY_A, XKB_KEY_a, WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT), "ctrl+shift+letter");
    expect(m("SHIFT+exclam", XKB_KEY_exclam, XKB_KEY_1, WLR_MODIFIER_SHIFT), "translated sym");
    expect(m("SHIFT+1", XKB_KEY_exclam, XKB_KEY_1, WLR_MODIFIER_SHIFT), "level-0 sym");
    expect(m("SHIFT+Tab", XKB_KEY_ISO_Left_Tab, XKB_KEY_Tab, WLR_MODIFIER_SHIFT), "shift+tab");
    expect(!m("CTRL+a", XKB_KEY_a, XKB_KEY_a, WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT), "extra modifier");
    expect(!m("CTRL+a", XKB_KEY_b, XKB_KEY_b, WLR_MODIFIER_CTRL), "wrong key");
    expect(!m("LOGO", XKB_KEY_NoSymbol, XKB_KEY_NoSymbol, WLR_MODIFIER_LOGO), "tap never key-matches");

    const auto allowed = [](const char *t) { return trigger_allowed(*parse_trigger(QString::fromLatin1(t))); };
    expect(!allowed("a"), "bare letter refused");
    expect(!allowed("SHIFT+a"), "shift-only refused");
    expect(allowed("CTRL+a"), "ctrl letter ok");
    expect(allowed("F11"), "F-key ok");
    expect(allowed("XF86AudioMute"), "XF86 ok");
    expect(allowed("Print"), "Print ok");
    expect(allowed("LOGO"), "tap ok");

    test_modifier_tap();

    return failures == 0 ? 0 : 1;
}
