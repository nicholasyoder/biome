// SPDX-License-Identifier: LGPL-3.0-or-later
//
// ext-session-lock-v1. layers.session_lock is the topmost fixed scene tree,
// so enabling it hides everything else; this module toggles it and manages
// the per-output lock content (BiomeOutput::lock_tree/lock_rect).

#pragma once

#include "core/server.h"

// BiomeOutput::lock_rect colors; abandoned = the lock client died while locked.
inline constexpr float kSessionLockColor[4] = {0.f, 0.f, 0.f, 1.f};
inline constexpr float kSessionLockAbandonedColor[4] = {1.f, 0.f, 0.f, 1.f};

void session_lock_init(BiomeServer *server);
void session_lock_finish(BiomeServer *server);

// While locked, moves keyboard focus to a mapped lock surface unless one has it,
// or clears it if there is none. No-op when unlocked.
void session_lock_refocus(BiomeServer *server);

// Sends `locked` once no output is still waiting on its first locked frame.
// No-op if there's no live lock or `locked` already went out.
void session_lock_maybe_send_locked(BiomeServer *server);
