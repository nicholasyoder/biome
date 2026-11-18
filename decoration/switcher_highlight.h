// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Renders the Alt-Tab switcher's live highlight box: a QSS-styled outline
// resized to the previewed window's on-screen frame, shown in place without
// raising or focusing it (see desktop/switcher.cpp).

#pragma once

#include "renderer.h" // RenderedFrame

namespace biome_decoration {

// Renders the persistent, QSS-styled SwitcherHighlight widget
// (builtin.qss's #biomeSwitcherHighlight rule) at the given size - same
// build/resize/QWidget::render() pattern as render_switcher()/
// render_decoration(). Callers position the result at the target window's
// on-screen box themselves.
// Cached per size and scale.
RenderedFrame render_switcher_highlight(int width, int height, double scale);

// Drops the cached render; call when the stylesheet changes.
void invalidate_switcher_highlight();

} // namespace biome_decoration
