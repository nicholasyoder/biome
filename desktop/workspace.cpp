// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/workspace.h"

#include "desktop/ext_workspace.h"
#include "desktop/layer_shell.h"
#include "desktop/session_lock.h"
#include "desktop/switcher.h"
#include "desktop/window_menu.h"

void update_toplevel_visibility(BiomeToplevel *toplevel) {
    // No lock check: layers.session_lock covers every toplevel while locked.
    bool visible = toplevel->workspace == toplevel->server->active_workspace && !toplevel->minimized;
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, visible);
    // Not before the first configure has gone out (toplevel_configure_premap).
    if (toplevel->type == BiomeToplevelType::Xdg && toplevel->placed &&
        toplevel->xdg_toplevel->scheduled.suspended == visible) {
        wlr_xdg_toplevel_set_suspended(toplevel->xdg_toplevel, !visible);
    }
}

void focus_topmost_on_active_workspace(BiomeServer *server) {
    if (server->session_locked) {
        session_lock_refocus(server);
        return;
    }
    if (wlr_surface *exclusive = layer_shell_exclusive_focus(server)) {
        if (server->seat->keyboard_state.focused_surface != exclusive) {
            grant_keyboard_focus_to_non_toplevel(server, exclusive);
        }
        return;
    }
    BiomeToplevel *pos;
    wl_list_for_each(pos, &server->toplevels, link) {
        if (pos->workspace == server->active_workspace && !pos->minimized) {
            focus_toplevel(pos);
            return;
        }
    }
    clear_keyboard_focus(server);
}

static int wrap_workspace(BiomeServer *server, int index) {
    if (index < 0) {
        return server->workspace_count - 1;
    }
    if (index >= server->workspace_count) {
        return 0;
    }
    return index;
}

void switch_workspace(BiomeServer *server, int index) {
    index = wrap_workspace(server, index);
    if (index == server->active_workspace) {
        return;
    }
    server->active_workspace = index;
    switcher_cancel(server);
    window_menu_close(server);

    BiomeToplevel *pos;
    wl_list_for_each(pos, &server->toplevels, link) {
        update_toplevel_visibility(pos);
    }
    // The pointer may be sitting over a surface that just got hidden; clear
    // its focus so stale events don't reach it, re-resolved on next motion.
    wlr_seat_pointer_clear_focus(server->seat);
    focus_topmost_on_active_workspace(server);
    ext_workspace_sync_state(server);
}

void move_toplevel_to_workspace(BiomeToplevel *toplevel, int index) {
    BiomeServer *server = toplevel->server;
    index = wrap_workspace(server, index);
    if (index == toplevel->workspace) {
        return;
    }
    toplevel->workspace = index;
    update_toplevel_visibility(toplevel);
    if (toplevel->urgent) {
        ext_workspace_sync_state(server);
    }
    if (index != server->active_workspace) {
        switcher_remove_toplevel(server, toplevel);
        window_menu_remove_toplevel(server, toplevel);
    }
    if (server->seat->keyboard_state.focused_surface == toplevel_surface(toplevel) &&
            index != server->active_workspace) {
        wlr_seat_pointer_clear_focus(server->seat);
        focus_topmost_on_active_workspace(server);
    }
    if (server->window_workspaces_changed != nullptr) {
        server->window_workspaces_changed(server);
    }
}
