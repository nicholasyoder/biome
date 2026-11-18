// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Decoration hit-test result, dependency-free so the compositor side can
// include it. The geometry itself lives in the widget tree (frame_widget.h).

#pragma once

namespace biome_decoration {

enum class Region {
    None,
    Titlebar,
    Icon, // titlebar app icon: opens the window menu
    ButtonMinimize,
    ButtonMaximize,
    ButtonClose,
    ResizeN,
    ResizeS,
    ResizeE,
    ResizeW,
    ResizeNE,
    ResizeNW,
    ResizeSE,
    ResizeSW,
};

} // namespace biome_decoration
