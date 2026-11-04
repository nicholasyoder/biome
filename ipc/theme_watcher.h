// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Live-applies Theme/dir + Theme/name from Biome's config (core/config.h) when
// ~/.config/Biome/ changes. Other config keys are not live-reloaded.

#pragma once

struct BiomeServer;

// Call after the initial load_decoration_theme(); `server` must outlive it.
void theme_watcher_init(BiomeServer *server);
