// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/touch_map.h"

#include <cstdio>

namespace {

int failures = 0;

void expect(bool condition, const char *what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

const TouchOutputCandidate laptop = {"eDP-1", 309, 174};
const TouchOutputCandidate monitor = {"DP-1", 527, 296};
const TouchOutputCandidate small = {"HDMI-A-1", 155, 87}; // 7" panel

TouchOutputCandidate edid(const char *connector, const char *make, const char *model, const char *serial) {
    TouchOutputCandidate c;
    c.name = connector;
    c.make = make;
    c.model = model;
    c.serial = serial;
    return c;
}

void test_remembered() {
    const auto msi = edid("DP-1", "MSI", "MP273U", "PB4HB15C00097");
    const auto msi_moved = edid("DP-3", "MSI", "MP273U", "PB4HB15C00097");
    const auto generic = edid("HDMI-A-1", "ADA", "", "");
    const auto generic_elsewhere = edid("HDMI-A-2", "ADA", "", "");
    expect(touch_pick_output({msi, generic}, nullptr, 0, 0, {"HDMI-A-1", "ADA", "", ""}) == 1,
        "remembered by connector without a serial");
    expect(touch_pick_output({msi, generic_elsewhere}, nullptr, 0, 0, {"HDMI-A-1", "ADA", "", ""}) == -1,
        "serial-less monitor on another connector isn't assumed");
    expect(touch_pick_output({generic, msi_moved}, nullptr, 0, 0, {"DP-1", "MSI", "MP273U", "PB4HB15C00097"}) == 1,
        "remembered by serial on a new connector");
    expect(touch_pick_output({edid("HDMI-A-1", "Other", "X", ""), msi}, nullptr, 0, 0,
        {"HDMI-A-1", "ADA", "", ""}) == -1, "a different monitor on the remembered connector");
    expect(touch_pick_output({msi, generic}, "DP-1", 0, 0, {"HDMI-A-1", "ADA", "", ""}) == 0,
        "libinput output name beats memory");
}

void test_identify() {
    std::vector<TouchOutputCandidate> outs = {monitor, small, laptop};
    for (int i = 0; i < 3; i++) {
        double x, y;
        touch_identify_slot(i, &x, &y);
        expect(touch_identify_hit(outs, x, y) == i, "tap on a slot picks that output");
        expect(touch_identify_hit(outs, x + 0.05, y - 0.05) == i, "near miss still counts");
    }
    expect(touch_identify_hit(outs, 0.5, 0.5) == -1, "center tap hits nothing");
    double x, y;
    touch_identify_slot(1, &x, &y);
    expect(touch_identify_hit({monitor}, x, y) == -1, "slot beyond the outputs hits nothing");

    // Rotated 90: slot 2 (logical bottom-left) is at the panel's native bottom-right.
    TouchOutputCandidate rotated = small;
    rotated.transform = WL_OUTPUT_TRANSFORM_90;
    double nx = 0.8, ny = 0.85; // native tap
    double ox = nx, oy = ny;
    touch_native_to_output(WL_OUTPUT_TRANSFORM_90, &ox, &oy);
    expect(ox > 0.1 && ox < 0.2 && oy > 0.75 && oy < 0.85, "90-degree native to output");
    expect(touch_identify_hit({monitor, laptop, rotated}, nx, ny) == 2, "rotated output's target");
    expect(touch_identify_hit({monitor, laptop, rotated}, 0.15, 0.8) == -1, "unrotated spot misses it");
}

} // namespace

int main() {
    expect(touch_pick_output({laptop, monitor}, "DP-1", 309, 174) == 1, "output name wins over size");
    expect(touch_pick_output({laptop, monitor}, "gone", 0, 0) == 0, "unknown name falls through");
    expect(touch_pick_output({monitor, small}, nullptr, 154, 86) == 1, "size match");
    expect(touch_pick_output({monitor, small}, nullptr, 86, 154) == 1, "size match, rotated digitizer");
    expect(touch_pick_output({monitor, laptop, small}, nullptr, 310, 175) == 1, "closest of several");
    expect(touch_pick_output({monitor, small}, nullptr, 250, 140) == -1, "no size match, several outputs");
    expect(touch_pick_output({monitor, monitor}, nullptr, 527, 296) == -1, "identical monitors are ambiguous");
    expect(touch_pick_output({monitor, laptop}, nullptr, 0, 0) == 1, "unknown size: the built-in panel");
    expect(touch_pick_output({monitor, small}, nullptr, 0, 0) == -1, "unknown size, no panel");
    expect(touch_pick_output({monitor}, nullptr, 100, 100) == 0, "single output");
    expect(touch_pick_output({}, nullptr, 100, 100) == -1, "no outputs");
    expect(touch_pick_output({{"DP-1", 0, 0}, small}, nullptr, 155, 87) == 1, "output without EDID size skipped");
    test_remembered();
    test_identify();
    if (failures == 0) {
        std::printf("touch_map: all passed\n");
    }
    return failures == 0 ? 0 : 1;
}
