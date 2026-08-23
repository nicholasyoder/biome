// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/xdg_shell.h"

#include "core/cursor.h"
#include "core/input.h"
#include "desktop/decoration_bridge.h"
#include "desktop/toplevel.h"

#include <cassert>
#include <cstdlib>

static void server_new_xdg_toplevel(wl_listener *listener, void *data);
static void server_new_xdg_popup(wl_listener *listener, void *data);
static void server_new_xdg_toplevel_decoration(wl_listener *listener, void *data);
static void server_new_kde_decoration(wl_listener *listener, void *data);
static void claim_pending_kde_decoration(BiomeServer *server, BiomeToplevel *toplevel, wlr_surface *surface);

void xdg_shell_init(BiomeServer *server) {
    wl_list_init(&server->toplevels);
    wl_list_init(&server->pending_kde_decorations);
    server->xdg_shell = wlr_xdg_shell_create(server->display, 3);
    server->new_xdg_toplevel.notify = server_new_xdg_toplevel;
    wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);
    server->new_xdg_popup.notify = server_new_xdg_popup;
    wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_xdg_popup);

    server->xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(server->display);
    server->new_xdg_toplevel_decoration.notify = server_new_xdg_toplevel_decoration;
    wl_signal_add(&server->xdg_decoration_manager->events.new_toplevel_decoration,
        &server->new_xdg_toplevel_decoration);

    // GTK3 clients (which never adopted xdg-decoration above) look for this
    // older KDE protocol instead - see kde_decoration_manager's declaration
    // in server.h.
    server->kde_decoration_manager = wlr_server_decoration_manager_create(server->display);
    wlr_server_decoration_manager_set_default_mode(
        server->kde_decoration_manager, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
    server->new_kde_decoration.notify = server_new_kde_decoration;
    wl_signal_add(&server->kde_decoration_manager->events.new_decoration, &server->new_kde_decoration);
}

static void xdg_toplevel_commit(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, commit);

    if (toplevel->xdg_toplevel->base->initial_commit) {
        // The compositor must reply to an initial commit with a configure
        // so the client can map the surface. 0,0 lets the client pick its
        // own size.
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
        if (toplevel->decoration != nullptr) {
            // By now the client has already sent set_mode/unset_mode (both
            // required to happen before its first surface.commit), so
            // requested_mode already reflects its ask - see
            // xdg_toplevel_decoration_request_mode for why acking couldn't
            // happen there instead: this is the first point a configure is
            // valid to send.
            toplevel->xdg_client_side_decorated = toplevel->decoration->requested_mode ==
                WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
            wlr_xdg_toplevel_decoration_v1_set_mode(toplevel->decoration,
                toplevel->xdg_client_side_decorated
                    ? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
                    : WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        } else if (toplevel->kde_decoration != nullptr) {
            // apply_kde_decoration_mode already resolved xdg_client_side_decorated
            // when this KDE object was created (server_new_kde_decoration runs
            // before this commit for a pre-map object) and deferred the
            // content_tree/render update to here, since it ran before
            // base->initialized was set - nothing left to do.
        } else {
            // Neither protocol was ever negotiated. Per both protocols' specs,
            // absence of a decoration object means client-side decorated - see
            // toplevel_decorated's comment - not Biome's own frame. This is
            // also the path libadwaita/GNOME HeaderBar apps hit (org.gnome.baobab
            // confirmed live): GTK4's gdk_wayland_toplevel_set_decorated() has a
            // bug where self->decorated is never initialized to GTK's documented
            // TRUE default, so a HeaderBar window's desired value (TRUE &&
            // !client_decorated == FALSE) equals that already-FALSE default and
            // its `if (self->decorated == decorated) return;` guard silently
            // no-ops the decoration request - it never reaches the compositor,
            // so no decoration object of either kind ever gets created for
            // these windows. Nothing Biome can do about the upstream bug, but
            // this default now happens to produce the right result anyway.
            toplevel->xdg_client_side_decorated = true;
            wlr_log(WLR_DEBUG, "xdg-decoration: app_id=%s created no decoration object -> client-side",
                toplevel->xdg_toplevel->app_id ? toplevel->xdg_toplevel->app_id : "(null)");
        }
        // content_tree was positioned assuming Biome's own frame back in
        // server_new_xdg_toplevel, before the decoration object (if any)
        // even existed - now that the mode is settled, correct it.
        wlr_scene_node_set_position(&toplevel->content_tree->node,
            decoration_border_width(toplevel, toplevel->maximized),
            decoration_titlebar_height(toplevel, toplevel->maximized));
        return;
    }

    // If this commit lands mid-resize-grab and the drag is moving the
    // left/top edge, this is the buffer matching the size last requested -
    // reposition now, anchored off the fixed opposite edge captured in
    // grab_geobox at grab start, so position and content land together. See
    // cursor.cpp's process_cursor_resize for why the eager position update
    // was removed for xdg-shell toplevels.
    BiomeServer *server = toplevel->server;
    if (server->cursor_mode == BiomeCursorMode::Resize && server->grabbed_toplevel == toplevel &&
            (server->resize_edges & (WLR_EDGE_LEFT | WLR_EDGE_TOP))) {
        wlr_box geo;
        toplevel_get_geometry(toplevel, &geo);
        int x = static_cast<int>(toplevel->scene_tree->node.x);
        int y = static_cast<int>(toplevel->scene_tree->node.y);
        if (server->resize_edges & WLR_EDGE_LEFT) {
            int content_right = server->grab_geobox.x + server->grab_geobox.width;
            x = content_right - geo.width - geo.x - decoration_border_width(toplevel, toplevel->maximized);
        }
        if (server->resize_edges & WLR_EDGE_TOP) {
            int content_bottom = server->grab_geobox.y + server->grab_geobox.height;
            y = content_bottom - geo.height - geo.y - decoration_titlebar_height(toplevel, toplevel->maximized);
        }
        wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
    }

    if (toplevel->maximize_reposition_pending) {
        // Same idea as the resize case above: wait for a commit whose size
        // actually differs from before set_toplevel_maximized requested the
        // change - see maximize_reposition_pending's declaration.
        wlr_box geo;
        toplevel_get_geometry(toplevel, &geo);
        bool resolves = geo.width != toplevel->maximize_pending_old_width ||
            geo.height != toplevel->maximize_pending_old_height;
        if (resolves) {
            wlr_scene_node_set_position(&toplevel->scene_tree->node,
                toplevel->maximize_pending_x, toplevel->maximize_pending_y);
            toplevel->maximize_reposition_pending = false;
            // The decoration (and its buttons) just moved out from under a
            // cursor that may not have moved since the click that requested
            // this - see refresh_decoration_hover's comment.
            refresh_decoration_hover(server);
        }
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
    if (toplevel->decoration != nullptr) {
        wl_list_remove(&toplevel->decoration_destroy.link);
        wl_list_remove(&toplevel->decoration_request_mode.link);
    }
    if (toplevel->kde_decoration != nullptr) {
        wl_list_remove(&toplevel->kde_decoration_destroy.link);
        wl_list_remove(&toplevel->kde_decoration_mode.link);
    }

    // scene_tree isn't tied to the xdg_surface's own lifecycle, so it has to
    // be destroyed explicitly - recursively destroys content_tree and the
    // decoration buffer too.
    wlr_scene_node_destroy(&toplevel->scene_tree->node);

    destroy_toplevel_decoration(toplevel);
    clear_decoration_tracking(toplevel->server, toplevel);
    remove_toplevel_from_switcher(toplevel->server, toplevel);
    free(toplevel);
}

// A client requests this to begin an interactive resize, typically from its
// own CSD.
static void xdg_toplevel_request_resize(wl_listener *listener, void *data) {
    auto *event = static_cast<wlr_xdg_toplevel_resize_event *>(data);
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
    begin_interactive(toplevel, BiomeCursorMode::Resize, event->edges, true);
}

// Maximize and unmaximize both go through this one signal, distinguished by
// requested.maximized. Ignored before the initial commit, letting the
// client finish its initial setup.
static void xdg_toplevel_request_maximize(wl_listener *listener, void *data) {
    (void)data;
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

// Just as with request_maximize, a configure reply is required here.
static void xdg_toplevel_request_fullscreen(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);
    if (toplevel->xdg_toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
    }
}

// Unlike maximize/fullscreen, xdg-shell has no configure state for
// minimized and thus no acknowledgment requirement - just act on it.
static void xdg_toplevel_request_minimize(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_minimize);
    set_toplevel_minimized(toplevel, toplevel->xdg_toplevel->requested.minimized);
}

static void server_new_xdg_toplevel(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, new_xdg_toplevel);
    auto *xdg_toplevel = static_cast<wlr_xdg_toplevel *>(data);

    auto *toplevel = static_cast<BiomeToplevel *>(calloc(1, sizeof(BiomeToplevel)));
    toplevel->server = server;
    toplevel->type = BiomeToplevelType::Xdg;
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
    BiomePopup *popup = wl_container_of(listener, popup, commit);

    if (popup->xdg_popup->base->initial_commit) {
        // The compositor must reply to an initial commit with a configure so
        // the client can map the surface; Biome sends an empty one.
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
    }
}

static void xdg_popup_destroy(wl_listener *listener, void *data) {
    (void)data;
    BiomePopup *popup = wl_container_of(listener, popup, destroy);

    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);

    free(popup);
}

static void server_new_xdg_popup(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, new_xdg_popup);
    auto *xdg_popup = static_cast<wlr_xdg_popup *>(data);

    auto *popup = static_cast<BiomePopup *>(calloc(1, sizeof(BiomePopup)));
    popup->xdg_popup = xdg_popup;

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
        // of hanging off it (mirrors desktop/layer_shell.cpp's identical fix
        // for layer-shell-owned popups - see that file for why this call is
        // necessary at all: without it, the positioner's own
        // constraint_adjustment has no box to slide within and is a no-op).
        // wlr_scene_node_coords() walks the scene tree for the parent's
        // absolute position regardless of whether it's a toplevel or (for a
        // nested popup-on-popup) another popup, so this works uniformly
        // rather than assuming a specific parent shape.
        int parent_lx = 0, parent_ly = 0;
        wlr_scene_node_coords(&parent_tree->node, &parent_lx, &parent_ly);
        wlr_output *wlr_output = wlr_output_layout_output_at(server->output_layout, parent_lx, parent_ly);
        if (wlr_output != nullptr) {
            wlr_box output_box = {};
            wlr_output_layout_get_box(server->output_layout, wlr_output, &output_box);
            if (!wlr_box_empty(&output_box)) {
                wlr_box unconstrain_box = output_box;
                unconstrain_box.x -= parent_lx;
                unconstrain_box.y -= parent_ly;
                wlr_xdg_popup_unconstrain_from_box(xdg_popup, &unconstrain_box);
            }
        }
    }

    popup->commit.notify = xdg_popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

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
        wlr_scene_node_set_position(&toplevel->content_tree->node,
            decoration_border_width(toplevel, toplevel->maximized),
            decoration_titlebar_height(toplevel, toplevel->maximized));
        render_toplevel_decoration(toplevel);
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
    wlr_scene_node_set_position(&toplevel->content_tree->node,
        decoration_border_width(toplevel, toplevel->maximized),
        decoration_titlebar_height(toplevel, toplevel->maximized));
    render_toplevel_decoration(toplevel);
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
// (confirmed live: gtk4-image-tool). Stashed on server->pending_kde_decorations
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
    wl_list_for_each_safe(pending, tmp, &server->pending_kde_decorations, link) {
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

// The org_kde_kwin_server_decoration protocol GTK3 (and thus Firefox) uses
// instead of xdg-decoration - see kde_decoration_manager's declaration in
// server.h. Unlike xdg-decoration, this protocol has no compositor-side
// override: wlroots auto-accepts whatever mode the client requests and
// echoes it straight back (see wlr_server_decoration.c) - so honoring the
// client's request here just means finally reading what it already decided,
// there's no negotiation to perform.
static void server_new_kde_decoration(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, new_kde_decoration);
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
    wl_list_insert(&server->pending_kde_decorations, &pending->link);
    pending->destroy.notify = pending_kde_decoration_destroy;
    wl_signal_add(&decoration->events.destroy, &pending->destroy);
}
