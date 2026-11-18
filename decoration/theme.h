// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Decoration theme: a Forest layered theme's biome.css, or the built-in
// decoration/theme/builtin.qss when none is usable.

#pragma once

#include "frame_widget.h"

#include <QString>

namespace biome_decoration {

// Applies theme `name` from `dir` (architecture-notes.md), or the built-in one
// if either is empty or the theme has no biome.css. Existing widgets are
// repolished; callers re-layout and re-render.
void load_decoration_theme(const QString &dir = {}, const QString &name = {});

// A laid-out frame for one toplevel. Caller owns it; plain delete (Biome
// never runs a Qt event loop, so no deleteLater).
DecorationFrame *create_decoration_frame();

} // namespace biome_decoration
