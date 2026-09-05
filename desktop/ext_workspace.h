// SPDX-License-Identifier: LGPL-3.0-or-later
//
// ext-workspace-v1, hand-rolled - wlroots ships no C type for this protocol
// (unlike wlr-layer-shell-unstable-v1/wlr-foreign-toplevel-management-
// unstable-v1, both of which wlroots implements itself), so this module
// owns the wl_global/wl_resource plumbing directly instead of wrapping an
// existing wlroots type the way every other desktop/*.cpp module does. See
// docs/phase4-plan.md's Workstream D for the research behind adopting it.
//
// Biome's workspace model (desktop/workspace.h) is a single flat, global
// list (not per-output), so this exposes exactly one ext_workspace_group_
// handle_v1 spanning every output, containing one ext_workspace_handle_v1
// per index in [0, BiomeServer::workspace_count). Only the `activate`
// capability is advertised - no create_workspace/remove/assign/deactivate,
// matching Biome's fixed-policy identity (nothing dynamically creates or
// destroys a workspace). output_enter/output_leave and workspace
// coordinates are deliberately not sent: neither carries information a
// client could act on given Biome's flat model, and every real client this
// was tested against (Forest's deskswitch) only needs the workspace list,
// names, and active state.
//
// Per ext-workspace-v1's `commit` semantics, a client's `activate` request
// doesn't take effect until that client separately sends `commit` on the
// manager - state changes from a batch of requests must be applied
// atomically. This module tracks one pending activation index per bound
// client and applies it on commit.

#pragma once

#include "core/server.h"

// Creates the ext_workspace_manager_v1 global.
void ext_workspace_init(BiomeServer *server);

// Re-sends `state` for every workspace handle on every bound client
// (cheaper to just resend all of them than to diff old/new active index),
// followed by `done`. Called from switch_workspace() (desktop/workspace.cpp)
// whenever BiomeServer::active_workspace changes, including changes that
// originated from this protocol's own activate/commit handling.
void ext_workspace_sync_active(BiomeServer *server);
