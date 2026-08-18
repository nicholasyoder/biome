// SPDX-License-Identifier: LGPL-3.0-or-later
//
// The 4-workspace model (matches workspace_count in
// forest/usr/share/forest/xfwm4.xml) and the visibility bookkeeping that
// goes with it.

#pragma once

#include "desktop/toplevel.h"

constexpr int kWorkspaceCount = 4;

// A toplevel's scene node should be enabled iff it's on the active
// workspace AND not minimized - the two hide-mechanisms are independent
// (either alone should hide it), so every place that changes either one
// goes through this instead of setting enabled directly.
void update_toplevel_visibility(BiomeToplevel *toplevel);

// Hands focus to the topmost (most-recently-focused) toplevel that's on the
// active workspace and not minimized, or clears keyboard focus if there
// isn't one.
void focus_topmost_on_active_workspace(BiomeServer *server);

void switch_workspace(BiomeServer *server, int index);
void move_toplevel_to_workspace(BiomeToplevel *toplevel, int index);
