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
    IconImage icon;    // empty (size == 0) shows a generic icon
    bool urgent = false; // drives #biomeSwitcherIcon[urgent=...]
};

// A row of entry icons with the selected one highlighted and its title below,
// styled by #biomeSwitcherPanel/#biomeSwitcherIcon/#biomeSwitcherTitleLabel.
RenderedFrame render_switcher(const std::vector<SwitcherEntry> &entries, int selected_index, double scale);

} // namespace biome_decoration
