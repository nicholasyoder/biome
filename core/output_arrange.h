// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Pure output-layout geometry (no server state), kept separate so it's unit-testable.

#pragma once

#include <cstddef>
#include <vector>

// Only box.h, not core/wlroots.hpp, so the test builds without generated protocol headers.
extern "C" {
#include <wlr/util/box.h>
}

// Component id (0..n-1, in order of first appearance) per box. Boxes sharing an edge
// segment or overlapping are connected; corner-only contact isn't.
std::vector<int> layout_components(const std::vector<wlr_box> &boxes);

// Translates whole components (keeping mirrored groups intact) until all boxes form one
// component; boxes[anchor]'s component never moves. Returns false if some component can't
// be attached without overlapping another box; `boxes` then holds the partial result.
bool close_layout_gaps(std::vector<wlr_box> &boxes, size_t anchor);
