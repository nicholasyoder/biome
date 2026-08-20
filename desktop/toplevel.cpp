// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/toplevel.h"

#include "core/cursor.h"
#include "desktop/app_icon.h"
#include "desktop/decoration_bridge.h"
#include "desktop/workspace.h"

#include <xcb/xproto.h>

wlr_surface *toplevel_surface(BiomeToplevel *toplevel) {
    return toplevel->type == BiomeToplevelType::Xdg
        ? toplevel->xdg_toplevel->base->surface
        : toplevel->xwayland_surface->surface;
}

void close_toplevel(BiomeToplevel *toplevel) {
    if (toplevel->type == BiomeToplevelType::Xdg) {
        wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
    } else {
        wlr_xwayland_surface_close(toplevel->xwayland_surface);
    }
}

void toplevel_get_geometry(BiomeToplevel *toplevel, wlr_box *box) {
    if (toplevel->type == BiomeToplevelType::Xdg) {
        wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, box);
        return;
    }
    box->x = 0;
    box->y = 0;
    box->width = toplevel->xwayland_surface->width;
    box->height = toplevel->xwayland_surface->height;
}

bool toplevel_decorated(const BiomeToplevel *toplevel) {
    if (toplevel->type == BiomeToplevelType::Xdg) {
        return true;
    }
    return toplevel->xwayland_surface->decorations == WLR_XWAYLAND_SURFACE_DECORATIONS_ALL;
}

BiomeToplevel *toplevel_from_xdg(wlr_xdg_toplevel *xdg_toplevel) {
    if (xdg_toplevel == nullptr) {
        return nullptr;
    }
    auto *tree = static_cast<wlr_scene_tree *>(xdg_toplevel->base->data);
    return tree ? static_cast<BiomeToplevel *>(tree->node.data) : nullptr;
}

BiomeToplevel *toplevel_from_xwayland(wlr_xwayland_surface *xsurface) {
    if (xsurface == nullptr) {
        return nullptr;
    }
    auto *tree = static_cast<wlr_scene_tree *>(xsurface->data);
    return tree ? static_cast<BiomeToplevel *>(tree->node.data) : nullptr;
}

void toplevel_set_size(BiomeToplevel *toplevel, int x, int y, int width, int height) {
    if (toplevel->type == BiomeToplevelType::Xdg) {
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, width, height);
    } else {
        wlr_xwayland_surface_configure(toplevel->xwayland_surface,
            static_cast<int16_t>(x), static_cast<int16_t>(y),
            static_cast<uint16_t>(width), static_cast<uint16_t>(height));
    }
}

void toplevel_sync_position(BiomeToplevel *toplevel, int x, int y) {
    if (toplevel->type == BiomeToplevelType::Xwayland) {
        wlr_xwayland_surface_configure(toplevel->xwayland_surface,
            static_cast<int16_t>(x), static_cast<int16_t>(y),
            toplevel->xwayland_surface->width, toplevel->xwayland_surface->height);
    }
}

void set_toplevel_focused(BiomeToplevel *toplevel, bool focused) {
    if (toplevel == nullptr) {
        return;
    }
    toplevel->focused = focused;
    render_toplevel_decoration(toplevel);
}

void focus_toplevel(BiomeToplevel *toplevel, wlr_surface *surface) {
    // Note: this function only deals with keyboard focus (and, for
    // Xwayland, the X11 stacking order that goes along with it).
    if (toplevel == nullptr) {
        return;
    }
    BiomeServer *server = toplevel->server;
    wlr_seat *seat = server->seat;
    wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    if (prev_surface == surface) {
        // Don't re-focus an already focused surface.
        return;
    }
    if (prev_surface) {
        // Deactivate the previously focused surface. This lets the client
        // know it no longer has focus and the client will repaint
        // accordingly, e.g. stop displaying a caret.
        wlr_xdg_toplevel *prev_xdg_toplevel =
            wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
        if (prev_xdg_toplevel != nullptr) {
            wlr_xdg_toplevel_set_activated(prev_xdg_toplevel, false);
            set_toplevel_focused(toplevel_from_xdg(prev_xdg_toplevel), false);
        } else {
            wlr_xwayland_surface *prev_xwayland_surface =
                wlr_xwayland_surface_try_from_wlr_surface(prev_surface);
            if (prev_xwayland_surface != nullptr) {
                wlr_xwayland_surface_activate(prev_xwayland_surface, false);
                set_toplevel_focused(toplevel_from_xwayland(prev_xwayland_surface), false);
            }
        }
    }
    wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    // Move the toplevel to the front
    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    wl_list_remove(&toplevel->link);
    wl_list_insert(&server->toplevels, &toplevel->link);
    // Activate the new surface
    if (toplevel->type == BiomeToplevelType::Xdg) {
        wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
    } else {
        wlr_xwayland_surface_activate(toplevel->xwayland_surface, true);
        // wlr_scene_node_raise_to_top only reorders our own render tree;
        // Xwayland windows also need their X11 stacking order raised, since
        // X11 clients (e.g. submenus) may position themselves relative to
        // sibling stacking order.
        wlr_xwayland_surface_restack(toplevel->xwayland_surface, nullptr, XCB_STACK_MODE_ABOVE);
    }
    set_toplevel_focused(toplevel, true);
    // Tell the seat to have the keyboard enter this surface. wlroots will
    // keep track of this and automatically send key events to the
    // appropriate clients without additional work on your part.
    if (keyboard != nullptr) {
        wlr_seat_keyboard_notify_enter(seat, surface,
            keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    }
}

void place_new_toplevel(BiomeToplevel *toplevel) {
    BiomeServer *server = toplevel->server;

    wlr_box geo;
    toplevel_get_geometry(toplevel, &geo);
    int width = geo.width > 0 ? geo.width : 0;
    int height = geo.height > 0 ? geo.height : 0;

    BiomeToplevel *parent = toplevel->type == BiomeToplevelType::Xdg
        ? toplevel_from_xdg(toplevel->xdg_toplevel->parent)
        : toplevel_from_xwayland(toplevel->xwayland_surface->parent);

    // (vis_x, vis_y): desired top-left of the *visible content*, i.e.
    // ignoring our border - toplevel_sync_position and the scene node
    // position (which also needs the border subtracted) are derived from
    // this below.
    int vis_x, vis_y;

    if (parent != nullptr) {
        wlr_box parent_geo;
        toplevel_get_geometry(parent, &parent_geo);
        int parent_vis_x = static_cast<int>(parent->scene_tree->node.x)
            + decoration_border_width(parent, parent->maximized) + parent_geo.x;
        int parent_vis_y = static_cast<int>(parent->scene_tree->node.y)
            + decoration_titlebar_height(parent, parent->maximized) + parent_geo.y;
        vis_x = parent_vis_x + (parent_geo.width - width) / 2;
        vis_y = parent_vis_y + (parent_geo.height - height) / 2;
        toplevel->workspace = parent->workspace;
    } else {
        wlr_box layout_box;
        wlr_output_layout_get_box(server->output_layout, nullptr, &layout_box);
        if (wlr_box_empty(&layout_box)) {
            return;
        }
        int index = static_cast<int>(wl_list_length(&server->toplevels)) % 8;
        int cascade = index * 24;
        vis_x = layout_box.x + (layout_box.width - width) / 2 + cascade;
        vis_y = layout_box.y + (layout_box.height - height) / 2 + cascade;
        toplevel->workspace = server->active_workspace;
    }

    // A freshly-placed toplevel is never already maximized.
    wlr_scene_node_set_position(&toplevel->scene_tree->node,
        vis_x - decoration_border_width(toplevel, false), vis_y - decoration_titlebar_height(toplevel, false));
    toplevel_sync_position(toplevel, vis_x, vis_y);
    toplevel->placed = true;
    update_toplevel_visibility(toplevel);
}

// The output the toplevel is currently (mostly) on, by its visible
// content's top-left corner - falls back to the full output layout extents
// (all outputs combined) if that point isn't on any output, same fallback
// place_new_toplevel uses for centering.
static wlr_box maximize_target_box(BiomeToplevel *toplevel) {
    // Called before toplevel->maximized flips to true (see set_toplevel_
    // maximized below), so toplevel->maximized here still reflects the
    // window's current (non-maximized) on-screen frame.
    BiomeServer *server = toplevel->server;
    double vis_x = toplevel->scene_tree->node.x + decoration_border_width(toplevel, toplevel->maximized);
    double vis_y = toplevel->scene_tree->node.y + decoration_titlebar_height(toplevel, toplevel->maximized);

    wlr_output *output = wlr_output_layout_output_at(server->output_layout, vis_x, vis_y);
    wlr_box box = {};
    wlr_output_layout_get_box(server->output_layout, output, &box);
    if (wlr_box_empty(&box)) {
        return box;
    }

    // box is the whole output - inset it by the decorated frame's border/
    // titlebar so the box this returns is the *content* area a maximized
    // window should fill, matching what toplevel_set_size()/set_toplevel_
    // maximized() below expect (they add the border/titlebar back on to get
    // the outer frame position). Left as the raw output box, the frame ends
    // up larger than the monitor - border/titlebar pushed off-screen on the
    // top-left, overflowing past the edge on the bottom-right - which is why
    // maximize used to look like it lost its border entirely.
    //
    // Unlike vis_x/vis_y above, this queries the *maximized* metrics
    // (hardcoded true, not toplevel->maximized) - a theme is free to size a
    // maximized window's border differently (decoration/theme/
    // biome-dark.qss's [maximized="true"] rules), and it's that state's
    // metrics the inset needs to reserve room for, not the window's current
    // (about-to-be-replaced) ones.
    int left = decoration_border_width(toplevel, true);
    int top = decoration_titlebar_height(toplevel, true);
    int right = decoration_border_right_width(toplevel, true);
    int bottom = decoration_border_bottom_height(toplevel, true);
    wlr_box content = {
        box.x + left,
        box.y + top,
        box.width - left - right,
        box.height - top - bottom,
    };
    return content;
}

void set_toplevel_maximized(BiomeToplevel *toplevel, bool maximized) {
    if (toplevel->maximized == maximized) {
        return;
    }

    wlr_box old_geo;
    toplevel_get_geometry(toplevel, &old_geo);

    wlr_box target;
    if (maximized) {
        toplevel->restore_box.x =
            static_cast<int>(toplevel->scene_tree->node.x) + decoration_border_width(toplevel, toplevel->maximized);
        toplevel->restore_box.y =
            static_cast<int>(toplevel->scene_tree->node.y) + decoration_titlebar_height(toplevel, toplevel->maximized);
        toplevel->restore_box.width = old_geo.width;
        toplevel->restore_box.height = old_geo.height;

        target = maximize_target_box(toplevel);
        if (wlr_box_empty(&target)) {
            return;
        }
        toplevel->maximized = true;
    } else {
        target = toplevel->restore_box;
        toplevel->maximized = false;
    }

    int node_x = target.x - decoration_border_width(toplevel, toplevel->maximized);
    int node_y = target.y - decoration_titlebar_height(toplevel, toplevel->maximized);
    if (toplevel->type == BiomeToplevelType::Xdg) {
        // See maximize_reposition_pending's declaration - picked up by
        // xdg_toplevel_commit once the resized buffer actually lands.
        toplevel->maximize_reposition_pending = true;
        toplevel->maximize_pending_x = node_x;
        toplevel->maximize_pending_y = node_y;
        toplevel->maximize_pending_old_width = old_geo.width;
        toplevel->maximize_pending_old_height = old_geo.height;
    } else {
        wlr_scene_node_set_position(&toplevel->scene_tree->node, node_x, node_y);
    }
    toplevel_set_size(toplevel, target.x, target.y, target.width, target.height);
    toplevel_sync_position(toplevel, target.x, target.y);

    if (toplevel->type == BiomeToplevelType::Xdg) {
        wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, maximized);
    } else {
        wlr_xwayland_surface_set_maximized(toplevel->xwayland_surface, maximized);
    }
    render_toplevel_decoration(toplevel);
}

void set_toplevel_minimized(BiomeToplevel *toplevel, bool minimized) {
    if (toplevel->minimized == minimized) {
        return;
    }
    toplevel->minimized = minimized;
    update_toplevel_visibility(toplevel);

    BiomeServer *server = toplevel->server;
    if (minimized) {
        if (server->seat->keyboard_state.focused_surface == toplevel_surface(toplevel)) {
            wlr_seat_pointer_clear_focus(server->seat);
            focus_topmost_on_active_workspace(server);
        }
    } else {
        focus_toplevel(toplevel, toplevel_surface(toplevel));
    }

    // xdg-shell has no "minimized" configure state to ack (unlike
    // maximize/fullscreen - minimize is a one-way client request with
    // nothing for the compositor to reply with). Xwayland does track it.
    if (toplevel->type == BiomeToplevelType::Xwayland) {
        wlr_xwayland_surface_set_minimized(toplevel->xwayland_surface, minimized);
    }
}

// Shared between xdg-shell and Xwayland: called once the underlying
// wlr_surface is ready to be shown on screen.
void toplevel_map(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, map);

    if (!toplevel->icon_resolved) {
        // app_id/WM_CLASS are expected to already be set by this point - a
        // well-behaved client sets them before its first map, the same
        // assumption place_new_toplevel/toplevel_get_geometry etc. already
        // make about a toplevel's other properties being ready here.
        if (toplevel->type == BiomeToplevelType::Xdg) {
            const char *app_id = toplevel->xdg_toplevel->app_id;
            toplevel->icon = resolve_app_id_icon(app_id != nullptr ? app_id : "");
        } else {
            const char *wm_class = toplevel->xwayland_surface->class_;
            xcb_ewmh_connection_t *ewmh =
                toplevel->server->ewmh_ready ? &toplevel->server->ewmh : nullptr;
            toplevel->icon = resolve_xwayland_icon(
                ewmh, toplevel->xwayland_surface->window_id, wm_class != nullptr ? wm_class : "");
        }
        toplevel->icon_resolved = true;
    }

    place_new_toplevel(toplevel);
    render_toplevel_decoration(toplevel);
    wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
    focus_toplevel(toplevel, toplevel_surface(toplevel));
}

void toplevel_unmap(wl_listener *listener, void *data) {
    (void)data;
    // Called when the surface is unmapped, and should no longer be shown.
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, unmap);

    // Reset the cursor mode if the grabbed toplevel was unmapped.
    if (toplevel == toplevel->server->grabbed_toplevel) {
        reset_cursor_mode(toplevel->server);
    }

    wl_list_remove(&toplevel->link);
}

// Shared between xdg-shell and Xwayland: both signal this with irrelevant
// (or no) event data, so the handler is identical either way.
void toplevel_request_move(wl_listener *listener, void *data) {
    (void)data;
    // This event is raised when a client would like to begin an interactive
    // move, typically because the user clicked on their client-side
    // decorations. Note that a more sophisticated compositor should check
    // the provided serial against a list of button press serials sent to
    // this client, to prevent the client from requesting this whenever they
    // want.
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_move);
    begin_interactive(toplevel, BiomeCursorMode::Move, 0, true);
}

BiomeToplevel *desktop_toplevel_at(
        BiomeServer *server, double lx, double ly,
        wlr_surface **surface, double *sx, double *sy) {
    // This returns the topmost node in the scene at the given layout
    // coords. We only care about surface nodes as we are specifically
    // looking for a surface in the surface tree of a BiomeToplevel.
    wlr_scene_node *node = wlr_scene_node_at(
        &server->scene->tree.node, lx, ly, sx, sy);
    if (node == nullptr || node->type != WLR_SCENE_NODE_BUFFER) {
        return nullptr;
    }
    wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    if (!scene_surface) {
        return nullptr;
    }

    *surface = scene_surface->surface;
    // Find the node corresponding to the BiomeToplevel at the root of this
    // surface tree, it is the only one for which we set the data field.
    // (Override-redirect Xwayland surfaces never set this, so clicking one
    // yields toplevel == nullptr - pointer events still reach it via
    // *surface above, it just isn't managed by us.)
    wlr_scene_tree *tree = node->parent;
    while (tree != nullptr && tree->node.data == nullptr) {
        tree = tree->node.parent;
    }
    return tree ? static_cast<BiomeToplevel *>(tree->node.data) : nullptr;
}
