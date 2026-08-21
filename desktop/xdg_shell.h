// SPDX-License-Identifier: LGPL-3.0-or-later
//
// xdg-shell: toplevel + popup surface lifecycle, and decoration negotiation
// for both xdg-decoration and the older KDE server-decoration protocol -
// Biome honors whatever mode a client negotiates via either, and treats a
// client that negotiates neither as client-side decorated (see
// toplevel_decorated in desktop/toplevel.h).

#pragma once

#include "core/server.h"

// Creates the xdg_shell, xdg_decoration_manager, and kde_decoration_manager
// globals and wires their listeners.
void xdg_shell_init(BiomeServer *server);
