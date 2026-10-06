// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/output_config.h"

#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

void expect(bool condition, const char *what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

OutputConfig config(OutputConfig::Mode mode, float scale, wl_output_transform transform, int x, int y) {
    OutputConfig cfg;
    cfg.mode = mode;
    cfg.scale = scale;
    cfg.transform = transform;
    cfg.position = std::make_pair(x, y);
    return cfg;
}

bool same(const OutputConfig &a, const OutputConfig &b) {
    bool mode_same = a.mode.has_value() == b.mode.has_value() &&
        (!a.mode || (a.mode->width == b.mode->width && a.mode->height == b.mode->height &&
                     a.mode->refresh_mhz == b.mode->refresh_mhz));
    return a.enabled == b.enabled && mode_same && static_cast<float>(a.scale) == static_cast<float>(b.scale) &&
        a.position == b.position && a.transform == b.transform;
}

} // namespace

int main() {
    // load_output_configs() reads the user file once, so save everything first.
    QTemporaryDir home;
    setenv("XDG_CONFIG_HOME", home.path().toLocal8Bit().constData(), 1);

    OutputConfig disabled = config({1920, 1080, 60000}, 1.0f, WL_OUTPUT_TRANSFORM_NORMAL, 0, 0);
    disabled.enabled = false;
    OutputConfig never_placed; // no mode/position: those keys stay unwritten
    never_placed.enabled = false;
    const std::vector<std::pair<std::string, OutputConfig>> saved = {
        {"DP-1", config({3840, 2160, 59997}, 1.5f, WL_OUTPUT_TRANSFORM_90, 0, 0)},
        {"DP-2", config({2560, 1440, 144000}, 1.00390625f, WL_OUTPUT_TRANSFORM_FLIPPED_270, 1440, -200)},
        {"HDMI-A-1", config({1280, 720, 0}, 4.0f / 3.0f, WL_OUTPUT_TRANSFORM_180, 3990, 0)},
        {"WL-1", disabled},
        {"WL-2", never_placed},
    };
    save_output_configs(saved);

    const auto loaded = load_output_configs();
    expect(loaded.size() == saved.size(), "every saved connector loads");
    for (const auto &[connector, cfg] : saved) {
        auto it = loaded.find(connector);
        expect(it != loaded.end() && same(it->second, cfg), connector.c_str());
    }

    if (failures != 0) {
        return EXIT_FAILURE;
    }
    std::puts("output_config: all passed");
    return EXIT_SUCCESS;
}
