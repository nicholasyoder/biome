// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Loads Biome's own self-contained decoration theme (decoration/theme/
// builtin.qss) - not Forest's installed theme. Forest's real QSS never
// defines window-decoration styling itself (fstyleloader only ever styles
// Forest's own widget chrome: panel, menus, popups), so this reproduces the
// color/shape values of Forest's base-dark + base-rounded theme layers for
// Biome's own new decoration-specific selectors instead, as a starting
// point to later merge with Forest's real themes (see docs/history.md's
// Phase 3 writeup).

#pragma once

#include "frame_widget.h"

#include <QString>

namespace biome_decoration {

// Applies the external layered theme `name` from `dir` (see docs/theming-plan.md
// and architecture-notes.md), or the built-in one if either is empty or the
// theme yields no biome.css. Replaces any previously applied stylesheet;
// existing widgets are re-polished by Qt but callers must re-layout/re-render.
// Call after a QApplication exists, before any DecorationFrame/SwitcherPanel
// is constructed.
void load_decoration_theme(const QString &dir = {}, const QString &name = {});

// Constructs a new DecorationFrame, already given a valid initial layout, so it's immediately safe to query geometry from or render.
// load_decoration_theme() must have already been called.
//
// Every BiomeToplevel owns exactly one of these (desktop/toplevel.h's
// BiomeToplevel::decoration_frame) rather than every window sharing one
// process-wide instance - see desktop/decoration_bridge.h's
// create_toplevel_decoration()/destroy_toplevel_decoration(). Caller takes
// ownership; plain `delete` is fine - deleteLater() would need the Qt event
// loop to actually run to take effect, and this code deliberately doesn't
// lean on that (same reasoning as switcher.cpp's SwitcherPanel teardown -
// see its own comment for why that's still true even though
// ipc/global_shortcuts_portal.cpp now pumps Qt's event loop periodically
// for D-Bus).
DecorationFrame *create_decoration_frame();

} // namespace biome_decoration
