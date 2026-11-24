// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Which output a touchscreen drives. Biome has no input config, so this is
// heuristic, with tap-to-identify (desktop/touch_identify.h) as the fallback.
// Kept free of compositor state for tests/touch_map_test.cpp.

#pragma once

#include <string>
#include <vector>

#include <wayland-server-protocol.h> // enum wl_output_transform

struct TouchOutputCandidate {
    std::string name; // connector, e.g. "eDP-1"
    int width_mm = 0, height_mm = 0;
    std::string make, model, serial; // EDID
    wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL;
};

// An earlier tap-to-identify answer (core/touch_store.h); connector empty = none.
struct TouchRemembered {
    std::string connector, make, model, serial;
};

// Index into outputs, or -1 if unclear (whole layout). In order: libinput's
// output name for the device, the remembered answer, the one output whose
// physical size matches the device's, the only built-in panel (device size
// unknown), the only output.
int touch_pick_output(const std::vector<TouchOutputCandidate> &outputs, const char *output_name,
    double width_mm, double height_mm, const TouchRemembered &remembered = {});

// Tap-to-identify: output i shows its target at slot i (normalized output
// coordinates); a tap's panel-native position picks the output. kSlots caps
// how many outputs can be told apart.
constexpr int kTouchIdentifySlots = 8;
void touch_identify_slot(int i, double *x, double *y);
// The output whose target the tap (native 0..1) hit, or -1.
int touch_identify_hit(const std::vector<TouchOutputCandidate> &outputs, double x, double y);

// Native 0..1 panel coordinates to normalized output coordinates, undoing transform.
void touch_native_to_output(wl_output_transform transform, double *x, double *y);
