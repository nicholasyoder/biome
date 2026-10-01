// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Reloads the compositor cursor when ~/.icons/default/index.theme (the
// freedesktop default cursor theme; Inherits= and Size=) changes. Only
// compositor-drawn and cursor-shape-v1 cursors update live.

#pragma once

struct BiomeServer;

// Call after cursor_init() (core/cursor.h).
void cursor_theme_watcher_init(BiomeServer *server);
