// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Renders the Alt-Tab switcher's live highlight box: a QSS-styled outline
// resized to the previewed window's on-screen frame, shown in place without
// raising or focusing it (see decoration_bridge.cpp's update_switcher_overlay).

#pragma once

#include "renderer.h" // RenderedFrame

namespace biome_decoration {

// Renders the persistent, QSS-styled SwitcherHighlight widget
// (biome-dark.qss's #biomeSwitcherHighlight rule) at the given size - same
// build/resize/QWidget::render() pattern as render_switcher()/
// render_decoration(). Callers position the result at the target window's
// on-screen box themselves.
RenderedFrame render_switcher_highlight(int width, int height);

} // namespace biome_decoration
