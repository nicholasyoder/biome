// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Bridge between Biome's config (core/config.h) and core/output.cpp.
// Group "Outputs" with a subgroup per wlr connector name (e.g. "eDP-1").
// On disk that's a single [Outputs] section with backslash-escaped keys,
// e.g. "eDP-1\enabled=true" - QSettings' canonical INI form for nested
// groups, not a separate [Outputs/eDP-1] header per connector.

#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <wayland-server-protocol.h> // enum wl_output_transform

struct OutputConfig {
    bool enabled = true;

    struct Mode {
        int width = 0;
        int height = 0;
        int refresh_mhz = 0; // 0 = unspecified (match on resolution only)
    };
    std::optional<Mode> mode; // nullopt = "preferred"

    double scale = 1.0;
    std::optional<std::pair<int, int>> position; // nullopt = auto-arrange
    // BiomeServer::output_layout_generation when `position` was recorded (Biome.conf = 0).
    unsigned position_generation = 0;
    wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL;
};

// Per-connector overrides keyed by wlr connector name. Malformed fields log
// a warning and keep OutputConfig{}'s defaults. Startup-only.
std::unordered_map<std::string, OutputConfig> load_output_configs();

// Writes these connectors' entries to the user Biome.conf (never conf.d), in
// the format load_output_configs() reads back exactly. Unset mode/position
// leave the existing keys alone.
void save_output_configs(const std::vector<std::pair<std::string, OutputConfig>> &configs);
