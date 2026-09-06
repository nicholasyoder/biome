// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Bridge between the Qt/QSettings-based config file and desktop/layer_shell.cpp
// - fade_config.cpp is the only file in core/ (besides output_config.cpp)
// that includes Qt headers. Config lives at ~/.config/Forest/Biome.conf
// (QSettings("Forest", "Biome")), group "LayerShell", two keys - each a
// single comma-separated string (not QSettings' own QStringList encoding,
// which isn't natural to hand-type), e.g.:
//   [LayerShell]
//   fadingNamespaces=forest-logout
//   scanoutFadingNamespaces=forest-logout-dim
//
// Two independent fade mechanisms exist (see FadeKind in
// desktop/layer_shell.cpp): fadingNamespaces gets the original per-pixel
// wlr_scene_buffer_set_opacity() fade; scanoutFadingNamespaces gets the
// opaque-snapshot-blend fade (direct-scanout-eligible, but only actually
// achieves that on an output where the faded surface is the only visible
// content). A namespace should appear in at most one of the two.

#pragma once

#include <string>
#include <unordered_set>

struct FadeConfig {
    std::unordered_set<std::string> fading_namespaces;
    std::unordered_set<std::string> scanout_fading_namespaces;
};

// Reads ~/.config/Forest/Biome.conf once. Absent config = empty sets = no
// namespace fades, same "caller doesn't special-case a missing file"
// contract as load_output_configs(). Safe to call even if the file doesn't
// exist. Startup-only - not meant to be called from a hot path.
FadeConfig load_fade_config();
