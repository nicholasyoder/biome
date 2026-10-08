// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Alt-Tab window switcher: previews an MRU snapshot frozen for the length of
// an Alt-hold, and focuses the selection on Alt release. Key matching lives in
// core/keybindings.cpp; rendering in decoration/switcher.cpp.

#pragma once

#include "desktop/toplevel.h"

// Creates the hidden panel/highlight scene nodes. Called once at startup.
void switcher_init(BiomeServer *server);

// Alt+Tab press: opens the switcher, or moves the selection if already open.
void switcher_cycle(BiomeServer *server, bool reverse);

// Alt release: focuses (un-minimizing if needed) the selection and closes.
void switcher_commit(BiomeServer *server);

// Drops toplevel from an open switcher, closing it if nothing is left. Must
// run before a toplevel is freed.
void switcher_remove_toplevel(BiomeServer *server, BiomeToplevel *toplevel);
