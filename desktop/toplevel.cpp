// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/toplevel.h"

#include "core/cursor.h"
#include "core/output.h"
#include "desktop/app_icon.h"
#include "desktop/decoration_bridge.h"
#include "desktop/ext_workspace.h"
#include "desktop/foreign_toplevel.h"
#include "desktop/workspace.h"

#include <algorithm>
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

void toplevel_get_frame_box(BiomeToplevel *toplevel, wlr_box *box) {
    wlr_box geo;
    toplevel_get_geometry(toplevel, &geo);
    box->x = static_cast<int>(toplevel->scene_tree->node.x);
    box->y = static_cast<int>(toplevel->scene_tree->node.y);
    box->width = decoration_border_width(toplevel, toplevel->maximized) + geo.width
        + decoration_border_right_width(toplevel, toplevel->maximized);
    box->height = decoration_titlebar_height(toplevel, toplevel->maximized) + geo.height
        + decoration_border_bottom_height(toplevel, toplevel->maximized);
}

bool toplevel_decorated(const BiomeToplevel *toplevel) {
    if (toplevel->fullscreen) {
        return false;
    }
    if (toplevel->type == BiomeToplevelType::Xdg) {
        return !toplevel->xdg_client_side_decorated;
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

void toplevel_set_position(BiomeToplevel *toplevel, int x, int y) {
    wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
    foreign_toplevel_update_outputs(toplevel);
}

void set_toplevel_focused(BiomeToplevel *toplevel, bool focused) {
    if (toplevel == nullptr) {
        return;
    }
    toplevel->focused = focused;
    if (focused) {
        set_toplevel_urgent(toplevel, false);
    }
    render_toplevel_decoration(toplevel);
    foreign_toplevel_sync_state(toplevel);
}

void activate_toplevel(BiomeToplevel *toplevel) {
    BiomeServer *server = toplevel->server;
    if (server->session_locked) {
        return;
    }
    // focus_toplevel() alone leaves a minimized or other-workspace window hidden.
    if (toplevel->workspace != server->active_workspace) {
        switch_workspace(server, toplevel->workspace);
    }
    set_toplevel_minimized(toplevel, false);
    focus_toplevel(toplevel);
}

void set_toplevel_urgent(BiomeToplevel *toplevel, bool urgent) {
    if (toplevel->urgent == urgent || (urgent && toplevel->focused)) {
        return;
    }
    toplevel->urgent = urgent;
    render_toplevel_decoration(toplevel);
    ext_workspace_sync_state(toplevel->server);
}

// The toplevel surface is, or whose xdg_popup chain is rooted on; null for
// layer surfaces, layer-owned popups and Xwayland unmanaged surfaces.
static BiomeToplevel *toplevel_owning_surface(wlr_surface *surface) {
    wlr_xdg_popup *popup;
    while (surface != nullptr && (popup = wlr_xdg_popup_try_from_wlr_surface(surface)) != nullptr) {
        surface = popup->parent;
    }
    if (surface == nullptr) {
        return nullptr;
    }
    if (wlr_xdg_toplevel *xdg = wlr_xdg_toplevel_try_from_wlr_surface(surface)) {
        return toplevel_from_xdg(xdg);
    }
    wlr_xwayland_surface *xsurface = wlr_xwayland_surface_try_from_wlr_surface(surface);
    if (xsurface != nullptr && !xsurface->override_redirect) {
        return toplevel_from_xwayland(xsurface);
    }
    return nullptr;
}

void clear_focused_toplevel(BiomeServer *server) {
    BiomeToplevel *prev =
        toplevel_owning_surface(server->seat->keyboard_state.focused_surface);
    if (prev == nullptr) {
        return;
    }
    if (prev->type == BiomeToplevelType::Xdg) {
        wlr_xdg_toplevel_set_activated(prev->xdg_toplevel, false);
    } else {
        wlr_xwayland_surface_activate(prev->xwayland_surface, false);
    }
    set_toplevel_focused(prev, false);
}

void grant_keyboard_focus_to_non_toplevel(BiomeServer *server, wlr_surface *surface) {
    // A window's own menu keeps it activated, as in Mutter/KWin.
    BiomeToplevel *owner = toplevel_owning_surface(surface);
    if (owner == nullptr ||
            owner != toplevel_owning_surface(server->seat->keyboard_state.focused_surface)) {
        clear_focused_toplevel(server);
    }
    wlr_seat *seat = server->seat;
    wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    wlr_seat_keyboard_enter(seat, surface,
        keyboard ? keyboard->keycodes : nullptr,
        keyboard ? keyboard->num_keycodes : 0,
        keyboard ? &keyboard->modifiers : nullptr);
}

// Keyboard focus only (and, for Xwayland, the X11 stacking order with it).
void focus_toplevel(BiomeToplevel *toplevel) {
    if (toplevel == nullptr) {
        return;
    }
    BiomeServer *server = toplevel->server;
    // While the session is locked, no normal toplevel may take keyboard
    // focus from the lock surfaces - this is the one choke point both
    // click-to-focus and auto-focus-on-map go through, so gating it here is
    // enough (see desktop/session_lock.h). Doesn't need to also guard
    // against raising the scene node above the lock anymore: toplevel->
    // scene_tree's parent is always one of the fixed server->layers trees
    // (layers.toplevels normally, layers.fullscreen while fullscreen - see
    // set_toplevel_fullscreen), both structurally below
    // server->layers.session_lock regardless of sibling order within them.
    if (server->session_locked) {
        return;
    }
    // Always the toplevel's own canonical role surface, never whatever
    // surface a caller's hit-test happened to land on - see this function's
    // doc comment in desktop/toplevel.h for why that distinction matters.
    wlr_surface *surface = toplevel_surface(toplevel);
    wlr_seat *seat = server->seat;
    wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    if (prev_surface == surface) {
        return;
    }
    // Returning from the toplevel's own popup: it's still activated, and a
    // deactivate/reactivate round trip would flicker foreign-toplevel state.
    bool from_own_popup = toplevel_owning_surface(prev_surface) == toplevel;
    if (!from_own_popup) {
        clear_focused_toplevel(server);
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
    if (keyboard == nullptr) {
        return;
    }
    // xdg_popup_unmap runs before wlroots ends the popup's keyboard grab,
    // whose .enter is a no-op - bypass it or the enter is silently dropped.
    if (from_own_popup) {
        wlr_seat_keyboard_enter(seat, surface,
            keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    } else {
        wlr_seat_keyboard_notify_enter(seat, surface,
            keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    }
}

// See declaration in toplevel.h.
wlr_box output_target_box(BiomeServer *server, wlr_output *wlr_output) {
    wlr_box box = {};
    wlr_output_layout_get_box(server->output_layout, wlr_output, &box);
    if (wlr_box_empty(&box)) {
        return box;
    }
    BiomeOutput *output = biome_output_from_wlr(server, wlr_output);
    if (output == nullptr) {
        return box;
    }
    wlr_box usable = output->usable_area;
    if (usable.width <= 0 || usable.height <= 0) {
        return box;
    }
    return wlr_box{box.x + usable.x, box.y + usable.y, usable.width, usable.height};
}

// The output whose usable area overlaps window_box the most, or nullptr if
// it overlaps no output. Used for maximize/fullscreen target selection - a
// single-point test on the window's corner picks the wrong monitor whenever
// the window straddles a seam with most of its area on the far side.
static wlr_output *output_with_largest_overlap(BiomeServer *server, const wlr_box &window_box) {
    wlr_output *best = nullptr;
    int best_area = 0;
    BiomeOutput *candidate;
    wl_list_for_each(candidate, &server->outputs, link) {
        wlr_box output_box = {};
        wlr_output_layout_get_box(server->output_layout, candidate->wlr, &output_box);
        wlr_box overlap;
        if (!wlr_box_intersection(&overlap, &window_box, &output_box)) {
            continue;
        }
        int area = overlap.width * overlap.height;
        if (area > best_area) {
            best_area = area;
            best = candidate->wlr;
        }
    }
    return best;
}

// Clamps a visible-content top-left so the *decorated* frame (not just the
// content's top-left) stays inside `target`.
static void clamp_content_into_box(BiomeToplevel *toplevel, const wlr_box &target, int width, int height,
                                   int *vis_x, int *vis_y) {
    int border = decoration_border_width(toplevel, false);
    int border_right = decoration_border_right_width(toplevel, false);
    int titlebar = decoration_titlebar_height(toplevel, false);
    int border_bottom = decoration_border_bottom_height(toplevel, false);
    int min_x = target.x + border;
    int min_y = target.y + titlebar;
    *vis_x = std::clamp(*vis_x, min_x, std::max(min_x, target.x + target.width - width - border_right));
    *vis_y = std::clamp(*vis_y, min_y, std::max(min_y, target.y + target.height - height - border_bottom));
}

static BiomeToplevel *toplevel_parent(BiomeToplevel *toplevel) {
    return toplevel->type == BiomeToplevelType::Xdg
        ? toplevel_from_xdg(toplevel->xdg_toplevel->parent)
        : toplevel_from_xwayland(toplevel->xwayland_surface->parent);
}

static wlr_output *cursor_output(BiomeServer *server) {
    return wlr_output_layout_output_at(server->output_layout, server->cursor->x, server->cursor->y);
}

// The output the toplevel's visible content currently overlaps the most.
static wlr_output *toplevel_content_output(BiomeToplevel *toplevel) {
    double vis_x = toplevel->scene_tree->node.x + decoration_border_width(toplevel, toplevel->maximized);
    double vis_y = toplevel->scene_tree->node.y + decoration_titlebar_height(toplevel, toplevel->maximized);
    wlr_box geo;
    toplevel_get_geometry(toplevel, &geo);
    wlr_box window_box = {static_cast<int>(vis_x), static_cast<int>(vis_y), geo.width, geo.height};
    return output_with_largest_overlap(toplevel->server, window_box);
}

// Where a not-yet-placed toplevel will land: its parent's output, else the cursor's.
static wlr_output *initial_placement_output(BiomeToplevel *toplevel) {
    BiomeToplevel *parent = toplevel_parent(toplevel);
    wlr_output *output = parent != nullptr && parent->placed ? toplevel_content_output(parent) : nullptr;
    return output != nullptr ? output : cursor_output(toplevel->server);
}

// Sets the toplevel's position after a pre-map configure (see
// toplevel_configure_premap) if the client's first buffer acked it and the
// target is still on an output. Otherwise drops the pre-map state so
// toplevel_map's map-time path redoes it. Returns whether it placed.
static bool place_premap_toplevel(BiomeToplevel *toplevel) {
    if (!toplevel->premap_pending) {
        return false;
    }
    toplevel->premap_pending = false;
    wlr_box target = toplevel->premap_target;
    bool acked = static_cast<int32_t>(toplevel->xdg_toplevel->base->current.configure_serial -
        toplevel->premap_serial) >= 0;
    if (!acked || output_with_largest_overlap(toplevel->server, target) == nullptr) {
        toplevel->maximized = false;
        if (toplevel->fullscreen) {
            toplevel->fullscreen = false;
            wlr_scene_node_reparent(&toplevel->scene_tree->node, toplevel->server->layers.toplevels);
        }
        wlr_scene_node_set_position(&toplevel->content_tree->node,
            decoration_border_width(toplevel, false), decoration_titlebar_height(toplevel, false));
        return false;
    }
    BiomeToplevel *parent = toplevel_parent(toplevel);
    toplevel->workspace = parent != nullptr ? parent->workspace : toplevel->server->active_workspace;
    toplevel_set_position(toplevel,
        target.x - decoration_border_width(toplevel, toplevel->maximized),
        target.y - decoration_titlebar_height(toplevel, toplevel->maximized));
    toplevel->placed = true;
    update_toplevel_visibility(toplevel);
    return true;
}

void place_new_toplevel(BiomeToplevel *toplevel) {
    BiomeServer *server = toplevel->server;

    if (toplevel->type == BiomeToplevelType::Xdg && place_premap_toplevel(toplevel)) {
        return;
    }

    wlr_box geo;
    toplevel_get_geometry(toplevel, &geo);
    int width = geo.width > 0 ? geo.width : 0;
    int height = geo.height > 0 ? geo.height : 0;

    BiomeToplevel *parent = toplevel_parent(toplevel);

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
        // Center on the output under the cursor (falling back to the whole
        // layout's box - see output_target_box - if the cursor isn't over
        // any output yet), not the combined multi-output layout box: on a
        // multi-monitor rig with differently-sized outputs, the combined
        // box's center frequently falls near the seam between two outputs
        // rather than the middle of either one.
        wlr_box target = output_target_box(server, cursor_output(server));
        if (wlr_box_empty(&target)) {
            return;
        }
        int index = static_cast<int>(wl_list_length(&server->toplevels)) % 8;
        int cascade = index * 24;
        vis_x = target.x + (target.width - width) / 2 + cascade;
        vis_y = target.y + (target.height - height) / 2 + cascade;
        toplevel->workspace = server->active_workspace;

        // A remembered/default size near the output's size would otherwise
        // push the titlebar or trailing border off-screen.
        clamp_content_into_box(toplevel, target, width, height, &vis_x, &vis_y);
    }

    // A freshly placed toplevel is never already maximized.
    toplevel_set_position(toplevel,
        vis_x - decoration_border_width(toplevel, false), vis_y - decoration_titlebar_height(toplevel, false));
    toplevel_sync_position(toplevel, vis_x, vis_y);
    toplevel->placed = true;
    update_toplevel_visibility(toplevel);
}

// Maximized content box on `output` - the full output layout extents if
// it's nullptr.
static wlr_box maximize_box_on(BiomeToplevel *toplevel, wlr_output *output) {
    wlr_box box = output_target_box(toplevel->server, output);
    if (wlr_box_empty(&box)) {
        return box;
    }

    // box is the space a maximized window should fill - the whole output,
    // shrunk by any exclusive-zone layer surface's reservation (see
    // output_target_box()) - inset further by the decorated frame's
    // border/titlebar so this returns the *content* area
    // (toplevel_set_size()/set_toplevel_maximized() add the border/
    // titlebar back to get the outer frame position). Left uninset, the
    // frame would end up larger than the box it's meant to fill.
    //
    // Hardcodes the *maximized* metrics (true, not toplevel->maximized) - a
    // theme can size a maximized window's border differently, and it's that
    // state's metrics the inset needs to reserve room for.
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

// Called before toplevel->maximized flips to true, so it still reflects the
// window's current on-screen frame.
static wlr_box maximize_target_box(BiomeToplevel *toplevel) {
    return maximize_box_on(toplevel, toplevel_content_output(toplevel));
}

// Moves/resizes to `target` (visible content box) under the toplevel's
// current maximized/fullscreen metrics. For xdg the node position waits for
// the configure ack - see reposition_pending's declaration; the caller must
// store the serial from its wlr_xdg_toplevel_set_* call. An empty target
// (xdg only: restore box unknown, see toplevel_configure_premap) lets the
// client pick its size and centers it once acked.
static void apply_target_box(BiomeToplevel *toplevel, const wlr_box &target) {
    int node_x = target.x - decoration_border_width(toplevel, toplevel->maximized);
    int node_y = target.y - decoration_titlebar_height(toplevel, toplevel->maximized);
    if (toplevel->type == BiomeToplevelType::Xdg) {
        toplevel->reposition_pending = true;
        toplevel->reposition_center = wlr_box_empty(&target);
        toplevel->reposition_pending_x = node_x;
        toplevel->reposition_pending_y = node_y;
    } else {
        toplevel_set_position(toplevel, node_x, node_y);
    }
    toplevel_set_size(toplevel, target.x, target.y, target.width, target.height);
    toplevel_sync_position(toplevel, target.x, target.y);
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

    apply_target_box(toplevel, target);

    if (toplevel->type == BiomeToplevelType::Xdg) {
        // wlr_xdg_toplevel_set_maximized schedules (or joins an
        // already-scheduled) configure and returns its serial - the size
        // set above, if any, coalesces into the same configure, so this is
        // the serial reposition_pending needs to see acked.
        toplevel->reposition_pending_serial = wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, maximized);
    } else {
        wlr_xwayland_surface_set_maximized(toplevel->xwayland_surface, maximized);
    }
    render_toplevel_decoration(toplevel);
    foreign_toplevel_sync_state(toplevel);
}

// Unlike maximize_box_on, the output's full box with no inset (a fullscreen
// frame has no border/titlebar - see toplevel_decorated), panels and all.
// Empty if `output` is nullptr.
static wlr_box fullscreen_box_on(BiomeServer *server, wlr_output *output) {
    wlr_box box = {};
    if (output != nullptr) {
        wlr_output_layout_get_box(server->output_layout, output, &box);
    }
    return box;
}

static wlr_box fullscreen_target_box(BiomeToplevel *toplevel) {
    return fullscreen_box_on(toplevel->server, toplevel_content_output(toplevel));
}

void set_toplevel_fullscreen(BiomeToplevel *toplevel, bool fullscreen) {
    if (toplevel->fullscreen == fullscreen) {
        return;
    }

    wlr_box old_geo;
    toplevel_get_geometry(toplevel, &old_geo);

    wlr_box target;
    if (fullscreen) {
        toplevel->fullscreen_restore_box.x =
            static_cast<int>(toplevel->scene_tree->node.x) + decoration_border_width(toplevel, toplevel->maximized);
        toplevel->fullscreen_restore_box.y =
            static_cast<int>(toplevel->scene_tree->node.y) + decoration_titlebar_height(toplevel, toplevel->maximized);
        toplevel->fullscreen_restore_box.width = old_geo.width;
        toplevel->fullscreen_restore_box.height = old_geo.height;

        target = fullscreen_target_box(toplevel);
        if (wlr_box_empty(&target)) {
            return;
        }
        // Flipped before the decoration_border_width/decoration_titlebar_height
        // calls below so they (via toplevel_decorated) already see the
        // borderless fullscreen state - node_x/node_y then land exactly on
        // target's top-left, with no border to offset for.
        toplevel->fullscreen = true;
        // Above every layer-shell layer (a fullscreen window must cover a
        // panel/dock sitting in the top layer) - see BiomeServer::layers'
        // declaration. Reparenting always lands as the topmost child of the
        // new parent (wlr_scene_node_reparent appends), so this also raises
        // the window above any other already-fullscreen toplevel.
        wlr_scene_node_reparent(&toplevel->scene_tree->node, toplevel->server->layers.fullscreen);
    } else {
        target = toplevel->fullscreen_restore_box;
        toplevel->fullscreen = false;
        wlr_scene_node_reparent(&toplevel->scene_tree->node, toplevel->server->layers.toplevels);
        // Fullscreened before map: no pre-fullscreen box was ever captured.
        if (wlr_box_empty(&target) && toplevel->maximized) {
            target = maximize_target_box(toplevel);
        }
    }

    apply_target_box(toplevel, target);

    if (toplevel->type == BiomeToplevelType::Xdg) {
        // See the maximized case's comment above.
        toplevel->reposition_pending_serial = wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, fullscreen);
    } else {
        wlr_xwayland_surface_set_fullscreen(toplevel->xwayland_surface, fullscreen);
    }
    render_toplevel_decoration(toplevel);
    foreign_toplevel_sync_state(toplevel);
}

void toplevel_configure_premap(BiomeToplevel *toplevel) {
    BiomeServer *server = toplevel->server;
    wlr_xdg_toplevel *xdg = toplevel->xdg_toplevel;
    wlr_output *output = initial_placement_output(toplevel);

    toplevel->fullscreen = false;
    toplevel->maximized = false;
    wlr_box target = {};
    if (xdg->requested.fullscreen) {
        wlr_output *fs_output = xdg->requested.fullscreen_output;
        if (fs_output == nullptr || wlr_output_layout_get(server->output_layout, fs_output) == nullptr) {
            fs_output = output;
        }
        target = fullscreen_box_on(server, fs_output);
        toplevel->fullscreen = !wlr_box_empty(&target);
    }
    if (xdg->requested.maximized) {
        wlr_box box = maximize_box_on(toplevel, output);
        toplevel->maximized = !wlr_box_empty(&box);
        if (!toplevel->fullscreen) {
            target = box;
        }
    }
    // Never had a floating size; unmaximize lets the client pick one.
    toplevel->restore_box = {};
    toplevel->fullscreen_restore_box = {};

    wlr_scene_node_reparent(&toplevel->scene_tree->node,
        toplevel->fullscreen ? server->layers.fullscreen : server->layers.toplevels);
    wlr_scene_node_set_position(&toplevel->content_tree->node,
        decoration_border_width(toplevel, toplevel->maximized),
        decoration_titlebar_height(toplevel, toplevel->maximized));

    wlr_xdg_toplevel_set_maximized(xdg, toplevel->maximized);
    wlr_xdg_toplevel_set_fullscreen(xdg, toplevel->fullscreen);
    toplevel->premap_serial = wlr_xdg_toplevel_set_size(xdg, target.width, target.height);
    toplevel->premap_target = target;
    toplevel->premap_pending = !wlr_box_empty(&target);
}

void toplevel_center_on_output(BiomeToplevel *toplevel) {
    wlr_box geo;
    toplevel_get_geometry(toplevel, &geo);
    wlr_box target = output_target_box(toplevel->server, toplevel_content_output(toplevel));
    if (wlr_box_empty(&target)) {
        return;
    }
    int vis_x = target.x + (target.width - geo.width) / 2;
    int vis_y = target.y + (target.height - geo.height) / 2;
    clamp_content_into_box(toplevel, target, geo.width, geo.height, &vis_x, &vis_y);
    toplevel_set_position(toplevel,
        vis_x - decoration_border_width(toplevel, toplevel->maximized),
        vis_y - decoration_titlebar_height(toplevel, toplevel->maximized));
}

// The enabled output nearest (cx, cy), or nullptr if none is enabled.
static wlr_output *nearest_output(BiomeServer *server, double cx, double cy) {
    double x, y;
    wlr_output_layout_closest_point(server->output_layout, nullptr, cx, cy, &x, &y);
    return wlr_output_layout_output_at(server->output_layout, x, y);
}

// If the decorated frame around `content` (a visible-content box) overlaps no
// output, clamps it into the output nearest its center. Returns whether it moved.
static bool rescue_content_box(BiomeToplevel *toplevel, wlr_box *content, bool maximized_metrics) {
    BiomeServer *server = toplevel->server;
    int left = decoration_border_width(toplevel, maximized_metrics);
    int top = decoration_titlebar_height(toplevel, maximized_metrics);
    wlr_box frame = {
        content->x - left,
        content->y - top,
        left + content->width + decoration_border_right_width(toplevel, maximized_metrics),
        top + content->height + decoration_border_bottom_height(toplevel, maximized_metrics),
    };
    if (output_with_largest_overlap(server, frame) != nullptr) {
        return false;
    }
    wlr_box target = output_target_box(server, nearest_output(server, frame.x + frame.width / 2.0,
                                                              frame.y + frame.height / 2.0));
    if (wlr_box_empty(&target)) {
        return false;
    }
    clamp_content_into_box(toplevel, target, content->width, content->height, &content->x, &content->y);
    return true;
}

void toplevels_relocate_for_layout(BiomeServer *server) {
    BiomeToplevel *toplevel;
    wl_list_for_each(toplevel, &server->toplevels, link) {
        if (!toplevel->placed) {
            continue;
        }
        wlr_box geo;
        toplevel_get_geometry(toplevel, &geo);
        int left = decoration_border_width(toplevel, toplevel->maximized);
        int top = decoration_titlebar_height(toplevel, toplevel->maximized);
        wlr_box content = {static_cast<int>(toplevel->scene_tree->node.x) + left,
                           static_cast<int>(toplevel->scene_tree->node.y) + top, geo.width, geo.height};

        bool changed = rescue_content_box(toplevel, &content, toplevel->maximized);
        if (changed) {
            toplevel_set_position(toplevel, content.x - left, content.y - top);
            toplevel_sync_position(toplevel, content.x, content.y);
        }
        if (toplevel->maximized) {
            rescue_content_box(toplevel, &toplevel->restore_box, false);
        }
        if (toplevel->fullscreen) {
            rescue_content_box(toplevel, &toplevel->fullscreen_restore_box, false);
        }

        // Fullscreen wins over maximized, as in set_toplevel_fullscreen.
        wlr_box target = {};
        if (toplevel->fullscreen) {
            target = fullscreen_target_box(toplevel);
        } else if (toplevel->maximized) {
            target = maximize_target_box(toplevel);
        }
        if (!wlr_box_empty(&target) &&
            (target.x != content.x || target.y != content.y || target.width != geo.width ||
             target.height != geo.height)) {
            apply_target_box(toplevel, target);
            if (toplevel->type == BiomeToplevelType::Xdg) {
                toplevel->reposition_pending_serial = toplevel->fullscreen
                    ? wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, true)
                    : wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, true);
            }
            render_toplevel_decoration(toplevel);
            changed = true;
        }

        if (changed && toplevel == server->grabbed_toplevel) {
            reset_cursor_mode(server);
        }
        // Unmoved windows can still gain/lose an output that moved or toggled.
        foreign_toplevel_update_outputs(toplevel);
    }
}

void set_toplevel_minimized(BiomeToplevel *toplevel, bool minimized) {
    if (toplevel->minimized == minimized) {
        return;
    }
    toplevel->minimized = minimized;
    update_toplevel_visibility(toplevel);
    foreign_toplevel_sync_state(toplevel);

    BiomeServer *server = toplevel->server;
    if (minimized) {
        if (server->seat->keyboard_state.focused_surface == toplevel_surface(toplevel)) {
            wlr_seat_pointer_clear_focus(server->seat);
            focus_topmost_on_active_workspace(server);
        }
    } else {
        focus_toplevel(toplevel);
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

    // Normally a no-op: toplevel_configure_premap already applied these. Only
    // acts if the client mapped without acking that configure, or its
    // output went away (see place_premap_toplevel).
    if (toplevel->type == BiomeToplevelType::Xdg) {
        if (toplevel->xdg_toplevel->requested.fullscreen) {
            set_toplevel_fullscreen(toplevel, true);
        } else if (toplevel->xdg_toplevel->requested.maximized) {
            set_toplevel_maximized(toplevel, true);
        }
    }

    render_toplevel_decoration(toplevel);
    wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
    if (toplevel->urgent) { // X11 urgency hint set before map
        ext_workspace_sync_state(toplevel->server);
    }
    foreign_toplevel_create(toplevel);
    focus_toplevel(toplevel);
}

void toplevel_unmap(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, unmap);
    BiomeServer *server = toplevel->server;

    if (toplevel == server->grabbed_toplevel) {
        reset_cursor_mode(server);
    }

    bool was_focused = server->seat->keyboard_state.focused_surface == toplevel_surface(toplevel);

    // Unlink before foreign_toplevel_destroy(): it synchronously fires
    // window_workspaces_changed, and org.biome.Workspaces' handler
    // (ipc/workspace_bridge.cpp) recomputes its snapshot by walking
    // server->toplevels right then - with the old order, this toplevel was
    // still linked in for that walk, so a closed window kept counting
    // against its workspace until some unrelated later event recomputed a
    // fresh (correct) snapshot.
    wl_list_remove(&toplevel->link);
    foreign_toplevel_destroy(toplevel);
    if (toplevel->urgent) {
        toplevel->urgent = false;
        ext_workspace_sync_state(server);
    }

    if (was_focused) {
        wlr_seat_pointer_clear_focus(server->seat);
        focus_topmost_on_active_workspace(server);
    }
}

// Shared between xdg-shell and Xwayland: both signal this with irrelevant
// (or no) event data, so the handler is identical either way. A client
// requests this to begin an interactive move, typically from its own CSD.
void toplevel_request_move(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_move);
    begin_interactive(toplevel, BiomeCursorMode::Move, 0, true);
}

wlr_scene_node *scene_node_at(BiomeServer *server, double lx, double ly, double *sx, double *sy) {
    return wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
}

BiomeToplevel *desktop_toplevel_at_node(wlr_scene_node *node, wlr_surface **surface) {
    if (node == nullptr || node->type != WLR_SCENE_NODE_BUFFER) {
        return nullptr;
    }
    wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    if (!scene_surface) {
        return nullptr;
    }

    *surface = scene_surface->surface;

    // A popup off a real toplevel nests inside that toplevel's content_tree,
    // whose scene node never gets node.data set (unlike the toplevel's own) -
    // without this check the walk-up below would sail past it and
    // misattribute the click/hover to the toplevel it's nested in.
    wlr_xdg_surface *xdg_surface =
        wlr_xdg_surface_try_from_wlr_surface(wlr_surface_get_root_surface(*surface));
    if (xdg_surface != nullptr && xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
        return nullptr;
    }

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

BiomeToplevel *desktop_toplevel_at(
        BiomeServer *server, double lx, double ly,
        wlr_surface **surface, double *sx, double *sy) {
    wlr_scene_node *node = scene_node_at(server, lx, ly, sx, sy);
    return desktop_toplevel_at_node(node, surface);
}
