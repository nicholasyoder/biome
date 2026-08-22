// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Region: the one piece of decoration geometry shared between the Qt widget
// tree (decoration/frame_widget.h's DecorationFrame::hitTest() returns one)
// and core/cursor.cpp's pointer handling (which dispatches on it), kept
// dependency-free (no Qt, no wlroots) so both sides can include it cheaply.
// The actual geometry - border widths, titlebar height, button/resize-corner
// hit regions - lives entirely in the real, QSS-styled widget tree
// (decoration/frame_widget.h/.cpp); core/cursor.cpp reads it back live from
// each toplevel's own persistent DecorationFrame instance (BiomeToplevel::
// decoration_frame, desktop/toplevel.h) instead of a separate
// hand-duplicated model.

#pragma once

namespace biome_decoration {

enum class Region {
    None,
    Titlebar,
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
