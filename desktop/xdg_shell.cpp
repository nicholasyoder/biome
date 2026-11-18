// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/xdg_shell.h"

#include "core/cursor.h"
#include "desktop/decoration_bridge.h"
#include "desktop/foreign_toplevel.h"
#include "desktop/switcher.h"
#include "desktop/window_menu.h"
#include "desktop/toplevel.h"
#include "desktop/workspace.h"

#include <cassert>
#include <cstdlib>

static void server_new_xdg_toplevel(wl_listener *listener, void *data);
static void server_new_xdg_popup(wl_listener *listener, void *data);
static void server_new_xdg_toplevel_decoration(wl_listener *listener, void *data);
static void server_new_kde_decoration(wl_listener *listener, void *data);
static void claim_pending_kde_decoration(BiomeServer *server, BiomeToplevel *toplevel, wlr_surface *surface);

void xdg_shell_init(BiomeServer *server) {
    server->xdg.server = server;
    wl_list_init(&server->toplevels);
    wl_list_init(&server->unmanaged);
    wl_list_init(&server->xdg.pending_kde_decorations);
    server->xdg.shell = wlr_xdg_shell_create(server->display, 6);
    server->xdg.new_toplevel.notify = server_new_xdg_toplevel;
    wl_signal_add(&server->xdg.shell->events.new_toplevel, &server->xdg.new_toplevel);
    server->xdg.new_popup.notify = server_new_xdg_popup;
    wl_signal_add(&server->xdg.shell->events.new_popup, &server->xdg.new_popup);

    server->xdg.decoration_manager = wlr_xdg_decoration_manager_v1_create(server->display);
    server->xdg.new_toplevel_decoration.notify = server_new_xdg_toplevel_decoration;
    wl_signal_add(&server->xdg.decoration_manager->events.new_toplevel_decoration,
        &server->xdg.new_toplevel_decoration);

    // GTK3 only speaks this older KDE protocol.
    server->xdg.kde_decoration_manager = wlr_server_decoration_manager_create(server->display);
    wlr_server_decoration_manager_set_default_mode(
        server->xdg.kde_decoration_manager, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
    server->xdg.new_kde_decoration.notify = server_new_kde_decoration;
    wl_signal_add(&server->xdg.kde_decoration_manager->events.new_decoration, &server->xdg.new_kde_decoration);
}

void xdg_shell_finish(BiomeServer *server) {
    wl_list_remove(&server->xdg.new_toplevel.link);
    wl_list_remove(&server->xdg.new_popup.link);
    wl_list_remove(&server->xdg.new_toplevel_decoration.link);
    wl_list_remove(&server->xdg.new_kde_decoration.link);
}

static void xdg_toplevel_commit(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, commit);

    if (toplevel->xdg_toplevel->base->initial_commit) {
        if (toplevel->decoration != nullptr) {
            // set_mode/unset_mode precede the first commit, and this is the
            // first point a configure may be sent.
            toplevel->xdg_client_side_decorated = toplevel->decoration->requested_mode ==
                WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
            wlr_xdg_toplevel_decoration_v1_set_mode(toplevel->decoration,
                toplevel->xdg_client_side_decorated
                    ? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
                    : WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        } else if (toplevel->kde_decoration != nullptr) {
            // Resolved when the KDE object was created.
        } else {
            // No decoration object means client-side per both protocols (and
            // is what libadwaita HeaderBar windows do, via a GTK4 bug).
            toplevel->xdg_client_side_decorated = true;
            wlr_log(WLR_DEBUG, "xdg-decoration: app_id=%s created no decoration object -> client-side",
                toplevel->xdg_toplevel->app_id ? toplevel->xdg_toplevel->app_id : "(null)");
        }
        // The required reply to the initial commit. After the decoration
        // mode is settled: the maximized size and content_tree offset
        // depend on it.
        toplevel_configure_premap(toplevel);
        return;
    }

    // If this commit lands mid-resize-grab and the drag is moving the
    // left/top edge, this is the buffer matching the size last requested -
    // reposition now, anchored off the fixed opposite edge captured in
    // grab_geobox at grab start, so position and content land together. See
    // cursor.cpp's process_cursor_resize for why the eager position update
    // was removed for xdg-shell toplevels.
    BiomeServer *server = toplevel->server;
    if (server->grab.mode == BiomeCursorMode::Resize && server->grab.toplevel == toplevel &&
            (server->grab.resize_edges & (WLR_EDGE_LEFT | WLR_EDGE_TOP))) {
        wlr_box geo;
        toplevel_get_geometry(toplevel, &geo);
        int x = static_cast<int>(toplevel->scene_tree->node.x);
        int y = static_cast<int>(toplevel->scene_tree->node.y);
        if (server->grab.resize_edges & WLR_EDGE_LEFT) {
            int content_right = server->grab.geobox.x + server->grab.geobox.width;
            x = content_right - geo.width - geo.x - decoration_border_width(toplevel, toplevel->maximized);
        }
        if (server->grab.resize_edges & WLR_EDGE_TOP) {
            int content_bottom = server->grab.geobox.y + server->grab.geobox.height;
            y = content_bottom - geo.height - geo.y - decoration_titlebar_height(toplevel, toplevel->maximized);
        }
        toplevel_set_position(toplevel, x, y);
    }

    if (toplevel->reposition_pending) {
        // Wait for the commit that acks the configure set_toplevel_maximized/
        // set_toplevel_fullscreen sent - see reposition_pending's
        // declaration. Not a size comparison: the requested size can equal
        // the size the client already has, in which case it never resizes
        // at all, only acks. Serial arithmetic is wraparound-safe per the
        // xdg-shell/wl_display_next_serial convention.
        bool resolves =
            static_cast<int32_t>(toplevel->xdg_toplevel->base->current.configure_serial -
                toplevel->reposition_pending_serial) >= 0;
        if (resolves) {
            if (toplevel->reposition_center) {
                toplevel_center_on_output(toplevel);
            } else {
                toplevel_set_position(toplevel, toplevel->reposition_pending_x, toplevel->reposition_pending_y);
            }
            toplevel->reposition_pending = false;
            // The decoration (and its buttons) just moved out from under a
            // cursor that may not have moved since the click that requested
            // this - see refresh_decoration_hover's comment.
            refresh_decoration_hover(server);
        }
    }

    // The client may have resized itself, or changed its title, outside of
    // an interactive grab - keep the decoration in sync.
    render_toplevel_decoration(toplevel);
    foreign_toplevel_update_outputs(toplevel);
}

static void xdg_toplevel_set_title(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, set_title);
    toplevel->title_serial++;
    render_toplevel_decoration(toplevel);
    foreign_toplevel_update_title_app_id(toplevel);
}

static void xdg_toplevel_set_parent(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, set_parent);
    foreign_toplevel_update_parent(toplevel);
}

static void xdg_toplevel_destroy(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, destroy);

    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->commit.link);
    wl_list_remove(&toplevel->set_title.link);
    wl_list_remove(&toplevel->set_parent.link);
    wl_list_remove(&toplevel->destroy.link);
    wl_list_remove(&toplevel->request_move.link);
    wl_list_remove(&toplevel->request_resize.link);
    wl_list_remove(&toplevel->request_show_window_menu.link);
    wl_list_remove(&toplevel->request_maximize.link);
    wl_list_remove(&toplevel->request_fullscreen.link);
    wl_list_remove(&toplevel->request_minimize.link);
    if (toplevel->decoration != nullptr) {
        wl_list_remove(&toplevel->decoration_destroy.link);
        wl_list_remove(&toplevel->decoration_request_mode.link);
    }
    if (toplevel->kde_decoration != nullptr) {
        wl_list_remove(&toplevel->kde_decoration_destroy.link);
        wl_list_remove(&toplevel->kde_decoration_mode.link);
    }

    toplevel_destroy_common(toplevel);
}

// A client requests this to begin an interactive resize, typically from its
// own CSD.
static void xdg_toplevel_request_resize(wl_listener *listener, void *data) {
    auto *event = static_cast<wlr_xdg_toplevel_resize_event *>(data);
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
    begin_interactive(toplevel, BiomeCursorMode::Resize, event->edges, true);
}

// CSD client asking for the window menu (e.g. right-click on its titlebar).
static void xdg_toplevel_request_show_window_menu(wl_listener *listener, void *data) {
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_show_window_menu);
    auto *event = static_cast<wlr_xdg_toplevel_show_window_menu_event *>(data);
    wlr_seat *seat = toplevel->server->seat;
    // Same spoofing check as begin_interactive's: only from the pointer-focused client.
    wlr_surface *focused = seat->pointer_state.focused_surface;
    if (!toplevel->placed || focused == nullptr ||
            toplevel_surface(toplevel) != wlr_surface_get_root_surface(focused)) {
        return;
    }
    // content_tree's origin is the surface's geometry origin.
    int x = toplevel->scene_tree->node.x + toplevel->content_tree->node.x + event->x;
    int y = toplevel->scene_tree->node.y + toplevel->content_tree->node.y + event->y;
    window_menu_open(toplevel, x, y);
}

// Pre-map requests go through toplevel_configure_premap instead; one sent
// before the initial commit is picked up by that commit.
static bool handle_premap_state_request(BiomeToplevel *toplevel) {
    if (toplevel->placed) {
        return false;
    }
    if (toplevel->xdg_toplevel->base->initialized) {
        toplevel_configure_premap(toplevel);
    }
    return true;
}

// Maximize and unmaximize both go through this one signal, distinguished by
// requested.maximized.
static void xdg_toplevel_request_maximize(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_maximize);
    if (handle_premap_state_request(toplevel)) {
        return;
    }
    bool requested = toplevel->xdg_toplevel->requested.maximized;
    bool was_maximized = toplevel->maximized;
    set_toplevel_maximized(toplevel, requested);
    if (was_maximized == requested) {
        // set_toplevel_maximized no-op'd (already in the requested state),
        // but xdg-shell still requires a configure reply to every request.
        wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, requested);
    }
}

// Same shape as xdg_toplevel_request_maximize just above - a configure reply
// is required even when the request no-ops because it asks for the state
// the toplevel is already in.
static void xdg_toplevel_request_fullscreen(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);
    if (handle_premap_state_request(toplevel)) {
        return;
    }
    bool requested = toplevel->xdg_toplevel->requested.fullscreen;
    bool was_fullscreen = toplevel->fullscreen;
    set_toplevel_fullscreen(toplevel, requested, requested ? toplevel->xdg_toplevel->requested.fullscreen_output : nullptr);
    if (was_fullscreen == requested) {
        wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, requested);
    }
}

// Unlike maximize/fullscreen, xdg-shell has no configure state for
// minimized and thus no acknowledgment requirement - just act on it.
static void xdg_toplevel_request_minimize(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_minimize);
    if (!toplevel->placed) {
        return;
    }
    set_toplevel_minimized(toplevel, toplevel->xdg_toplevel->requested.minimized);
}

static void server_new_xdg_toplevel(wl_listener *listener, void *data) {
    BiomeServer *server = biome_server_from(listener, BiomeXdgShell, new_toplevel);
    auto *xdg_toplevel = static_cast<wlr_xdg_toplevel *>(data);

    BiomeToplevel *toplevel = toplevel_create(server, BiomeToplevelType::Xdg);
    toplevel->xdg_toplevel = xdg_toplevel;
    // Before anything below reads toplevel_decorated()/xdg_client_side_decorated
    // (border-width lookups just a few lines down), pick up a KDE decoration
    // object the client may have created before this toplevel existed.
    claim_pending_kde_decoration(server, toplevel, xdg_toplevel->base->surface);

    toplevel->scene_tree = wlr_scene_tree_create(toplevel->server->layers.toplevels);
    toplevel->scene_tree->node.data = toplevel;
    create_toplevel_decoration(toplevel);

    toplevel->content_tree =
        wlr_scene_xdg_surface_create(toplevel->scene_tree, xdg_toplevel->base);
    toplevel->content_tree->node.data = toplevel;
    wlr_scene_node_set_position(&toplevel->content_tree->node,
        decoration_border_width(toplevel, toplevel->maximized), decoration_titlebar_height(toplevel, toplevel->maximized));
    xdg_toplevel->base->data = toplevel->content_tree;

    toplevel->map.notify = toplevel_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
    toplevel->unmap.notify = toplevel_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
    toplevel->commit.notify = xdg_toplevel_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
    toplevel->set_title.notify = xdg_toplevel_set_title;
    wl_signal_add(&xdg_toplevel->events.set_title, &toplevel->set_title);
    toplevel->set_parent.notify = xdg_toplevel_set_parent;
    wl_signal_add(&xdg_toplevel->events.set_parent, &toplevel->set_parent);

    toplevel->destroy.notify = xdg_toplevel_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

    toplevel->request_move.notify = toplevel_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
    toplevel->request_resize.notify = xdg_toplevel_request_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
    toplevel->request_show_window_menu.notify = xdg_toplevel_request_show_window_menu;
    wl_signal_add(&xdg_toplevel->events.request_show_window_menu, &toplevel->request_show_window_menu);
    toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
    wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
    toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
    toplevel->request_minimize.notify = xdg_toplevel_request_minimize;
    wl_signal_add(&xdg_toplevel->events.request_minimize, &toplevel->request_minimize);
}

static void constrain_popup_to_output(BiomeServer *server, wlr_xdg_popup *xdg_popup);

static void xdg_popup_commit(wl_listener *listener, void *data) {
    (void)data;
    BiomePopup *popup = wl_container_of(listener, popup, commit);

    if (popup->xdg_popup->base->initial_commit) {
        if (popup->unconstrain_on_initial_commit) {
            constrain_popup_to_output(popup->server, popup->xdg_popup);
        }
        if (popup->has_layer_unconstrain_box) {
            wlr_xdg_popup_unconstrain_from_box(popup->xdg_popup, &popup->layer_unconstrain_box);
        }
        // The compositor must reply to an initial commit with a configure so
        // the client can map the surface; Biome sends an empty one.
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
    }
}

static void xdg_popup_destroy(wl_listener *listener, void *data) {
    (void)data;
    BiomePopup *popup = wl_container_of(listener, popup, destroy);

    if (popup->server->activation.unmapped_popup == popup->xdg_popup->base->surface) {
        popup->server->activation.unmapped_popup = nullptr;
    }
    wl_list_remove(&popup->map.link);
    wl_list_remove(&popup->unmap.link);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);
    wl_list_remove(&popup->reposition.link);

    free(popup);
}

// Walks xdg_popup->parent up to the true root of a popup-on-popup chain
// (e.g. a submenu) - not just the immediate parent. Shared by
// constrain_popup_to_output() and the keyboard-focus decision below.
//
// root_popup->parent is never null once attached: either a real
// xdg_toplevel's surface, or (per wlroots' layer_surface_handle_get_popup())
// a layer surface's own wl_surface. Resolve which via
// wlr_layer_surface_v1_try_from_wlr_surface()/wlr_xdg_toplevel_try_from_
// wlr_surface() - don't assume null means layer-shell-owned.
static wlr_xdg_popup *find_root_popup(wlr_xdg_popup *xdg_popup) {
    wlr_xdg_popup *root_popup = xdg_popup;
    while (root_popup->parent != nullptr) {
        wlr_xdg_surface *parent_surface = wlr_xdg_surface_try_from_wlr_surface(root_popup->parent);
        if (parent_surface == nullptr || parent_surface->role != WLR_XDG_SURFACE_ROLE_POPUP) {
            break;
        }
        root_popup = parent_surface->popup;
    }
    return root_popup;
}

// Slides xdg_popup back on-screen against whichever output its ROOT ancestor
// surface currently sits on - shared by the initial new_popup handling below
// and by xdg_popup_reposition() (a client re-running its positioner, e.g.
// once a menu's real content size is known, needs the exact same treatment
// or the corrected geometry sails through unconstrained).
//
// wlr_xdg_popup_unconstrain_from_box()'s own doc comment requires the box to
// be in "the popup's root toplevel parent surface" coordinate system - NOT
// just its immediate parent, which find_root_popup() resolves (see its own
// comment for why that distinction matters for a submenu). Once at the root
// popup, its own scene node's parent is the true root surface (a toplevel's
// tree or a layer surface's tree either way).
static void constrain_popup_to_output(BiomeServer *server, wlr_xdg_popup *xdg_popup) {
    if (!xdg_popup->base->initialized) {
        return;
    }
    wlr_xdg_popup *root_popup = find_root_popup(xdg_popup);

    auto *root_scene_tree = static_cast<wlr_scene_tree *>(root_popup->base->data);
    if (root_scene_tree == nullptr || root_scene_tree->node.parent == nullptr) {
        return;
    }

    int root_lx = 0, root_ly = 0;
    wlr_scene_node_coords(&root_scene_tree->node.parent->node, &root_lx, &root_ly);
    wlr_output *wlr_output = wlr_output_layout_output_at(server->output_layout, root_lx, root_ly);
    if (wlr_output == nullptr) {
        return;
    }

    wlr_box output_box = {};
    wlr_output_layout_get_box(server->output_layout, wlr_output, &output_box);
    if (wlr_box_empty(&output_box)) {
        return;
    }

    wlr_box unconstrain_box = output_box;
    unconstrain_box.x -= root_lx;
    unconstrain_box.y -= root_ly;
    wlr_xdg_popup_unconstrain_from_box(xdg_popup, &unconstrain_box);
}

// xdg_popup.reposition (wlr_xdg_popup::events.reposition) fires when a
// client re-runs its positioner against the same popup, most commonly to
// correct an initial size guess once a menu's real content/layout is known
// (Qt's QMenu on Wayland does exactly this: create the popup, then
// immediately reposition once its true size is settled - confirmed via
// WAYLAND_DEBUG trace against Forest's system tray menu). Without this
// listener, that corrected positioner - the one actually shown to the user -
// never gets run through constrain_popup_to_output() at all, since the
// new_popup-time call below only ever runs once; a tall menu anchored near a
// bottom-docked panel would keep the exact off-screen geometry the client
// asked for.
static void xdg_popup_reposition(wl_listener *listener, void *data) {
    (void)data;
    BiomePopup *popup = wl_container_of(listener, popup, reposition);
    constrain_popup_to_output(popup->server, popup->xdg_popup);
}

// Whether a clicked popup may take keyboard focus. Layer-shell popups inherit
// their parent's keyboard_interactivity, so clicks may focus them; a
// toplevel's non-grabbing popup must not (Chromium closes it on the leave).
bool popup_wants_keyboard_focus(wlr_xdg_popup *xdg_popup) {
    wlr_xdg_popup *root_popup = find_root_popup(xdg_popup);
    if (root_popup->parent != nullptr &&
            wlr_layer_surface_v1_try_from_wlr_surface(root_popup->parent) != nullptr) {
        return true;
    }
    return xdg_popup->seat != nullptr;
}

static void xdg_popup_map(wl_listener *listener, void *data) {
    (void)data;
    BiomePopup *popup = wl_container_of(listener, popup, map);
    BiomeServer *server = popup->server;

    // Only grabbing or xdg-activated popups take focus on map.
    wlr_surface *surface = popup->xdg_popup->base->surface;
    const bool activated = server->activation.unmapped_popup == surface;
    server->activation.unmapped_popup = nullptr;
    if (popup->xdg_popup->seat == nullptr && !activated) {
        return;
    }

    // A grabbing popup's own .enter callback is a deliberate no-op (wlroots'
    // wlr_xdg_popup.c), so the grab-aware wlr_seat_keyboard_notify_enter()
    // would be silently swallowed here - grant_keyboard_focus_to_non_
    // toplevel()'s plain wlr_seat_keyboard_enter() bypasses that.
    grant_keyboard_focus_to_non_toplevel(server, surface);
}

static void xdg_popup_unmap(wl_listener *listener, void *data) {
    (void)data;
    BiomePopup *popup = wl_container_of(listener, popup, unmap);
    BiomeServer *server = popup->server;
    wlr_seat *seat = server->seat;
    if (seat->keyboard_state.focused_surface != popup->xdg_popup->base->surface) {
        return;
    }

    // Hand focus back to whatever this popup was anchored on: its xdg_popup
    // parent, if that resolves to a live BiomeToplevel (routed through
    // focus_toplevel so its raise/activate bookkeeping stays right), or
    // otherwise the bare parent surface itself - a layer surface (e.g. the
    // panel) or an enclosing popup in a submenu chain, neither of which is a
    // BiomeToplevel. Falls back to the topmost visible toplevel if the parent
    // is already gone.
    wlr_surface *parent = popup->xdg_popup->parent;
    BiomeToplevel *parent_toplevel = nullptr;
    if (parent != nullptr) {
        wlr_xdg_toplevel *parent_xdg = wlr_xdg_toplevel_try_from_wlr_surface(parent);
        if (parent_xdg != nullptr) {
            parent_toplevel = toplevel_from_xdg(parent_xdg);
        } else {
            wlr_xwayland_surface *parent_xwayland = wlr_xwayland_surface_try_from_wlr_surface(parent);
            if (parent_xwayland != nullptr) {
                parent_toplevel = toplevel_from_xwayland(parent_xwayland);
            }
        }
    }
    if (parent_toplevel != nullptr) {
        focus_toplevel(parent_toplevel);
        return;
    }
    if (parent != nullptr) {
        grant_keyboard_focus_to_non_toplevel(server, parent);
        return;
    }
    focus_topmost_on_active_workspace(server);
}

static void server_new_xdg_popup(wl_listener *listener, void *data) {
    BiomeServer *server = biome_server_from(listener, BiomeXdgShell, new_popup);
    auto *xdg_popup = static_cast<wlr_xdg_popup *>(data);

    // A new grab while locked would capture the lock client's input; popup_done.
    if (server->session_locked) {
        wlr_xdg_popup_destroy(xdg_popup);
        return;
    }

    auto *popup = static_cast<BiomePopup *>(calloc(1, sizeof(BiomePopup)));
    popup->xdg_popup = xdg_popup;
    popup->server = server;

    // Adding a popup to the scene graph needs its parent scene node, which
    // is why every xdg_surface's user data is set to its scene node. A
    // layer-shell-owned popup is created with xdg_popup->parent still null
    // here - the client gets a popup via the ordinary xdg_surface.get_popup
    // request (parent-less, which xdg-shell itself allows specifically for
    // this case) and only associates it with its layer surface afterward,
    // via a separate zwlr_layer_surface_v1.get_popup request (confirmed
    // from wlroots' own types/wlr_layer_shell_v1.c, not assumed). That
    // second request is what fires wlr_layer_surface_v1::events.new_popup -
    // desktop/layer_shell.cpp's handler creates the scene node once the
    // parent is actually known; commit/destroy listeners are still wired
    // unconditionally below since those don't need a parent scene tree yet.
    if (xdg_popup->parent != nullptr) {
        wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
        assert(parent != nullptr);
        auto *parent_tree = static_cast<wlr_scene_tree *>(parent->data);
        xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

        // Constrain to whichever output the parent surface is actually on,
        // so an anchor near the screen edge gets slid back on-screen instead
        // of hanging off it - without this, the positioner's own
        // constraint_adjustment has no box to slide within and is a no-op.
        // See xdg_popup_reposition() below for why this same call also has
        // to run again on every later reposition, not just here.
        popup->unconstrain_on_initial_commit = true;
    }
    xdg_popup->base->surface->data = popup; // layer_shell.cpp's new_popup handler finds it here

    popup->map.notify = xdg_popup_map;
    wl_signal_add(&xdg_popup->base->surface->events.map, &popup->map);

    popup->unmap.notify = xdg_popup_unmap;
    wl_signal_add(&xdg_popup->base->surface->events.unmap, &popup->unmap);

    popup->commit.notify = xdg_popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

    popup->reposition.notify = xdg_popup_reposition;
    wl_signal_add(&xdg_popup->events.reposition, &popup->reposition);

    popup->destroy.notify = xdg_popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

static void xdg_toplevel_decoration_destroy_handler(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, decoration_destroy);
    wl_list_remove(&toplevel->decoration_destroy.link);
    wl_list_remove(&toplevel->decoration_request_mode.link);
    toplevel->decoration = nullptr;
    // xdg_client_side_decorated is left at its last negotiated value - a
    // client tearing down its decoration object typically does so right
    // before the whole surface goes away too, and there's no "please go
    // back to the default" signal to honor even if it didn't.
}

// After xdg_client_side_decorated flips on an initialized toplevel. Pre-map,
// the premap configure was sized for the old mode (e.g. LibreOffice creates
// its KDE decoration after the initial commit), so redo it.
static void apply_decoration_change(BiomeToplevel *toplevel) {
    if (!toplevel->placed) {
        toplevel_configure_premap(toplevel);
        return;
    }
    wlr_scene_node_set_position(&toplevel->content_tree->node,
        decoration_border_width(toplevel, toplevel->maximized),
        decoration_titlebar_height(toplevel, toplevel->maximized));
    render_toplevel_decoration(toplevel);
}

// A client's zxdg_toplevel_decoration_v1 asked for a mode change (also
// fires once for its first set_mode/unset_mode, sent before the initial
// commit - see xdg_toplevel_commit, which acks that first request instead
// of doing it here since a configure isn't valid to send pre-init).
static void xdg_toplevel_decoration_request_mode(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, decoration_request_mode);
    if (!toplevel->xdg_toplevel->base->initialized) {
        return;
    }
    bool want_client_side = toplevel->decoration->requested_mode ==
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
    wlr_xdg_toplevel_decoration_v1_set_mode(toplevel->decoration,
        want_client_side ? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
                          : WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    if (toplevel->xdg_client_side_decorated != want_client_side) {
        toplevel->xdg_client_side_decorated = want_client_side;
        apply_decoration_change(toplevel);
    }
}

// A client created an xdg_toplevel_decoration object to negotiate server- or
// client-side decorations. Biome honors whatever the client asks for rather
// than forcing server-side - see toplevel_decorated. This only actually
// removes Biome's frame for a client whose CSD is genuinely conditional on
// the granted mode (confirmed working for Chromium/Electron apps); a client
// whose custom titlebar is unconditional real widget content regardless of
// what's granted (e.g. some GTK headerbar apps) will still show both - no
// protocol negotiation can fix that case, it needs an explicit override
// instead. A client that never creates one of these objects (or a KDE one -
// see server_new_kde_decoration) at all is assumed to be client-side
// decorated already, per both protocols' specs - see toplevel_decorated.
static void server_new_xdg_toplevel_decoration(wl_listener *listener, void *data) {
    (void)listener;
    auto *decoration = static_cast<wlr_xdg_toplevel_decoration_v1 *>(data);
    BiomeToplevel *toplevel = toplevel_from_xdg(decoration->toplevel);
    if (toplevel == nullptr) {
        return;
    }
    toplevel->decoration = decoration;
    toplevel->decoration_destroy.notify = xdg_toplevel_decoration_destroy_handler;
    wl_signal_add(&decoration->events.destroy, &toplevel->decoration_destroy);
    toplevel->decoration_request_mode.notify = xdg_toplevel_decoration_request_mode;
    wl_signal_add(&decoration->events.request_mode, &toplevel->decoration_request_mode);
}

static void kde_decoration_destroy_handler(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, kde_decoration_destroy);
    wl_list_remove(&toplevel->kde_decoration_destroy.link);
    wl_list_remove(&toplevel->kde_decoration_mode.link);
    toplevel->kde_decoration = nullptr;
}

// Shared by server_new_kde_decoration (first mode, sent by wlroots
// immediately as the manager's default before this listener can even be
// attached - see below) and kde_decoration_mode_handler (later changes).
static void apply_kde_decoration_mode(BiomeToplevel *toplevel) {
    bool want_client_side =
        toplevel->kde_decoration->mode == WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT;
    if (toplevel->xdg_client_side_decorated == want_client_side) {
        return;
    }
    toplevel->xdg_client_side_decorated = want_client_side;
    if (!toplevel->xdg_toplevel->base->initialized) {
        // Too early to touch content_tree/decoration_buffer - the initial
        // position gets set for real once xdg_toplevel_commit's
        // initial_commit branch runs, same as the xdg-decoration path.
        return;
    }
    apply_decoration_change(toplevel);
}

static void kde_decoration_mode_handler(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, kde_decoration_mode);
    apply_kde_decoration_mode(toplevel);
}

// Attaches an already-created KDE server_decoration object to toplevel,
// hooking its destroy/mode listeners and applying its current mode. Shared
// by server_new_kde_decoration (decoration created after the toplevel
// already exists) and claim_pending_kde_decoration (decoration created
// before it - see BiomePendingKdeDecoration below).
static void attach_kde_decoration(BiomeToplevel *toplevel, wlr_server_decoration *decoration) {
    toplevel->kde_decoration = decoration;
    toplevel->kde_decoration_destroy.notify = kde_decoration_destroy_handler;
    wl_signal_add(&decoration->events.destroy, &toplevel->kde_decoration_destroy);
    toplevel->kde_decoration_mode.notify = kde_decoration_mode_handler;
    wl_signal_add(&decoration->events.mode, &toplevel->kde_decoration_mode);
    apply_kde_decoration_mode(toplevel);
}

// A KDE decoration object created before its wl_surface has an xdg_toplevel
// role yet - GTK routinely creates it that way (wl_surface ->
// server_decoration -> get_xdg_surface -> get_toplevel), so
// server_new_kde_decoration can't resolve a BiomeToplevel for it right away
// (confirmed live: gtk4-image-tool). Stashed on server->xdg.pending_kde_decorations
// and claimed by server_new_xdg_toplevel once a toplevel appears for the
// same surface - see claim_pending_kde_decoration. Without this, such a
// decoration object is silently dropped and the surface falls back to
// toplevel_decorated's "no decoration object" default.
struct BiomePendingKdeDecoration {
    wl_list link;
    wlr_server_decoration *decoration;
    wl_listener destroy;
};

static void pending_kde_decoration_destroy(wl_listener *listener, void *data) {
    (void)data;
    BiomePendingKdeDecoration *pending = wl_container_of(listener, pending, destroy);
    wl_list_remove(&pending->link);
    wl_list_remove(&pending->destroy.link);
    free(pending);
}

// Called from server_new_xdg_toplevel for every new toplevel, in case its
// surface already has a KDE decoration object stashed above.
static void claim_pending_kde_decoration(BiomeServer *server, BiomeToplevel *toplevel, wlr_surface *surface) {
    BiomePendingKdeDecoration *pending, *tmp;
    wl_list_for_each_safe(pending, tmp, &server->xdg.pending_kde_decorations, link) {
        if (pending->decoration->surface == surface) {
            wlr_server_decoration *decoration = pending->decoration;
            wl_list_remove(&pending->link);
            wl_list_remove(&pending->destroy.link);
            free(pending);
            attach_kde_decoration(toplevel, decoration);
            return;
        }
    }
}

// KDE server-decoration (GTK3, Firefox). wlroots echoes back whatever mode
// the client asks for, so this just reads its decision.
static void server_new_kde_decoration(wl_listener *listener, void *data) {
    BiomeServer *server = biome_server_from(listener, BiomeXdgShell, new_kde_decoration);
    auto *decoration = static_cast<wlr_server_decoration *>(data);
    wlr_xdg_surface *xdg_surface = wlr_xdg_surface_try_from_wlr_surface(decoration->surface);
    BiomeToplevel *toplevel = (xdg_surface != nullptr && xdg_surface->toplevel != nullptr)
        ? toplevel_from_xdg(xdg_surface->toplevel) : nullptr;
    if (toplevel != nullptr) {
        attach_kde_decoration(toplevel, decoration);
        return;
    }
    // No xdg_toplevel exists for this surface yet - stash it, see
    // BiomePendingKdeDecoration.
    auto *pending = static_cast<BiomePendingKdeDecoration *>(calloc(1, sizeof(BiomePendingKdeDecoration)));
    pending->decoration = decoration;
    wl_list_insert(&server->xdg.pending_kde_decorations, &pending->link);
    pending->destroy.notify = pending_kde_decoration_destroy;
    wl_signal_add(&decoration->events.destroy, &pending->destroy);
}
