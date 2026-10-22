// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/pointer_constraints.h"

#include "core/cursor.h"

#include <cmath>

namespace {

struct BiomePointerConstraint {
    BiomeServer *server = nullptr;
    wlr_pointer_constraint_v1 *wlr = nullptr;
    wl_listener set_region = {};
    wl_listener destroy = {};
};

struct SurfaceSearch {
    wlr_surface *surface = nullptr;
    bool found = false;
    int x = 0, y = 0;
};

void find_surface_buffer(wlr_scene_buffer *buffer, int sx, int sy, void *data) {
    auto *search = static_cast<SurfaceSearch *>(data);
    if (search->found) {
        return;
    }
    wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(buffer);
    if (scene_surface != nullptr && scene_surface->surface == search->surface) {
        search->found = true;
        search->x = sx;
        search->y = sy;
    }
}

bool surface_layout_origin(BiomeServer *server, wlr_surface *surface, double *x, double *y) {
    SurfaceSearch search;
    search.surface = surface;
    wlr_scene_node_for_each_buffer(&server->scene->tree.node, find_surface_buffer, &search);
    *x = search.x;
    *y = search.y;
    return search.found;
}

// Region stays empty until the surface's first commit after the request.
void warp_into_region(BiomeServer *server, wlr_pointer_constraint_v1 *constraint) {
    if (!pixman_region32_not_empty(&constraint->region)) {
        return;
    }
    double ox, oy;
    if (!surface_layout_origin(server, constraint->surface, &ox, &oy)) {
        return;
    }
    double sx = server->cursor->x - ox;
    double sy = server->cursor->y - oy;
    if (pixman_region32_contains_point(&constraint->region,
            static_cast<int>(std::floor(sx)), static_cast<int>(std::floor(sy)), nullptr)) {
        return;
    }
    int count;
    pixman_box32_t *box = pixman_region32_rectangles(&constraint->region, &count);
    wlr_cursor_warp_closest(server->cursor, nullptr,
        ox + (box->x1 + box->x2) / 2.0, oy + (box->y1 + box->y2) / 2.0);
    cursor_rebase(server);
}

// The hint is where the client drew its own cursor while locked.
void warp_to_cursor_hint(BiomeServer *server, wlr_pointer_constraint_v1 *constraint) {
    if (constraint->type != WLR_POINTER_CONSTRAINT_V1_LOCKED || !constraint->current.cursor_hint.enabled ||
            server->seat->pointer_state.focused_surface != constraint->surface) {
        return;
    }
    double sx = constraint->current.cursor_hint.x;
    double sy = constraint->current.cursor_hint.y;
    wlr_cursor_warp(server->cursor, nullptr, server->pointer_focus_origin_x + sx, server->pointer_focus_origin_y + sy);
    wlr_seat_pointer_warp(server->seat, sx, sy);
}

void deactivate(BiomeServer *server) {
    wlr_pointer_constraint_v1 *constraint = server->active_constraint;
    if (constraint == nullptr) {
        return;
    }
    // Cleared first: send_deactivated destroys a oneshot constraint.
    server->active_constraint = nullptr;
    warp_to_cursor_hint(server, constraint);
    wlr_pointer_constraint_v1_send_deactivated(constraint);
}

void activate(BiomeServer *server, wlr_pointer_constraint_v1 *constraint) {
    if (server->active_constraint == constraint) {
        return;
    }
    deactivate(server);
    if (constraint == nullptr) {
        return;
    }
    server->active_constraint = constraint;
    wlr_pointer_constraint_v1_send_activated(constraint);
    warp_into_region(server, constraint);
}

// Root match: a client may constrain a subsurface (e.g. Firefox's content),
// while keyboard focus is on the toplevel's main surface.
wlr_pointer_constraint_v1 *constraint_for_focus(BiomeServer *server, wlr_surface *focus) {
    if (focus == nullptr) {
        return nullptr;
    }
    wlr_surface *root = wlr_surface_get_root_surface(focus);
    wlr_pointer_constraint_v1 *constraint;
    wl_list_for_each(constraint, &server->pointer_constraints->constraints, link) {
        if (constraint->seat == server->seat && wlr_surface_get_root_surface(constraint->surface) == root) {
            return constraint;
        }
    }
    return nullptr;
}

void handle_constraint_set_region(wl_listener *listener, void *data) {
    (void)data;
    BiomePointerConstraint *wrapper = wl_container_of(listener, wrapper, set_region);
    if (wrapper->server->active_constraint == wrapper->wlr) {
        warp_into_region(wrapper->server, wrapper->wlr);
    }
}

void handle_constraint_destroy(wl_listener *listener, void *data) {
    (void)data;
    BiomePointerConstraint *wrapper = wl_container_of(listener, wrapper, destroy);
    BiomeServer *server = wrapper->server;
    if (server->active_constraint == wrapper->wlr) {
        server->active_constraint = nullptr;
        warp_to_cursor_hint(server, wrapper->wlr);
    }
    wl_list_remove(&wrapper->set_region.link);
    wl_list_remove(&wrapper->destroy.link);
    delete wrapper;
}

void handle_new_constraint(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, new_pointer_constraint);
    auto *wrapper = new BiomePointerConstraint;
    wrapper->server = server;
    wrapper->wlr = static_cast<wlr_pointer_constraint_v1 *>(data);
    wrapper->set_region.notify = handle_constraint_set_region;
    wl_signal_add(&wrapper->wlr->events.set_region, &wrapper->set_region);
    wrapper->destroy.notify = handle_constraint_destroy;
    wl_signal_add(&wrapper->wlr->events.destroy, &wrapper->destroy);

    if (server->active_constraint == nullptr &&
            constraint_for_focus(server, server->seat->keyboard_state.focused_surface) == wrapper->wlr) {
        activate(server, wrapper->wlr);
    }
}

void handle_keyboard_focus_change(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, keyboard_focus_change);
    auto *event = static_cast<wlr_seat_keyboard_focus_change_event *>(data);
    activate(server, constraint_for_focus(server, event->new_surface));
}

} // namespace

void pointer_constraints_init(BiomeServer *server) {
    server->relative_pointer_manager = wlr_relative_pointer_manager_v1_create(server->display);
    server->pointer_constraints = wlr_pointer_constraints_v1_create(server->display);
    server->new_pointer_constraint.notify = handle_new_constraint;
    wl_signal_add(&server->pointer_constraints->events.new_constraint, &server->new_pointer_constraint);
    server->keyboard_focus_change.notify = handle_keyboard_focus_change;
    wl_signal_add(&server->seat->keyboard_state.events.focus_change, &server->keyboard_focus_change);
}

void pointer_constraints_clamp_motion(BiomeServer *server, double *dx, double *dy) {
    wlr_pointer_constraint_v1 *constraint = server->active_constraint;
    // Only while the pointer is actually over it - an activation warp can
    // fail (e.g. region still empty), and Biome's own grabs take precedence.
    if (constraint == nullptr || server->cursor_mode != BiomeCursorMode::Passthrough ||
            server->seat->pointer_state.focused_surface != constraint->surface) {
        return;
    }
    if (constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
        *dx = 0;
        *dy = 0;
        return;
    }
    double sx = server->cursor->x - server->pointer_focus_origin_x;
    double sy = server->cursor->y - server->pointer_focus_origin_y;
    double cx, cy;
    if (wlr_region_confine(&constraint->region, sx, sy, sx + *dx, sy + *dy, &cx, &cy)) {
        *dx = cx - sx;
        *dy = cy - sy;
    } else if (pixman_region32_not_empty(&constraint->region)) {
        // Outside the region (it changed, or wasn't committed at activation).
        *dx = 0;
        *dy = 0;
        warp_into_region(server, constraint);
    }
}
