// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Loads Biome's own self-contained decoration theme (decoration/theme/
// biome-dark.qss) - not Forest's installed theme. Forest's real QSS never
// defines window-decoration styling itself (fstyleloader only ever styles
// Forest's own widget chrome: panel, menus, popups), so this reproduces the
// color/shape values of Forest's base-dark + base-rounded theme layers for
// Biome's own new decoration-specific selectors instead, as a starting
// point to later merge with Forest's real themes (see docs/plan.md's
// Phase 3 writeup).

#pragma once

#include "frame_widget.h"

namespace biome_decoration {

// Flat RGBA (0..1) colors for decoration/switcher.cpp's hand-painted
// Alt-Tab panel - not used by the window frame itself (that's painted
// entirely through QSS, see frame_widget.h). Every field here is read back
// from the real QSS-styled widget tree by load_decoration_theme(), not a
// hand-duplicated literal, so it can't drift out of sync with the theme.
struct DecorationColors {
    float titlebar_bg[4];
    float titlebar_fg[4];
    float border_focused[4];
    float border_unfocused[4];
};

// Loads the embedded biome-dark.qss, builds and styles the persistent
// DecorationFrame widget tree decoration/renderer.cpp renders for every
// decoration repaint, and reads its qproperty-* metrics back into
// layout.h's runtime geometry globals (kBorderWidth, kTitlebarHeight, ...).
// Must be called once, after a QApplication exists, before any toplevel is
// created - not live-reloaded. Returns the flat colors
// decoration/switcher.cpp's hand-painted Alt-Tab overlay still uses.
DecorationColors load_decoration_theme();

// The persistent widget tree load_decoration_theme() built. Valid only
// after load_decoration_theme() has been called.
DecorationFrame *decoration_frame();

} // namespace biome_decoration
