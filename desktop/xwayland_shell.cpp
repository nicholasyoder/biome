// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/xwayland_shell.h"

#include "core/cursor.h"
#include "core/input.h"
#include "desktop/decoration_bridge.h"
#include "desktop/foreign_toplevel.h"
#include "desktop/toplevel.h"

#include <cstdlib>

static void server_new_xwayland_surface(wl_listener *listener, void *data);
static void server_xwayland_ready(wl_listener *listener, void *data);

void xwayland_init(BiomeServer *server, wlr_compositor *compositor) {
    server->xwayland = wlr_xwayland_create(server->display, compositor, true);
    if (server->xwayland == nullptr) {
        wlr_log(WLR_ERROR, "failed to start Xwayland; X11 apps will not work");
        return;
    }
    server->new_xwayland_surface.notify = server_new_xwayland_surface;
    wl_signal_add(&server->xwayland->events.new_surface, &server->new_xwayland_surface);
    server->xwayland_ready.notify = server_xwayland_ready;
    wl_signal_add(&server->xwayland->events.ready, &server->xwayland_ready);
    wlr_xwayland_set_seat(server->xwayland, server->seat);
    setenv("DISPLAY", server->xwayland->display_name, true);
}

// --- Xwayland: managed toplevels -------------------------------------

// The wlr_surface backing this X11 window now exists - hook up map/unmap
// (shared with xdg-shell) and create its scene node.
static void xwayland_toplevel_associate(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, associate);
    wlr_xwayland_surface *xsurface = toplevel->xwayland_surface;

    toplevel->content_tree =
        wlr_scene_subsurface_tree_create(toplevel->scene_tree, xsurface->surface);
    toplevel->content_tree->node.data = toplevel;
    // Not necessarily a fresh toplevel - an Xwayland surface can dissociate
    // and re-associate while staying mapped/maximized, so this has to use
    // the toplevel's actual current state rather than assuming false.
    wlr_scene_node_set_position(&toplevel->content_tree->node,
        decoration_border_width(toplevel, toplevel->maximized), decoration_titlebar_height(toplevel, toplevel->maximized));
    xsurface->data = toplevel->content_tree;

    toplevel->map.notify = toplevel_map;
    wl_signal_add(&xsurface->surface->events.map, &toplevel->map);
    toplevel->unmap.notify = toplevel_unmap;
    wl_signal_add(&xsurface->surface->events.unmap, &toplevel->unmap);
}

// The wlr_surface is going away, but the X11 window wrapper may persist
// (re-associated later) - wlroots destroys the scene node tied to the
// surface itself, this just drops our listeners.
static void xwayland_toplevel_dissociate(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, dissociate);
    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    toplevel->xwayland_surface->data = nullptr;
    // scene_tree (the container, and its decoration) persists in case this
    // surface re-associates later.
    toplevel->content_tree = nullptr;
}

static void xwayland_toplevel_set_title(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, set_title);
    render_toplevel_decoration(toplevel);
    foreign_toplevel_update_title_app_id(toplevel);
}

static void xwayland_toplevel_destroy(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, destroy);

    wl_list_remove(&toplevel->associate.link);
    wl_list_remove(&toplevel->dissociate.link);
    wl_list_remove(&toplevel->set_title.link);
    wl_list_remove(&toplevel->destroy.link);
    wl_list_remove(&toplevel->request_move.link);
    wl_list_remove(&toplevel->request_resize.link);
    wl_list_remove(&toplevel->request_maximize.link);
    wl_list_remove(&toplevel->request_fullscreen.link);
    wl_list_remove(&toplevel->request_minimize.link);
    wl_list_remove(&toplevel->request_configure.link);

    // scene_tree was created up front in server_new_xwayland_surface and
    // outlives any single associate/dissociate cycle, so it's destroyed here.
    wlr_scene_node_destroy(&toplevel->scene_tree->node);

    destroy_toplevel_decoration(toplevel);
    clear_decoration_tracking(toplevel->server, toplevel);
    remove_toplevel_from_switcher(toplevel->server, toplevel);
    free(toplevel);
}

static void xwayland_toplevel_request_resize(wl_listener *listener, void *data) {
    auto *event = static_cast<wlr_xwayland_resize_event *>(data);
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
    begin_interactive(toplevel, BiomeCursorMode::Resize, event->edges, true);
}

// X11 tracks maximize as two independent axes (_NET_WM_STATE_MAXIMIZED_
// VERT/HORZ); Biome doesn't offer a partial-axis maximize, so treat
// "maximized" as both being requested together.
static void xwayland_toplevel_request_maximize(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_maximize);
    bool requested = toplevel->xwayland_surface->maximized_vert &&
        toplevel->xwayland_surface->maximized_horz;
    set_toplevel_maximized(toplevel, requested);
}

static void xwayland_toplevel_request_fullscreen(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);
    wlr_xwayland_surface_set_fullscreen(toplevel->xwayland_surface, false);
}

static void xwayland_toplevel_request_minimize(wl_listener *listener, void *data) {
    auto *event = static_cast<wlr_xwayland_minimize_event *>(data);
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_minimize);
    set_toplevel_minimized(toplevel, event->minimize);
}

// X11 clients can ask to move/resize themselves outside of an interactive
// grab (e.g. a terminal resizing to fit its font) - honored as-is, keeping
// the scene node in sync.
static void xwayland_toplevel_request_configure(wl_listener *listener, void *data) {
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_configure);
    auto *event = static_cast<wlr_xwayland_surface_configure_event *>(data);

    wlr_xwayland_surface_configure(toplevel->xwayland_surface,
        event->x, event->y, event->width, event->height);
    if (toplevel->content_tree) {
        wlr_scene_node_set_position(&toplevel->scene_tree->node,
            event->x - decoration_border_width(toplevel, toplevel->maximized),
            event->y - decoration_titlebar_height(toplevel, toplevel->maximized));
        render_toplevel_decoration(toplevel);
    }
}

// --- Xwayland: override-redirect (unmanaged) surfaces -----------------

static void unmanaged_associate(wl_listener *listener, void *data) {
    (void)data;
    BiomeUnmanaged *surface = wl_container_of(listener, surface, associate);
    wlr_xwayland_surface *xsurface = surface->xwayland_surface;

    // Parented under layers.toplevels (not scene->tree directly) so it's
    // structurally below layers.session_lock like every other toplevel-ish
    // piece of content - see BiomeServer::layers' doc comment in server.h.
    // An override-redirect surface (X11 popup/menu/tooltip) has no
    // BiomeToplevel, so update_toplevel_visibility()'s per-toplevel
    // visibility logic never runs for it; this reparenting is what covers
    // it instead, with no runtime session_locked check needed here anymore.
    surface->scene_tree =
        wlr_scene_subsurface_tree_create(surface->server->layers.toplevels, xsurface->surface);
    wlr_scene_node_set_position(&surface->scene_tree->node, xsurface->x, xsurface->y);

    surface->map.notify = [](wl_listener *l, void *d) {
        (void)d;
        BiomeUnmanaged *s = wl_container_of(l, s, map);
        // raise_to_top only reorders siblings within layers.toplevels (its
        // scene_tree's parent - see unmanaged_associate above), so unlike
        // before this reparenting, it can no longer put this surface above
        // layers.session_lock regardless of lock state - no check needed
        // for that anymore. Keyboard focus is a separate, seat-level
        // concern structural z-order doesn't touch, though: while locked,
        // the lock surface holds focus (desktop/session_lock.cpp) and
        // nothing else may take it, so that grab still needs its own check.
        wlr_scene_node_raise_to_top(&s->scene_tree->node);
        if (!s->server->session_locked && wlr_xwayland_or_surface_wants_focus(s->xwayland_surface)) {
            grant_keyboard_focus_to_non_toplevel(s->server, s->xwayland_surface->surface);
        }
    };
    wl_signal_add(&xsurface->surface->events.map, &surface->map);

    surface->unmap.notify = [](wl_listener *l, void *d) {
        (void)d;
        BiomeUnmanaged *s = wl_container_of(l, s, unmap);
        wlr_seat *seat = s->server->seat;
        if (seat->keyboard_state.focused_surface != s->xwayland_surface->surface) {
            return;
        }
        // Hand focus back to the topmost managed toplevel, if any.
        if (!wl_list_empty(&s->server->toplevels)) {
            BiomeToplevel *top = wl_container_of(s->server->toplevels.next, top, link);
            focus_toplevel(top, toplevel_surface(top));
        } else {
            wlr_seat_keyboard_notify_clear_focus(seat);
        }
    };
    wl_signal_add(&xsurface->surface->events.unmap, &surface->unmap);
}

static void unmanaged_dissociate(wl_listener *listener, void *data) {
    (void)data;
    BiomeUnmanaged *surface = wl_container_of(listener, surface, dissociate);
    wl_list_remove(&surface->map.link);
    wl_list_remove(&surface->unmap.link);
    surface->scene_tree = nullptr;
}

static void unmanaged_destroy(wl_listener *listener, void *data) {
    (void)data;
    BiomeUnmanaged *surface = wl_container_of(listener, surface, destroy);
    wl_list_remove(&surface->associate.link);
    wl_list_remove(&surface->dissociate.link);
    wl_list_remove(&surface->destroy.link);
    wl_list_remove(&surface->request_configure.link);
    free(surface);
}

static void unmanaged_request_configure(wl_listener *listener, void *data) {
    BiomeUnmanaged *surface = wl_container_of(listener, surface, request_configure);
    auto *event = static_cast<wlr_xwayland_surface_configure_event *>(data);
    wlr_xwayland_surface_configure(surface->xwayland_surface,
        event->x, event->y, event->width, event->height);
    if (surface->scene_tree) {
        wlr_scene_node_set_position(&surface->scene_tree->node, event->x, event->y);
    }
}

static void server_new_xwayland_surface(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, new_xwayland_surface);
    auto *xsurface = static_cast<wlr_xwayland_surface *>(data);

    if (xsurface->override_redirect) {
        auto *surface = static_cast<BiomeUnmanaged *>(calloc(1, sizeof(BiomeUnmanaged)));
        surface->server = server;
        surface->xwayland_surface = xsurface;

        surface->associate.notify = unmanaged_associate;
        wl_signal_add(&xsurface->events.associate, &surface->associate);
        surface->dissociate.notify = unmanaged_dissociate;
        wl_signal_add(&xsurface->events.dissociate, &surface->dissociate);
        surface->destroy.notify = unmanaged_destroy;
        wl_signal_add(&xsurface->events.destroy, &surface->destroy);
        surface->request_configure.notify = unmanaged_request_configure;
        wl_signal_add(&xsurface->events.request_configure, &surface->request_configure);
        return;
    }

    auto *toplevel = static_cast<BiomeToplevel *>(calloc(1, sizeof(BiomeToplevel)));
    toplevel->server = server;
    toplevel->type = BiomeToplevelType::Xwayland;
    toplevel->xwayland_surface = xsurface;

    // scene_tree is created up front (unlike content_tree, which comes and
    // goes with associate/dissociate) so the decoration survives
    // re-association.
    toplevel->scene_tree = wlr_scene_tree_create(server->layers.toplevels);
    toplevel->scene_tree->node.data = toplevel;
    create_toplevel_decoration(toplevel);

    toplevel->associate.notify = xwayland_toplevel_associate;
    wl_signal_add(&xsurface->events.associate, &toplevel->associate);
    toplevel->dissociate.notify = xwayland_toplevel_dissociate;
    wl_signal_add(&xsurface->events.dissociate, &toplevel->dissociate);
    toplevel->destroy.notify = xwayland_toplevel_destroy;
    wl_signal_add(&xsurface->events.destroy, &toplevel->destroy);
    toplevel->set_title.notify = xwayland_toplevel_set_title;
    wl_signal_add(&xsurface->events.set_title, &toplevel->set_title);
    toplevel->request_move.notify = toplevel_request_move;
    wl_signal_add(&xsurface->events.request_move, &toplevel->request_move);
    toplevel->request_resize.notify = xwayland_toplevel_request_resize;
    wl_signal_add(&xsurface->events.request_resize, &toplevel->request_resize);
    toplevel->request_maximize.notify = xwayland_toplevel_request_maximize;
    wl_signal_add(&xsurface->events.request_maximize, &toplevel->request_maximize);
    toplevel->request_fullscreen.notify = xwayland_toplevel_request_fullscreen;
    wl_signal_add(&xsurface->events.request_fullscreen, &toplevel->request_fullscreen);
    toplevel->request_minimize.notify = xwayland_toplevel_request_minimize;
    wl_signal_add(&xsurface->events.request_minimize, &toplevel->request_minimize);
    toplevel->request_configure.notify = xwayland_toplevel_request_configure;
    wl_signal_add(&xsurface->events.request_configure, &toplevel->request_configure);
}

// Give Xwayland a cursor image as soon as it's up - without this, X11
// clients show no cursor at all until they set their own.
static void server_xwayland_ready(wl_listener *listener, void *data) {
    (void)data;
    BiomeServer *server = wl_container_of(listener, server, xwayland_ready);
    wlr_xcursor *xcursor = wlr_xcursor_manager_get_xcursor(server->cursor_mgr, "default", 1.0f);
    if (xcursor != nullptr && xcursor->image_count > 0) {
        wlr_xcursor_image *image = xcursor->images[0];
        wlr_xwayland_set_cursor(server->xwayland, image->buffer,
            image->width * 4, image->width, image->height,
            static_cast<int32_t>(image->hotspot_x), static_cast<int32_t>(image->hotspot_y));
    }

    // Atom-initialize the EWMH connection used for _NET_WM_ICON lookup
    // (desktop/app_icon.h) - the underlying xcb connection is only valid
    // from this event onward.
    xcb_connection_t *conn = wlr_xwayland_get_xwm_connection(server->xwayland);
    if (conn != nullptr) {
        xcb_intern_atom_cookie_t *cookies = xcb_ewmh_init_atoms(conn, &server->ewmh);
        server->ewmh_ready = xcb_ewmh_init_atoms_replies(&server->ewmh, cookies, nullptr) != 0;
    }
}
