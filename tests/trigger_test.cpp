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

    return failures == 0 ? 0 : 1;
}
