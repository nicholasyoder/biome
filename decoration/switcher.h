// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Renders the graphical Alt-Tab switcher overlay: a simple centered panel
// listing window titles, no live thumbnails (matches xfwm4's
// cycle_preview=false default that Phase 2's MRU cycling already mirrors).

#pragma once

#include "renderer.h" // RenderedFrame

#include <string>
#include <vector>

namespace biome_decoration {

struct SwitcherEntry {
    std::string label; // title, falling back to app_id/class if empty
};

// Renders the persistent, QSS-styled SwitcherPanel widget tree (biome-dark
// .qss's #biomeSwitcherPanel/#biomeSwitcherRow rules) - the same
// build/resize/QWidget::render() pattern renderer.cpp uses for the window
// frame, not a hand-painted overlay. selected_index is highlighted - the
// caller is expected to keep it in sync with whichever entry is currently
// focused (index 0, in Biome's MRU list).
RenderedFrame render_switcher(const std::vector<SwitcherEntry> &entries, int selected_index);

} // namespace biome_decoration
