// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/session_lock.h"

#include "desktop/window_menu.h"

#include "core/cursor.h"
#include "core/output.h"
#include "desktop/foreign_toplevel.h"
#include "desktop/switcher.h"
#include "desktop/workspace.h"

// Scene content and listeners for one output's lock surface.
struct BiomeLockSurface {
    BiomeServer *server = nullptr;
    wlr_session_lock_surface_v1 *lock_surface = nullptr;
    // Destroys itself with the wlr_surface.
    wlr_scene_tree *scene_tree = nullptr;
    wl_listener destroy = {};
    wl_listener surface_map = {};
};

static bool is_mapped_lock_surface(wlr_surface *surface) {
    return surface != nullptr && surface->mapped && wlr_session_lock_surface_v1_try_from_wlr_surface(surface) != nullptr;
}

// Raw enter, not notify_enter: a client's xdg_popup grab (wlroots doesn't check its
// serial) has a no-op enter and would keep focus.
static void enter_lock_surface(BiomeServer *server, wlr_surface *surface) {
    wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
    wlr_seat_keyboard_enter(server->seat, surface, keyboard ? keyboard->keycodes : nullptr,
        keyboard ? keyboard->num_keycodes : 0, keyboard ? &keyboard->modifiers : nullptr);
}

void session_lock_refocus(BiomeServer *server) {
    if (!server->session_locked || is_mapped_lock_surface(server->seat->keyboard_state.focused_surface)) {
        return;
    }
    BiomeOutput *output;
    wl_list_for_each(output, &server->outputs, link) {
        if (output->lock_surface != nullptr && output->lock_surface->surface->mapped) {
            enter_lock_surface(server, output->lock_surface->surface);
            return;
        }
    }
    wlr_seat_keyboard_clear_focus(server->seat);
}

static void handle_lock_surface_map(wl_listener *listener, void *data) {
    (void)data;
    BiomeLockSurface *wrapper = wl_container_of(listener, wrapper, surface_map);
    // First mapped lock surface keeps focus (the spec leaves this to the compositor).
    if (!is_mapped_lock_surface(wrapper->server->seat->keyboard_state.focused_surface)) {
        enter_lock_surface(wrapper->server, wrapper->lock_surface->surface);
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
    BiomeServer *server = wrapper->server;
    free(wrapper);
    // Its output went away mid-lock: hand focus to another lock surface.
    session_lock_refocus(server);
}

static void handle_lock_new_surface(wl_listener *listener, void *data) {
    BiomeServer *server = biome_server_from(listener, BiomeSessionLock, new_surface);
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
    BiomeServer *server = biome_server_from(listener, BiomeSessionLock, unlock);
    server->session_locked = false;
    wlr_scene_node_set_enabled(&server->layers.session_lock->node, false);

    // No toplevel visibility sweep needed here: a toplevel's enabled bit is
    // never touched by locking under the current structural layer stack
    // (see workspace.cpp), so there's nothing to restore.

    // Restores focus to the MRU-front toplevel that's actually visible on
    // the active workspace (not just MRU-front overall - the same
    // distinction switch_workspace() already has to make), or clears focus
    // if there isn't one.
    focus_topmost_on_active_workspace(server);
    cursor_rebase(server); // pointer focus was cleared at lock
    foreign_toplevel_set_capture_blank(server, false);
}

// Fires for both a clean unlock_and_destroy (after lock_unlock above) and an
// abnormal client death (lock_unlock never firing at all) - deliberately
// does not touch server->session_locked, see server.h's field comment.
static void handle_lock_destroy(wl_listener *listener, void *data) {
    (void)data;
    BiomeServer *server = biome_server_from(listener, BiomeSessionLock, destroy);
    wl_list_remove(&server->lock.new_surface.link);
    wl_list_remove(&server->lock.unlock.link);
    wl_list_remove(&server->lock.destroy.link);
    server->lock.active = nullptr;

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
    BiomeServer *server = biome_server_from(listener, BiomeSessionLock, new_lock);
    auto *lock = static_cast<wlr_session_lock_v1 *>(data);

    if (server->lock.active != nullptr) {
        // Another lock is live: refuse (sends finished). Not session_locked,
        // so a replacement can take over after a lock client crashes.
        wlr_session_lock_v1_destroy(lock);
        return;
    }

    server->lock.active = lock;
    server->lock.locked_sent = false;
    server->lock.new_surface.notify = handle_lock_new_surface;
    wl_signal_add(&lock->events.new_surface, &server->lock.new_surface);
    server->lock.unlock.notify = handle_lock_unlock;
    wl_signal_add(&lock->events.unlock, &server->lock.unlock);
    server->lock.destroy.notify = handle_lock_destroy;
    wl_signal_add(&lock->events.destroy, &server->lock.destroy);

    server->session_locked = true;
    foreign_toplevel_set_capture_blank(server, true);
    window_menu_close(server);
    switcher_cancel(server);
    // An xdg_popup grab's keyboard enter/clear_focus are no-ops, so it would keep
    // focus (and get the password) while locked. Ending it sends popup_done.
    wlr_seat_pointer_end_grab(server->seat);
    wlr_seat_keyboard_end_grab(server->seat);
    wlr_seat_touch_end_grab(server->seat);
    clear_keyboard_focus(server);
    wlr_seat_pointer_notify_clear_focus(server->seat);

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
    // toplevel's scene_tree is a child of one of the fixed server->layers
    // trees (layers.toplevels normally, layers.fullscreen while fullscreen -
    // see set_toplevel_fullscreen; server.h's BiomeServer::layers doc
    // comment), both structurally below server->layers.session_lock for the
    // lifetime of the compositor - including for a toplevel mapped for the
    // first time *while* locked, since place_new_toplevel() parents it there
    // too, not as a fresh sibling of scene->tree.

    BiomeOutput *output;
    wl_list_for_each(output, &server->outputs, link) {
        if (output->wlr->enabled) {
            output->pending_lock_frame = true;
        }
    }
    // No enabled output (all powered off or unplugged): nothing to wait for.
    session_lock_maybe_send_locked(server);
}

void session_lock_maybe_send_locked(BiomeServer *server) {
    if (!server->session_locked || server->lock.active == nullptr || server->lock.locked_sent) {
        return;
    }
    BiomeOutput *output;
    wl_list_for_each(output, &server->outputs, link) {
        if (output->pending_lock_frame) {
            return;
        }
    }
    server->lock.locked_sent = true;
    wlr_session_lock_v1_send_locked(server->lock.active);
}

void session_lock_init(BiomeServer *server) {
    server->lock.server = server;
    // server->layers.session_lock already exists (core/layers.cpp, called
    // from output_manager_init before this runs) and starts disabled -
    // nothing to create here.
    server->lock.manager = wlr_session_lock_manager_v1_create(server->display);
    server->lock.new_lock.notify = handle_new_session_lock;
    wl_signal_add(&server->lock.manager->events.new_lock, &server->lock.new_lock);
}

void session_lock_finish(BiomeServer *server) {
    wl_list_remove(&server->lock.new_lock.link);
}
