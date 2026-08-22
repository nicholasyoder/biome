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

// Reads the embedded biome-dark.qss and applies it application-wide via
// qApp->setStyleSheet() - every widget created afterwards, whether shown or
// (as everywhere in Biome) only ever offscreen-rendered, picks the rules up
// automatically with no per-widget setStyleSheet() call needed (verified
// empirically: an app-wide stylesheet cascades to widgets constructed later
// exactly the same as one set directly on them). Must be called once, after
// a QApplication exists, before any DecorationFrame/SwitcherPanel is
// constructed - not live-reloaded.
void load_decoration_theme();

// Constructs a new DecorationFrame, already repolished (see frame_widget.h's
// repolish_tree() - Qt's normal on-first-show auto-polish never runs since
// Biome never QWidget::show()s these widgets, and an app-wide stylesheet
// doesn't exempt a never-shown widget from that) and given a valid initial
// layout, so it's immediately safe to query geometry from or render.
// load_decoration_theme() must have already been called.
//
// Every BiomeToplevel owns exactly one of these (desktop/toplevel.h's
// BiomeToplevel::decoration_frame) rather than every window sharing one
// process-wide instance - see desktop/decoration_bridge.h's
// create_toplevel_decoration()/destroy_toplevel_decoration(). Caller takes
// ownership; plain `delete` is fine (Biome never pumps a Qt event loop, so
// deleteLater() would just leak - same reasoning as switcher.cpp's
// SwitcherPanel teardown).
DecorationFrame *create_decoration_frame();

} // namespace biome_decoration
