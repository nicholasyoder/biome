// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Bridge between the Qt/QSettings-based output config file and
// core/output.cpp - output_config.cpp is the only file in core/ that
// includes Qt headers. Config lives at ~/.config/Forest/Biome.conf
// (QSettings("Forest", "Biome")), group "Outputs" with a subgroup per
// output keyed by the wlr connector name (e.g. "eDP-1", "HDMI-A-1") - same
// beginGroup()-per-item convention forest itself uses (e.g. panel plugins).
// On disk that's a single [Outputs] section with backslash-escaped keys,
// e.g. "eDP-1\enabled=true" - that's QSettings' own canonical INI form for
// nested groups (confirmed against forest's real Panel.conf), not a
// separate [Outputs/eDP-1] header per connector.

#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

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
    wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL;
};

// Reads ~/.config/Forest/Biome.conf once and returns per-connector
// overrides keyed by wlr connector name. A connector absent from the file,
// or with individually malformed fields, gets OutputConfig{}'s defaults for
// the affected field(s) (a warning is logged for malformed fields, see
// output_config.cpp). Safe to call even if the file doesn't exist (returns
// an empty map). Startup-only - not meant to be called from a hot path.
std::unordered_map<std::string, OutputConfig> load_output_configs();
