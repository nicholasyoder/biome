// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Sources decoration colors from Forest's active QSS theme. Forest's QSS
// system (fstyleloader) never defines window-decoration styling itself -
// it's purely for Forest's own widget chrome (panel, menus, popups) - but
// its dark/light theme layers do define a reusable pair of selectors,
// QWidget[FSS-color="surface"] and QWidget[FSS-color="pane"], that this
// reads for background/border colors instead.

#pragma once

namespace biome_decoration {

struct DecorationColors {
    float titlebar_bg[4];      // QSS "surface" background, RGBA 0..1
    float titlebar_fg[4];      // derived from titlebar_bg's lightness
    float border_focused[4];   // Biome's own accent - QSS has no equivalent
    float border_unfocused[4]; // QSS "pane" border-color
};

// Reads Forest's active theme (~/.config/Forest/Forest.conf's "theme" key,
// then /usr/share/forest/themes/<theme>/...) via Qt's real QSS/QWidget style
// engine, not a hand-rolled CSS parser. Must be called after a QApplication
// exists. Falls back to Biome's own flat colors (matching Phase 2's border)
// if Forest's theme files can't be found. Read once at startup - not
// live-reloaded on theme change (see docs/plan.md's Phase 3 writeup).
DecorationColors load_decoration_colors();

} // namespace biome_decoration
