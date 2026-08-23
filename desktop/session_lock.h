// SPDX-License-Identifier: LGPL-3.0-or-later
//
// ext-session-lock-v1: grants a client an opaque, input-exclusive lock
// surface per output while the session is locked. Security-sensitive - see
// docs/plan.md's Phase 3.5 and Phase 4 writeups for the invariants this
// relies on. server->layers.session_lock (core/layers.cpp) is structurally
// the topmost of BiomeServer::layers' six fixed trees, so everything else
// in the compositor is unreachable to hit-testing/rendering the instant
// that tree is enabled - this module only needs to toggle its enabled bit
// and manage its per-output content (BiomeOutput::lock_tree/lock_rect,
// created in core/output.cpp), not raise anything itself.

#pragma once

#include "core/server.h"

// Colors for BiomeOutput::lock_rect - shared between core/output.cpp (where
// the rect is first created) and session_lock.cpp (which flips to the
// abandoned color when a lock client dies without unlocking, and resets
// back to normal at the start of the next lock).
inline constexpr float kSessionLockColor[4] = {0.f, 0.f, 0.f, 1.f};
inline constexpr float kSessionLockAbandonedColor[4] = {1.f, 0.f, 0.f, 1.f};

// Creates the ext_session_lock_manager_v1 global and wires its listener.
void session_lock_init(BiomeServer *server);
