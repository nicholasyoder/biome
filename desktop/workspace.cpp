// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/workspace.h"

void update_toplevel_visibility(BiomeToplevel *toplevel) {
    bool visible = toplevel->workspace == toplevel->server->active_workspace && !toplevel->minimized;
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, visible);
}

void focus_topmost_on_active_workspace(BiomeServer *server) {
    BiomeToplevel *pos;
    wl_list_for_each(pos, &server->toplevels, link) {
        if (pos->workspace == server->active_workspace && !pos->minimized) {
            focus_toplevel(pos, toplevel_surface(pos));
            return;
        }
    }
    wlr_seat_keyboard_notify_clear_focus(server->seat);
}

static int wrap_workspace(int index) {
    if (index < 0) {
        return kWorkspaceCount - 1;
    }
    if (index >= kWorkspaceCount) {
        return 0;
    }
    return index;
}

void switch_workspace(BiomeServer *server, int index) {
    index = wrap_workspace(index);
    if (index == server->active_workspace) {
        return;
    }
    server->active_workspace = index;

    BiomeToplevel *pos;
    wl_list_for_each(pos, &server->toplevels, link) {
        update_toplevel_visibility(pos);
    }
    // The pointer may be sitting over a surface that just got hidden; clear
    // its focus so stale events don't reach it, re-resolved on next motion.
    wlr_seat_pointer_clear_focus(server->seat);
    focus_topmost_on_active_workspace(server);
}

void move_toplevel_to_workspace(BiomeToplevel *toplevel, int index) {
    BiomeServer *server = toplevel->server;
    index = wrap_workspace(index);
    if (index == toplevel->workspace) {
        return;
    }
    toplevel->workspace = index;
    update_toplevel_visibility(toplevel);
    if (server->seat->keyboard_state.focused_surface == toplevel_surface(toplevel) &&
            index != server->active_workspace) {
        wlr_seat_pointer_clear_focus(server->seat);
        focus_topmost_on_active_workspace(server);
    }
}
