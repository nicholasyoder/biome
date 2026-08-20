// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/xdg_shell.h"

#include "core/cursor.h"
#include "desktop/decoration_bridge.h"
#include "desktop/toplevel.h"

#include <cassert>
#include <cstdlib>

static void server_new_xdg_toplevel(wl_listener *listener, void *data);
static void server_new_xdg_popup(wl_listener *listener, void *data);
static void server_new_xdg_toplevel_decoration(wl_listener *listener, void *data);

void xdg_shell_init(BiomeServer *server) {
    // Set up xdg-shell version 3. The xdg-shell is a Wayland protocol which
    // is used for application windows.
    wl_list_init(&server->toplevels);
    server->xdg_shell = wlr_xdg_shell_create(server->display, 3);
    server->new_xdg_toplevel.notify = server_new_xdg_toplevel;
    wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);
    server->new_xdg_popup.notify = server_new_xdg_popup;
    wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_xdg_popup);

    server->xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(server->display);
    server->new_xdg_toplevel_decoration.notify = server_new_xdg_toplevel_decoration;
    wl_signal_add(&server->xdg_decoration_manager->events.new_toplevel_decoration,
        &server->new_xdg_toplevel_decoration);
}

static void xdg_toplevel_commit(wl_listener *listener, void *data) {
    (void)data;
    // Called when a new surface state is committed.
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, commit);

    if (toplevel->xdg_toplevel->base->initial_commit) {
        // When an xdg_surface performs an initial commit, the compositor
        // must reply with a configure so the client can map the surface.
        // Biome configures the xdg_toplevel with 0,0 size to let the client
        // pick the dimensions itself.
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
        if (toplevel->pending_decoration != nullptr) {
            wl_list_remove(&toplevel->pending_decoration_destroy.link);
            wlr_xdg_toplevel_decoration_v1_set_mode(
                toplevel->pending_decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
            toplevel->pending_decoration = nullptr;
        }
        return;
    }
    // The client may have resized itself, or changed its title, outside of
    // an interactive grab - keep the decoration in sync.
    render_toplevel_decoration(toplevel);
}

static void xdg_toplevel_set_title(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, set_title);
    render_toplevel_decoration(toplevel);
}

static void xdg_toplevel_destroy(wl_listener *listener, void *data) {
    (void)data;
    // Called when the xdg_toplevel is destroyed.
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, destroy);

    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->commit.link);
    wl_list_remove(&toplevel->set_title.link);
    wl_list_remove(&toplevel->destroy.link);
    wl_list_remove(&toplevel->request_move.link);
    wl_list_remove(&toplevel->request_resize.link);
    wl_list_remove(&toplevel->request_maximize.link);
    wl_list_remove(&toplevel->request_fullscreen.link);
    wl_list_remove(&toplevel->request_minimize.link);
    if (toplevel->pending_decoration != nullptr) {
        // The toplevel is being destroyed before ever reaching its initial
        // commit (e.g. a client that creates a decoration object then
        // disconnects) - drop the listener registered on the decoration's
        // own destroy signal before this toplevel (and that listener along
        // with it) is freed below.
        wl_list_remove(&toplevel->pending_decoration_destroy.link);
    }

    // scene_tree is a plain wlr_scene_tree_create() we allocated ourselves
    // (to hold the decoration), not one tied to the xdg_surface's own
    // lifecycle - we have to destroy it explicitly. This recursively
    // destroys content_tree and the decoration buffer too.
    wlr_scene_node_destroy(&toplevel->scene_tree->node);

    clear_decoration_tracking(toplevel->server, toplevel);
    free(toplevel);
}

static void xdg_toplevel_request_resize(wl_listener *listener, void *data) {
    // This event is raised when a client would like to begin an interactive
    // resize, typically because the user clicked on their client-side
    // decorations. Note that a more sophisticated compositor should check
    // the provided serial against a list of button press serials sent to
    // this client, to prevent the client from requesting this whenever they
    // want.
    auto *event = static_cast<wlr_xdg_toplevel_resize_event *>(data);
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
    begin_interactive(toplevel, BiomeCursorMode::Resize, event->edges, true);
}

static void xdg_toplevel_request_maximize(wl_listener *listener, void *data) {
    (void)data;
    // This event is raised when a client would like to maximize (or
    // unmaximize - both go through this one signal, distinguished by
    // requested.maximized) itself, typically its own titlebar's maximize
    // button for a CSD client. If the request was sent before an initial
    // commit, don't do anything and let the client finish its initial setup.
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_maximize);
    if (!toplevel->xdg_toplevel->base->initialized) {
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

static void xdg_toplevel_request_fullscreen(wl_listener *listener, void *data) {
    (void)data;
    // Just as with request_maximize, we must send a configure here.
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);
    if (toplevel->xdg_toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
    }
}

static void xdg_toplevel_request_minimize(wl_listener *listener, void *data) {
    (void)data;
    // Unlike maximize/fullscreen, xdg-shell has no configure state for
    // minimized and thus no acknowledgment requirement - just act on it.
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_minimize);
    set_toplevel_minimized(toplevel, toplevel->xdg_toplevel->requested.minimized);
}

static void server_new_xdg_toplevel(wl_listener *listener, void *data) {
    // This event is raised when a client creates a new toplevel (application window).
    BiomeServer *server = wl_container_of(listener, server, new_xdg_toplevel);
    auto *xdg_toplevel = static_cast<wlr_xdg_toplevel *>(data);

    // Allocate a BiomeToplevel for this surface
    auto *toplevel = static_cast<BiomeToplevel *>(calloc(1, sizeof(BiomeToplevel)));
    toplevel->server = server;
    toplevel->type = BiomeToplevelType::Xdg;
    toplevel->xdg_toplevel = xdg_toplevel;

    toplevel->scene_tree = wlr_scene_tree_create(&toplevel->server->scene->tree);
    toplevel->scene_tree->node.data = toplevel;
    create_toplevel_decoration(toplevel);

    toplevel->content_tree =
        wlr_scene_xdg_surface_create(toplevel->scene_tree, xdg_toplevel->base);
    toplevel->content_tree->node.data = toplevel;
    wlr_scene_node_set_position(&toplevel->content_tree->node,
        decoration_border_width(), decoration_titlebar_height());
    xdg_toplevel->base->data = toplevel->content_tree;

    // Listen to the various events it can emit
    toplevel->map.notify = toplevel_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
    toplevel->unmap.notify = toplevel_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
    toplevel->commit.notify = xdg_toplevel_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
    toplevel->set_title.notify = xdg_toplevel_set_title;
    wl_signal_add(&xdg_toplevel->events.set_title, &toplevel->set_title);

    toplevel->destroy.notify = xdg_toplevel_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

    toplevel->request_move.notify = toplevel_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
    toplevel->request_resize.notify = xdg_toplevel_request_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
    toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
    wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
    toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
    toplevel->request_minimize.notify = xdg_toplevel_request_minimize;
    wl_signal_add(&xdg_toplevel->events.request_minimize, &toplevel->request_minimize);
}

static void xdg_popup_commit(wl_listener *listener, void *data) {
    (void)data;
    // Called when a new surface state is committed.
    BiomePopup *popup = wl_container_of(listener, popup, commit);

    if (popup->xdg_popup->base->initial_commit) {
        // When an xdg_surface performs an initial commit, the compositor
        // must reply with a configure so the client can map the surface.
        // Biome sends an empty configure. A more sophisticated compositor
        // might change an xdg_popup's geometry to ensure it's not
        // positioned off-screen, for example.
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
    }
}

static void xdg_popup_destroy(wl_listener *listener, void *data) {
    (void)data;
    // Called when the xdg_popup is destroyed.
    BiomePopup *popup = wl_container_of(listener, popup, destroy);

    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);

    free(popup);
}

static void server_new_xdg_popup(wl_listener *listener, void *data) {
    (void)listener;
    // This event is raised when a client creates a new popup.
    auto *xdg_popup = static_cast<wlr_xdg_popup *>(data);

    auto *popup = static_cast<BiomePopup *>(calloc(1, sizeof(BiomePopup)));
    popup->xdg_popup = xdg_popup;

    // We must add xdg popups to the scene graph so they get rendered. The
    // wlroots scene graph provides a helper for this, but to use it we must
    // provide the proper parent scene node of the xdg popup. To enable
    // this, we always set the user data field of xdg_surfaces to the
    // corresponding scene node.
    wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
    assert(parent != nullptr);
    auto *parent_tree = static_cast<wlr_scene_tree *>(parent->data);
    xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

    popup->commit.notify = xdg_popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

    popup->destroy.notify = xdg_popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

static void pending_decoration_destroy_handler(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, pending_decoration_destroy);
    wl_list_remove(&toplevel->pending_decoration_destroy.link);
    toplevel->pending_decoration = nullptr;
}

static void server_new_xdg_toplevel_decoration(wl_listener *listener, void *data) {
    (void)listener;
    // A client created an xdg_toplevel_decoration object, whether to ask
    // for server-side or client-side decorations. Biome always draws its
    // own (decoration/), so the client's request is irrelevant - always
    // force server-side and skip client-side entirely. This is what
    // resolves the CSD-double-decoration rough edge: a CSD-capable client
    // (foot, GTK apps) that honors this now won't draw its own frame on
    // top of Biome's.
    auto *decoration = static_cast<wlr_xdg_toplevel_decoration_v1 *>(data);
    if (decoration->toplevel->base->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        return;
    }
    // Not initialized yet - the common case, since clients create their
    // decoration object before their first surface commit. Setting the
    // mode now would hit wlroots' "configure scheduled for an
    // uninitialized xdg_surface" guard and get silently dropped. Defer to
    // xdg_toplevel_commit's initial_commit handling instead, which is
    // guaranteed to run after initialization.
    BiomeToplevel *toplevel = toplevel_from_xdg(decoration->toplevel);
    if (toplevel == nullptr) {
        return;
    }
    toplevel->pending_decoration = decoration;
    toplevel->pending_decoration_destroy.notify = pending_decoration_destroy_handler;
    wl_signal_add(&decoration->events.destroy, &toplevel->pending_decoration_destroy);
}
