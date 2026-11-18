// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/ext_workspace.h"

#include "desktop/toplevel.h"
#include "desktop/workspace.h"

#include <cstdint>
#include <string>

namespace {

constexpr uint32_t kExtWorkspaceVersion = 1;

bool workspace_urgent(BiomeServer *server, int index) {
    BiomeToplevel *pos;
    wl_list_for_each(pos, &server->toplevels, link) {
        if (pos->urgent && pos->workspace == index) {
            return true;
        }
    }
    return false;
}

// Only activate is advertised; other requests are ignored, as the spec allows.
void handle_commit(wl_listener *listener, void *data) {
    BiomeServer *server = biome_server_from(listener, BiomeExtWorkspace, commit);
    auto *event = static_cast<wlr_ext_workspace_v1_commit_event *>(data);
    int activate = -1;
    wlr_ext_workspace_v1_request *request;
    wl_list_for_each(request, event->requests, link) {
        if (request->type == WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE && request->activate.workspace != nullptr) {
            activate = static_cast<int>(reinterpret_cast<intptr_t>(request->activate.workspace->data));
        }
    }
    if (activate >= 0) {
        switch_workspace(server, activate);
    }
}

} // namespace

void ext_workspace_init(BiomeServer *server) {
    BiomeExtWorkspace &ws = server->ext_workspace;
    ws.server = server;
    ws.manager = wlr_ext_workspace_manager_v1_create(server->display, kExtWorkspaceVersion);
    ws.group = wlr_ext_workspace_group_handle_v1_create(ws.manager, 0);
    ws.commit.notify = handle_commit;
    wl_signal_add(&ws.manager->events.commit, &ws.commit);
    for (int index = 0; index < server->workspace_count; index++) {
        const std::string id = std::to_string(index);
        wlr_ext_workspace_handle_v1 *handle = wlr_ext_workspace_handle_v1_create(
            ws.manager, id.c_str(), EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
        handle->data = reinterpret_cast<void *>(static_cast<intptr_t>(index));
        wlr_ext_workspace_handle_v1_set_group(handle, ws.group);
        wlr_ext_workspace_handle_v1_set_name(handle, std::to_string(index + 1).c_str());
        const uint32_t coordinates[] = {static_cast<uint32_t>(index)};
        wlr_ext_workspace_handle_v1_set_coordinates(handle, coordinates, 1);
        wlr_ext_workspace_handle_v1_set_active(handle, index == server->active_workspace);
        ws.handles.push_back(handle);
    }
}

void ext_workspace_finish(BiomeServer *server) {
    wl_list_remove(&server->ext_workspace.commit.link);
}

void ext_workspace_output_enter(BiomeServer *server, wlr_output *output) {
    wlr_ext_workspace_group_handle_v1_output_enter(server->ext_workspace.group, output);
}

void ext_workspace_sync_state(BiomeServer *server) {
    const std::vector<wlr_ext_workspace_handle_v1 *> &handles = server->ext_workspace.handles;
    for (int index = 0; index < static_cast<int>(handles.size()); index++) {
        wlr_ext_workspace_handle_v1_set_active(handles[index], index == server->active_workspace);
        wlr_ext_workspace_handle_v1_set_urgent(handles[index], workspace_urgent(server, index));
    }
}
