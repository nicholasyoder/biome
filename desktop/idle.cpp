// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/idle.h"

namespace {

struct BiomeIdleInhibitor {
    BiomeServer *server = nullptr;
    wlr_idle_inhibitor_v1 *wlr = nullptr;
    wl_listener destroy = {};
};

struct VisibleSearch {
    wlr_surface *surface = nullptr;
    bool found = false;
};

void find_visible_buffer(wlr_scene_buffer *buffer, int sx, int sy, void *data) {
    (void)sx;
    (void)sy;
    auto *search = static_cast<VisibleSearch *>(data);
    if (search->found || buffer->active_outputs == 0) {
        return;
    }
    wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(buffer);
    if (scene_surface != nullptr && scene_surface->surface == search->surface) {
        search->found = true;
    }
}

// The walk skips disabled subtrees, which matters: wlroots 0.18 leaves a
// buffer's active_outputs stale when an ancestor is disabled (minimize,
// workspace switch). active_outputs itself covers occlusion and offscreen.
bool surface_visible(BiomeServer *server, wlr_surface *surface) {
    VisibleSearch search;
    search.surface = surface;
    wlr_scene_node_for_each_buffer(&server->scene->tree.node, find_visible_buffer, &search);
    return search.found;
}

// `ignore`: wlroots emits an inhibitor's destroy while it's still listed.
void update_inhibited(BiomeServer *server, wlr_idle_inhibitor_v1 *ignore) {
    bool inhibited = false;
    wlr_idle_inhibitor_v1 *inhibitor;
    wl_list_for_each(inhibitor, &server->idle_inhibit_manager->inhibitors, link) {
        if (inhibitor != ignore && surface_visible(server, inhibitor->surface)) {
            inhibited = true;
            break;
        }
    }
    if (inhibited != server->idle_inhibited) {
        wlr_log(WLR_DEBUG, "idle: %s", inhibited ? "inhibited" : "uninhibited");
    }
    server->idle_inhibited = inhibited;
    wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, inhibited);
}

void handle_inhibitor_destroy(wl_listener *listener, void *data) {
    (void)data;
    BiomeIdleInhibitor *wrapper = wl_container_of(listener, wrapper, destroy);
    wl_list_remove(&wrapper->destroy.link);
    update_inhibited(wrapper->server, wrapper->wlr);
    delete wrapper;
}

void handle_new_inhibitor(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, new_idle_inhibitor);
    auto *wrapper = new BiomeIdleInhibitor;
    wrapper->server = server;
    wrapper->wlr = static_cast<wlr_idle_inhibitor_v1 *>(data);
    wrapper->destroy.notify = handle_inhibitor_destroy;
    wl_signal_add(&wrapper->wlr->events.destroy, &wrapper->destroy);
    update_inhibited(server, nullptr);
}

} // namespace

void idle_init(BiomeServer *server) {
    server->idle_notifier = wlr_idle_notifier_v1_create(server->display);
    server->idle_inhibit_manager = wlr_idle_inhibit_v1_create(server->display);
    server->new_idle_inhibitor.notify = handle_new_inhibitor;
    wl_signal_add(&server->idle_inhibit_manager->events.new_inhibitor, &server->new_idle_inhibitor);
}

void idle_notify_activity(BiomeServer *server) {
    wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
}

void idle_update_inhibited(BiomeServer *server) {
    if (wl_list_empty(&server->idle_inhibit_manager->inhibitors) && !server->idle_inhibited) {
        return;
    }
    update_inhibited(server, nullptr);
}
