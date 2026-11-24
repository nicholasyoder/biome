// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/touch_map.h"

#include <algorithm>
#include <cmath>

namespace {

bool is_builtin_panel(const std::string &name) {
    return name.rfind("eDP", 0) == 0 || name.rfind("LVDS", 0) == 0 || name.rfind("DSI", 0) == 0;
}

// Worst per-axis relative size error, either orientation; infinite if unknown.
double size_error(const TouchOutputCandidate &output, double width_mm, double height_mm) {
    if (output.width_mm <= 0 || output.height_mm <= 0) {
        return INFINITY;
    }
    auto err = [](double a, double b) { return std::fabs(a - b) / b; };
    double w = output.width_mm, h = output.height_mm;
    return std::min(std::max(err(width_mm, w), err(height_mm, h)),
        std::max(err(width_mm, h), err(height_mm, w)));
}

// A serial identifies the monitor wherever it's plugged; without one, the
// connector does (as long as the same model is still on it).
bool is_remembered(const TouchOutputCandidate &output, const TouchRemembered &r) {
    if (output.make != r.make || output.model != r.model) {
        return false;
    }
    return r.serial.empty() ? output.name == r.connector : output.serial == r.serial;
}

// wlr_fbox_transform() on a point in the unit square.
void transform_point(wl_output_transform t, double *x, double *y) {
    const double px = *x, py = *y;
    switch (t) {
    case WL_OUTPUT_TRANSFORM_NORMAL: break;
    case WL_OUTPUT_TRANSFORM_90: *x = 1 - py; *y = px; break;
    case WL_OUTPUT_TRANSFORM_180: *x = 1 - px; *y = 1 - py; break;
    case WL_OUTPUT_TRANSFORM_270: *x = py; *y = 1 - px; break;
    case WL_OUTPUT_TRANSFORM_FLIPPED: *x = 1 - px; break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_90: *x = py; *y = px; break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_180: *y = 1 - py; break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_270: *x = 1 - py; *y = 1 - px; break;
    }
}

} // namespace

int touch_pick_output(const std::vector<TouchOutputCandidate> &outputs, const char *output_name,
        double width_mm, double height_mm, const TouchRemembered &remembered) {
    const int count = static_cast<int>(outputs.size());
    if (output_name != nullptr) {
        for (int i = 0; i < count; i++) {
            if (outputs[i].name == output_name) {
                return i;
            }
        }
    }

    if (!remembered.connector.empty()) {
        for (int i = 0; i < count; i++) {
            if (is_remembered(outputs[i], remembered)) {
                return i;
            }
        }
    }

    if (width_mm > 0 && height_mm > 0) {
        constexpr double kTolerance = 0.1;
        int best = -1;
        double best_error = kTolerance;
        bool tied = false;
        for (int i = 0; i < count; i++) {
            double e = size_error(outputs[i], width_mm, height_mm);
            if (e <= kTolerance && best >= 0 && outputs[i].width_mm == outputs[best].width_mm &&
                    outputs[i].height_mm == outputs[best].height_mm) {
                tied = true; // identical monitors: can't tell which one
            } else if (e <= best_error) {
                best = i;
                best_error = e;
                tied = false;
            }
        }
        if (best >= 0) {
            return tied ? -1 : best;
        }
    } else {
        int builtin = -1;
        for (int i = 0; i < count; i++) {
            if (is_builtin_panel(outputs[i].name)) {
                builtin = builtin == -1 ? i : -2;
            }
        }
        if (builtin >= 0) {
            return builtin;
        }
    }

    return count == 1 ? 0 : -1;
}

void touch_native_to_output(wl_output_transform transform, double *x, double *y) {
    transform_point(transform, x, y);
}

void touch_identify_slot(int i, double *x, double *y) {
    // Corners, then edge midpoints; the center is left for the prompt text.
    static const double slots[kTouchIdentifySlots][2] = {
        {0.15, 0.2}, {0.85, 0.2}, {0.15, 0.8}, {0.85, 0.8},
        {0.5, 0.2}, {0.5, 0.8}, {0.15, 0.5}, {0.85, 0.5},
    };
    *x = slots[i % kTouchIdentifySlots][0];
    *y = slots[i % kTouchIdentifySlots][1];
}

int touch_identify_hit(const std::vector<TouchOutputCandidate> &outputs, double x, double y) {
    constexpr double kRadius = 0.15;
    int best = -1;
    double best_distance = kRadius;
    const int count = std::min(static_cast<int>(outputs.size()), kTouchIdentifySlots);
    for (int i = 0; i < count; i++) {
        // Where the tap lands on output i if this panel is output i.
        double ox = x, oy = y;
        touch_native_to_output(outputs[i].transform, &ox, &oy);
        double tx, ty;
        touch_identify_slot(i, &tx, &ty);
        double d = std::hypot(ox - tx, oy - ty);
        if (d < best_distance) {
            best = i;
            best_distance = d;
        }
    }
    return best;
}
