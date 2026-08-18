// SPDX-License-Identifier: LGPL-3.0-or-later
//
// xdg-shell: toplevel + popup surface lifecycle, and xdg-decoration
// negotiation (Biome always forces server-side mode - see
// server_new_xdg_toplevel_decoration in xdg_shell.cpp).

#pragma once

#include "core/server.h"

// Creates the xdg_shell and xdg_decoration_manager globals and wires their
// listeners.
void xdg_shell_init(BiomeServer *server);
