// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Bridge between Biome's config (core/config.h) and desktop/layer_shell.cpp.
// Group "LayerShell", two keys - each a comma-separated string when
// hand-typed with more than one namespace, e.g.:
//   [LayerShell]
//   fadingNamespaces=forest-logout
//   scanoutFadingNamespaces=forest-logout-dim,forest-startup
//
// QSettings' IniFormat auto-detects an unescaped comma on read-back and
// hands the value back as a QStringList rather than a plain QString - a
// single namespace (no comma) still round-trips as a plain string. Read
// via QVariant::toStringList() (see parse_namespace_list() in
// fade_config.cpp), which normalizes both shapes; do not switch back to
// QVariant::toString(), which silently returns an empty string for a
// multi-element QStringList instead of rejoining it.
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

// Absent keys = empty sets = no namespace fades. Startup-only.
FadeConfig load_fade_config();
