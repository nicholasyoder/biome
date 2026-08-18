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

// Reads the embedded biome-dark.qss (see this file's header above) and
// applies it to root, then repolishes root's whole subtree so every rule -
// including qproperty-* values - takes effect immediately. Needed because
// Biome never QWidget::show()s these widgets through a real event loop, so
// Qt's normal on-first-show auto-polish never runs (see frame_widget.h's
// repolish_tree()). Shared by load_decoration_theme() below, for its
// DecorationFrame, and switcher.cpp's SwitcherPanel - two separate
// top-level widget trees painted by the same stylesheet.
void apply_decoration_stylesheet(QWidget *root);

// Builds/styles the persistent DecorationFrame widget tree decoration/
// renderer.cpp renders for every decoration repaint and core/main.cpp
// hit-tests/positions against (DecorationFrame::hitTest()/borderWidth()/
// titlebarHeight()). Must be called once, after a QApplication exists,
// before any toplevel is created - not live-reloaded.
void load_decoration_theme();

// The persistent widget tree load_decoration_theme() built. Valid only
// after load_decoration_theme() has been called.
DecorationFrame *decoration_frame();

} // namespace biome_decoration
