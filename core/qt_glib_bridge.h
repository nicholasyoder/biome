// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Bridges Qt's GLib-backed event dispatcher (QEventDispatcherGlib, which
// drives g_main_context_default() on the main thread) into wl_event_loop, so
// Biome's own wl_display_run() loop dispatches Qt/QtDBus fd-driven instead of
// via a polling timer. See docs/qt-event-loop-integration-research.md for the
// full design/feasibility writeup. Needs zero Qt API - it only ever touches
// the process-wide default GLib context through public <glib.h> calls, so
// anything else attached to that context (now or later) rides along free.

#pragma once

struct BiomeServer;

// Call once at startup, immediately after server->display is created and
// before anything else (output_manager_init(), the IPC modules, etc.) runs -
// so Qt/D-Bus dispatch is live for the entire rest of startup. No matching
// teardown call: this bridge acquires the GLib context once and holds it for
// the life of the process (see qt_glib_bridge.cpp for why that's correct).
void qt_glib_bridge_init(BiomeServer *server);
