// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/workspace.h"

void update_toplevel_visibility(BiomeToplevel *toplevel) {
    // No session_locked check here (Phase 3.5 added one; removed now that
    // Workstream A's real per-output layer stack exists - see
    // BiomeServer::layers' doc comment in server.h). toplevel->scene_tree is
    // always a child of server->layers.toplevels, which is structurally
    // below server->layers.session_lock for every toplevel that will ever
    // exist, not just the ones that existed when a lock began - so while
    // locked, layers.session_lock's opaque per-output rect (desktop/
    // session_lock.cpp) already covers every toplevel for both rendering
    // and hit-testing (desktop_toplevel_at/decoration_toplevel_at stop at
    // the first node they hit, topmost first) with no need to separately
    // disable this node too.
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
