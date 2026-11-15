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

    return failures == 0 ? 0 : 1;
}
