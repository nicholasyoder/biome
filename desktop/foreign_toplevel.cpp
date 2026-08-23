// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/foreign_toplevel.h"

#include "desktop/toplevel.h"

// Per-BiomeToplevel wrapper around wlr_foreign_toplevel_handle_v1 - not
// exposed outside this file, same shape as session_lock.cpp's
// BiomeLockSurface. toplevel is kept so the request_* listeners (which only
// get the handle, not the BiomeToplevel, from wlroots) can act on the real
// window.
struct BiomeForeignToplevel {
    BiomeToplevel *toplevel = nullptr;
    wlr_foreign_toplevel_handle_v1 *handle = nullptr;
    wl_listener request_maximize = {};
    wl_listener request_minimize = {};
    wl_listener request_activate = {};
    wl_listener request_fullscreen = {};
    wl_listener request_close = {};
};

static void handle_request_maximize(wl_listener *listener, void *data) {
    BiomeForeignToplevel *wrapper = wl_container_of(listener, wrapper, request_maximize);
    auto *event = static_cast<wlr_foreign_toplevel_handle_v1_maximized_event *>(data);
    set_toplevel_maximized(wrapper->toplevel, event->maximized);
}

static void handle_request_minimize(wl_listener *listener, void *data) {
    BiomeForeignToplevel *wrapper = wl_container_of(listener, wrapper, request_minimize);
    auto *event = static_cast<wlr_foreign_toplevel_handle_v1_minimized_event *>(data);
    set_toplevel_minimized(wrapper->toplevel, event->minimized);
}

static void handle_request_activate(wl_listener *listener, void *data) {
    (void)data;
    BiomeForeignToplevel *wrapper = wl_container_of(listener, wrapper, request_activate);
    focus_toplevel(wrapper->toplevel, toplevel_surface(wrapper->toplevel));
}

// Biome has no fullscreen support anywhere (see this module's header
// comment) - a request here can't be honored, so it's just ignored. The
// handle's fullscreen state bit is never set in the first place, so no
// reply/no-op signal is needed either.
static void handle_request_fullscreen(wl_listener *listener, void *data) {
    (void)listener;
    (void)data;
}

static void handle_request_close(wl_listener *listener, void *data) {
    (void)data;
    BiomeForeignToplevel *wrapper = wl_container_of(listener, wrapper, request_close);
    close_toplevel(wrapper->toplevel);
}

void foreign_toplevel_init(BiomeServer *server) {
    server->foreign_toplevel_manager = wlr_foreign_toplevel_manager_v1_create(server->display);
}

void foreign_toplevel_create(BiomeToplevel *toplevel) {
    BiomeServer *server = toplevel->server;
    if (server->foreign_toplevel_manager == nullptr) {
        return;
    }

    auto *wrapper = static_cast<BiomeForeignToplevel *>(calloc(1, sizeof(BiomeForeignToplevel)));
    wrapper->toplevel = toplevel;
    wrapper->handle = wlr_foreign_toplevel_handle_v1_create(server->foreign_toplevel_manager);
    wrapper->handle->data = wrapper;
    toplevel->foreign_toplevel = wrapper;

    wrapper->request_maximize.notify = handle_request_maximize;
    wl_signal_add(&wrapper->handle->events.request_maximize, &wrapper->request_maximize);
    wrapper->request_minimize.notify = handle_request_minimize;
    wl_signal_add(&wrapper->handle->events.request_minimize, &wrapper->request_minimize);
    wrapper->request_activate.notify = handle_request_activate;
    wl_signal_add(&wrapper->handle->events.request_activate, &wrapper->request_activate);
    wrapper->request_fullscreen.notify = handle_request_fullscreen;
    wl_signal_add(&wrapper->handle->events.request_fullscreen, &wrapper->request_fullscreen);
    wrapper->request_close.notify = handle_request_close;
    wl_signal_add(&wrapper->handle->events.request_close, &wrapper->request_close);

    foreign_toplevel_update_title_app_id(toplevel);
    foreign_toplevel_sync_state(toplevel);

    wlr_output *output = wlr_output_layout_output_at(
        server->output_layout, toplevel->scene_tree->node.x, toplevel->scene_tree->node.y);
    if (output != nullptr) {
        wlr_foreign_toplevel_handle_v1_output_enter(wrapper->handle, output);
    }
}

void foreign_toplevel_destroy(BiomeToplevel *toplevel) {
    BiomeForeignToplevel *wrapper = toplevel->foreign_toplevel;
    if (wrapper == nullptr) {
        return;
    }
    toplevel->foreign_toplevel = nullptr;

    wl_list_remove(&wrapper->request_maximize.link);
    wl_list_remove(&wrapper->request_minimize.link);
    wl_list_remove(&wrapper->request_activate.link);
    wl_list_remove(&wrapper->request_fullscreen.link);
    wl_list_remove(&wrapper->request_close.link);

    // Sends the `closed` event to any client still holding this handle.
    wlr_foreign_toplevel_handle_v1_destroy(wrapper->handle);
    free(wrapper);
}

void foreign_toplevel_update_title_app_id(BiomeToplevel *toplevel) {
    BiomeForeignToplevel *wrapper = toplevel->foreign_toplevel;
    if (wrapper == nullptr) {
        return;
    }

    const char *title = "";
    const char *app_id = "";
    if (toplevel->type == BiomeToplevelType::Xdg) {
        title = toplevel->xdg_toplevel->title != nullptr ? toplevel->xdg_toplevel->title : "";
        app_id = toplevel->xdg_toplevel->app_id != nullptr ? toplevel->xdg_toplevel->app_id : "";
    } else {
        title = toplevel->xwayland_surface->title != nullptr ? toplevel->xwayland_surface->title : "";
        app_id = toplevel->xwayland_surface->class_ != nullptr ? toplevel->xwayland_surface->class_ : "";
    }
    wlr_foreign_toplevel_handle_v1_set_title(wrapper->handle, title);
    wlr_foreign_toplevel_handle_v1_set_app_id(wrapper->handle, app_id);
}

void foreign_toplevel_sync_state(BiomeToplevel *toplevel) {
    BiomeForeignToplevel *wrapper = toplevel->foreign_toplevel;
    if (wrapper == nullptr) {
        return;
    }
    wlr_foreign_toplevel_handle_v1_set_maximized(wrapper->handle, toplevel->maximized);
    wlr_foreign_toplevel_handle_v1_set_minimized(wrapper->handle, toplevel->minimized);
    wlr_foreign_toplevel_handle_v1_set_activated(wrapper->handle, toplevel->focused);
}
