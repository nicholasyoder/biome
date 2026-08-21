// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/toplevel.h"

#include "core/cursor.h"
#include "desktop/app_icon.h"
#include "desktop/decoration_bridge.h"
#include "desktop/workspace.h"

#include <cstring>
#include <xcb/xproto.h>

namespace {

// Apps whose window content already includes a full titlebar (min/max/close
// + drag area) but never negotiate that via any decoration protocol -
// primarily libadwaita/GNOME HeaderBar apps (org.gnome.baobab confirmed via
// live testing). GTK *intends* to ask the compositor not to decorate these -
// gtk_window_set_titlebar()'s own docs say GTK "will do its best to convince
// the window manager not to put its own titlebar on the window" - but GTK4's
// Wayland backend has what looks like a real bug that silently defeats it:
// gdk_wayland_toplevel_set_decorated() (gdk/wayland/gdktoplevel-wayland.c,
// checked against both the 4.18.6 installed here and current GTK main) opens
// with `if (self->decorated == decorated) return;`, but self->decorated is
// never initialized to GTK's documented TRUE default when the toplevel is
// constructed - it starts at the struct's zero/FALSE value. A HeaderBar
// window computes the desired value as TRUE && !client_decorated == FALSE,
// which happens to equal that already-FALSE default, so the guard treats it
// as a no-op and the decoration request never reaches the compositor at all
// (a plain window computes TRUE, which does differ from FALSE, so its
// request goes through fine - matches the asymmetry seen live: `foot` sent a
// request, `org.gnome.baobab` sent none). Nothing Biome can do about that
// upstream bug, so - same as every other wlroots compositor facing this
// (sway's answer to the identical ask in swaywm/sway#3661 is a manual
// `for_window [app_id=...] border csd` config rule) - this hand-maintained
// list is the practical workaround.
//
// Kept deliberately minimal for now (one confirmed entry) rather than
// pre-seeding with the wider libadwaita/GNOME Circle app ecosystem
// (Nautilus, GNOME Text Editor, Settings, Console, Calculator, and similar
// Adw.ApplicationWindow-with-HeaderBar apps almost certainly hit the same
// GTK bug, but haven't been confirmed live) - add an entry as each is
// actually hit, matching wlr_xdg_toplevel::app_id.
//
// TODO: this should eventually move out of a compiled-in array and into a
// user-editable config file, so people hitting a new offender can add it
// themselves without a rebuild - a bigger default list (seeded from the
// libadwaita app category above) would make more sense once that exists.
constexpr const char *kAlwaysClientSideDecoratedAppIds[] = {
    "org.gnome.baobab",
};

bool app_id_always_client_side_decorated(const char *app_id) {
    if (app_id == nullptr) {
        return false;
    }
    for (const char *known : kAlwaysClientSideDecoratedAppIds) {
        if (std::strcmp(app_id, known) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

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
        if (toplevel->xdg_client_side_decorated) {
            return false;
        }
        return !app_id_always_client_side_decorated(toplevel->xdg_toplevel->app_id);
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

// Keyboard focus only (and, for Xwayland, the X11 stacking order with it).
void focus_toplevel(BiomeToplevel *toplevel, wlr_surface *surface) {
    if (toplevel == nullptr) {
        return;
    }
    BiomeServer *server = toplevel->server;
    wlr_seat *seat = server->seat;
    wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    if (prev_surface == surface) {
        return;
    }
    if (prev_surface) {
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
    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    wl_list_remove(&toplevel->link);
    wl_list_insert(&server->toplevels, &toplevel->link);
    if (toplevel->type == BiomeToplevelType::Xdg) {
        wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
    } else {
        wlr_xwayland_surface_activate(toplevel->xwayland_surface, true);
        // wlr_scene_node_raise_to_top only reorders our own render tree;
        // Xwayland windows also need their X11 stacking order raised, since
        // X11 clients (e.g. submenus) may position relative to it.
        wlr_xwayland_surface_restack(toplevel->xwayland_surface, nullptr, XCB_STACK_MODE_ABOVE);
    }
    set_toplevel_focused(toplevel, true);
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

    // (vis_x, vis_y): desired top-left of the visible content, ignoring our
    // border - the scene node position (border subtracted) is derived from
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

    // A freshly placed toplevel is never already maximized.
    wlr_scene_node_set_position(&toplevel->scene_tree->node,
        vis_x - decoration_border_width(toplevel, false), vis_y - decoration_titlebar_height(toplevel, false));
    toplevel_sync_position(toplevel, vis_x, vis_y);
    toplevel->placed = true;
    update_toplevel_visibility(toplevel);
}

// The output the toplevel is currently (mostly) on, by its visible
// content's top-left corner - falls back to the full output layout extents
// if that point isn't on any output. Called before toplevel->maximized
// flips to true, so it still reflects the window's current on-screen frame.
static wlr_box maximize_target_box(BiomeToplevel *toplevel) {
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
    // titlebar so this returns the content area a maximized window should
    // fill (toplevel_set_size()/set_toplevel_maximized() add the border/
    // titlebar back to get the outer frame position). Left as the raw
    // output box, the frame would end up larger than the monitor.
    //
    // Unlike vis_x/vis_y above, this hardcodes the *maximized* metrics
    // (true, not toplevel->maximized) - a theme can size a maximized
    // window's border differently, and it's that state's metrics the inset
    // needs to reserve room for.
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
        // Picked up by xdg_toplevel_commit once the resized buffer lands -
        // see maximize_reposition_pending's declaration.
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

    // xdg-shell has no "minimized" configure state to ack, unlike
    // maximize/fullscreen - it's a one-way client request. Xwayland does
    // track it.
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
        // A well-behaved client sets app_id/WM_CLASS before its first map.
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
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, unmap);

    if (toplevel == toplevel->server->grabbed_toplevel) {
        reset_cursor_mode(toplevel->server);
    }

    wl_list_remove(&toplevel->link);
}

// Shared between xdg-shell and Xwayland: both signal this with irrelevant
// (or no) event data, so the handler is identical either way. A client
// requests this to begin an interactive move, typically from its own CSD.
void toplevel_request_move(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_move);
    begin_interactive(toplevel, BiomeCursorMode::Move, 0, true);
}

BiomeToplevel *desktop_toplevel_at(
        BiomeServer *server, double lx, double ly,
        wlr_surface **surface, double *sx, double *sy) {
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
    // Find the BiomeToplevel at the root of this surface tree - the only
    // node with its data field set. (Override-redirect Xwayland surfaces
    // never set this, so clicking one yields toplevel == nullptr; pointer
    // events still reach it via *surface, it just isn't managed by us.)
    wlr_scene_tree *tree = node->parent;
    while (tree != nullptr && tree->node.data == nullptr) {
        tree = tree->node.parent;
    }
    return tree ? static_cast<BiomeToplevel *>(tree->node.data) : nullptr;
}
