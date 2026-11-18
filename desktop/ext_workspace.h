// SPDX-License-Identifier: LGPL-3.0-or-later
//
// ext-workspace-v1 on wlroots' helper: one group spanning every output, one
// handle per workspace index with id "<index>" (what org.biome.Workspaces
// numbers mean), name "<index + 1>" and coordinates [index]. Only activate.

#pragma once

#include "core/server.h"

void ext_workspace_init(BiomeServer *server);
void ext_workspace_finish(BiomeServer *server);

// Adds an output to the group; wlroots drops it when the output is destroyed.
void ext_workspace_output_enter(BiomeServer *server, wlr_output *output);

// Pushes active/urgent state. Call whenever active_workspace changes or an
// urgent toplevel appears, disappears or changes workspace.
void ext_workspace_sync_state(BiomeServer *server);
