// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/session_lock.h"

#include "core/output.h"
#include "desktop/workspace.h"

// Per-BiomeOutput wlr_session_lock_surface_v1 wrapper - not exposed outside
// this file. BiomeOutput::lock_surface holds the raw wlroots pointer for
// output_request_state()'s resize-reconfigure; this struct is only for the
// scene content and listener bookkeeping that goes with it.
struct BiomeLockSurface {
    BiomeServer *server = nullptr;
    wlr_session_lock_surface_v1 *lock_surface = nullptr;
    // Not explicitly destroyed here - wlr_scene_subsurface_tree_create's
    // tree self-destroys on the underlying wlr_surface's own destroy event
    // (confirmed in wlroots' subsurface_tree.c), and it's already unmapped
    // (invisible) well before that by the time our own destroy handler runs.
    wlr_scene_tree *scene_tree = nullptr;
    wl_listener destroy = {};
    wl_listener surface_map = {};
};

static void handle_lock_surface_map(wl_listener *listener, void *data) {
    (void)data;
    BiomeLockSurface *wrapper = wl_container_of(listener, wrapper, surface_map);
    wlr_seat *seat = wrapper->server->seat;
    if (seat->keyboard_state.focused_surface != nullptr) {
        // Some other lock surface already claimed keyboard focus - per spec
        // this is compositor policy; matching the suggested convention of
        // "the first lock surface created gets focus" but keyed on actual
        // map order rather than creation-request order.
        return;
    }
    wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    if (keyboard != nullptr) {
        wlr_seat_keyboard_notify_enter(seat, wrapper->lock_surface->surface,
            keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    }
}

static void handle_lock_surface_destroy(wl_listener *listener, void *data) {
    (void)data;
    BiomeLockSurface *wrapper = wl_container_of(listener, wrapper, destroy);
    wl_list_remove(&wrapper->destroy.link);
    wl_list_remove(&wrapper->surface_map.link);

    BiomeOutput *output;
    wl_list_for_each(output, &wrapper->server->outputs, link) {
        if (output->lock_surface == wrapper->lock_surface) {
            output->lock_surface = nullptr;
            break;
        }
    }
    free(wrapper);
}

static void handle_lock_new_surface(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, lock_new_surface);
    auto *lock_surface = static_cast<wlr_session_lock_surface_v1 *>(data);

    BiomeOutput *output = biome_output_from_wlr(server, lock_surface->output);
    if (output == nullptr) {
        // wlroots already validated the wl_output resource before emitting
        // this signal - shouldn't happen, but nothing sane to do if our own
        // BiomeOutput bookkeeping doesn't have a match.
        return;
    }

    auto *wrapper = static_cast<BiomeLockSurface *>(calloc(1, sizeof(BiomeLockSurface)));
    wrapper->server = server;
    wrapper->lock_surface = lock_surface;
    wrapper->scene_tree = wlr_scene_subsurface_tree_create(output->lock_tree, lock_surface->surface);
    lock_surface->data = wrapper;
    output->lock_surface = lock_surface;

    wrapper->destroy.notify = handle_lock_surface_destroy;
    wl_signal_add(&lock_surface->events.destroy, &wrapper->destroy);
    wrapper->surface_map.notify = handle_lock_surface_map;
    wl_signal_add(&lock_surface->surface->events.map, &wrapper->surface_map);

    int width, height;
    wlr_output_effective_resolution(output->wlr, &width, &height);
    wlr_session_lock_surface_v1_configure(
        lock_surface, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
}

static void handle_lock_unlock(wl_listener *listener, void *data) {
    (void)data;
    BiomeServer *server = wl_container_of(listener, server, lock_unlock);
    server->session_locked = false;
    wlr_scene_node_set_enabled(&server->layers.session_lock->node, false);

    // No toplevel visibility sweep needed here (Phase 3.5 had one, to undo
    // update_toplevel_visibility()'s now-removed session_locked clause -
    // see workspace.cpp): a toplevel's enabled bit was never touched by
    // locking in the first place under the current structural layer stack,
    // so there's nothing to restore.

    // Restores focus to the MRU-front toplevel that's actually visible on
    // the active workspace (not just MRU-front overall - the same
    // distinction switch_workspace() already has to make), or clears focus
    // if there isn't one.
    focus_topmost_on_active_workspace(server);
}

// Fires for both a clean unlock_and_destroy (after lock_unlock above) and an
// abnormal client death (lock_unlock never firing at all) - deliberately
// does not touch server->session_locked, see server.h's field comment.
static void handle_lock_destroy(wl_listener *listener, void *data) {
    (void)data;
    BiomeServer *server = wl_container_of(listener, server, lock_destroy);
    wl_list_remove(&server->lock_new_surface.link);
    wl_list_remove(&server->lock_unlock.link);
    wl_list_remove(&server->lock_destroy.link);
    server->active_lock = nullptr;

    if (server->session_locked) {
        // Abandoned lock (client died without unlock_and_destroy, so
        // session_locked deliberately stayed true - see server.h's field
        // comment) - tint every output's blank rect red so a permanently-
        // stuck-locked screen reads as visually distinct from a normal
        // in-progress lock, matching sway's own convention for this case.
        BiomeOutput *output;
        wl_list_for_each(output, &server->outputs, link) {
            if (output->lock_rect != nullptr) {
                wlr_scene_rect_set_color(output->lock_rect, kSessionLockAbandonedColor);
            }
        }
    }
}

static void handle_new_session_lock(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, new_session_lock);
    auto *lock = static_cast<wlr_session_lock_v1 *>(data);

    if (server->active_lock != nullptr) {
        // Already locked, or another lock is mid-negotiation - refuse this
        // one immediately per spec ("finished event should be sent
        // immediately... if the compositor decides the locked event will
        // not be sent"). Note this checks active_lock, not session_locked:
        // after a crashed lock client's destroy event nulls active_lock,
        // session_locked can still be true while a *replacement* client is
        // allowed to lock() and take over - see server.h's field comment.
        wlr_session_lock_v1_destroy(lock);
        return;
    }

    server->active_lock = lock;
    server->lock_new_surface.notify = handle_lock_new_surface;
    wl_signal_add(&lock->events.new_surface, &server->lock_new_surface);
    server->lock_unlock.notify = handle_lock_unlock;
    wl_signal_add(&lock->events.unlock, &server->lock_unlock);
    server->lock_destroy.notify = handle_lock_destroy;
    wl_signal_add(&lock->events.destroy, &server->lock_destroy);

    server->session_locked = true;
    wlr_seat_keyboard_notify_clear_focus(server->seat);
    wlr_seat_pointer_notify_clear_focus(server->seat);

    // No raise_to_top needed: server->layers.session_lock is structurally
    // the last of BiomeServer::layers' six fixed trees (core/layers.cpp),
    // so it's already the topmost sibling of scene->tree the moment it's
    // enabled - see server.h's doc comment on BiomeServer::layers.
    wlr_scene_node_set_enabled(&server->layers.session_lock->node, true);

    // Every lock starts from a clean, normally-colored rect - undoes the red
    // abandoned-lock tint (see handle_lock_destroy) if this is a replacement
    // client recovering from a crash; a harmless no-op otherwise.
    BiomeOutput *color_output;
    wl_list_for_each(color_output, &server->outputs, link) {
        if (color_output->lock_rect != nullptr) {
            wlr_scene_rect_set_color(color_output->lock_rect, kSessionLockColor);
        }
    }

    // No toplevel visibility sweep needed here either, for the same
    // structural reason handle_lock_unlock's comment above gives: every
    // toplevel's scene_tree is a child of the fixed server->layers.toplevels
    // tree (server.h's BiomeServer::layers doc comment), which is
    // structurally below server->layers.session_lock for the lifetime of
    // the compositor - including for a toplevel mapped for the first time
    // *while* locked, since place_new_toplevel() parents it there too, not
    // as a fresh sibling of scene->tree. That's what closes the gap Phase
    // 3.5's original ad-hoc single-raised-tree design had (a brand new
    // scene node always becomes the newest topmost sibling of *its own*
    // parent, which used to be scene->tree itself for every toplevel).

    BiomeOutput *output;
    wl_list_for_each(output, &server->outputs, link) {
        if (output->wlr->enabled) {
            output->pending_lock_frame = true;
        }
    }
}

void session_lock_init(BiomeServer *server) {
    // server->layers.session_lock already exists (core/layers.cpp, called
    // from output_manager_init before this runs) and starts disabled -
    // nothing to create here.
    server->session_lock_manager = wlr_session_lock_manager_v1_create(server->display);
    server->new_session_lock.notify = handle_new_session_lock;
    wl_signal_add(&server->session_lock_manager->events.new_lock, &server->new_session_lock);
}
