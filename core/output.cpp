// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/output.h"

#include "core/layers.h"
#include "desktop/layer_shell.h"
#include "desktop/session_lock.h"

#include <ctime>

static void server_new_output(wl_listener *listener, void *data);

void output_manager_init(BiomeServer *server) {
    server->output_layout = wlr_output_layout_create(server->display);

    wl_list_init(&server->outputs);
    server->output_configs = load_output_configs();
    server->new_output.notify = server_new_output;
    wl_signal_add(&server->backend->events.new_output, &server->new_output);

    // Handles all rendering and damage tracking; things get added to it at
    // the proper positions and wlr_scene_output_commit() renders a frame.
    server->scene = wlr_scene_create();
    server->scene_layout = wlr_scene_attach_output_layout(server->scene, server->output_layout);
    scene_layers_init(server);

    // Self-contained: listens to output_layout's own add/change/destroy
    // signals itself, so nothing else needs to touch the returned pointer
    // after creation (same one-call shape as
    // wlr_primary_selection_v1_device_manager_create in core/main.cpp).
    wlr_xdg_output_manager_v1_create(server->display, server->output_layout);
}

// Called at the output's refresh rate (e.g. 60Hz).
static void output_frame(wl_listener *listener, void *data) {
    (void)data;
    BiomeOutput *output = wl_container_of(listener, output, frame);
    BiomeServer *server = output->server;
    wlr_scene *scene = server->scene;

    wlr_scene_output *scene_output = wlr_scene_get_scene_output(scene, output->wlr);
    wlr_scene_output_commit(scene_output, nullptr);

    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);

    // ext-session-lock-v1: the `locked` event must not be sent until a
    // locked frame has actually been presented on every output (not just
    // scheduled) - see desktop/session_lock.cpp. This waits for every
    // currently-enabled output to commit at least one frame after the lock
    // began before declaring it satisfied.
    if (server->session_locked && output->pending_lock_frame) {
        output->pending_lock_frame = false;
        if (server->active_lock != nullptr && !server->active_lock->locked_sent) {
            bool any_pending = false;
            BiomeOutput *other;
            wl_list_for_each(other, &server->outputs, link) {
                if (other->pending_lock_frame) {
                    any_pending = true;
                    break;
                }
            }
            if (!any_pending) {
                wlr_session_lock_v1_send_locked(server->active_lock);
            }
        }
    }
}

// e.g. Wayland/X11 backends request a new mode when the output window resizes.
static void output_request_state(wl_listener *listener, void *data) {
    BiomeOutput *output = wl_container_of(listener, output, request_state);
    auto *event = static_cast<const wlr_output_event_request_state *>(data);
    wlr_output_commit_state(output->wlr, event->state);

    // Keep the session-lock blank rect (and any live lock surface's
    // configured size) in sync with a live resolution change - otherwise a
    // shrunk rect would leave real desktop content visible around its edges
    // while locked. Only realistically reachable on the nested dev backends
    // today (a real DRM/KMS mode doesn't change without a fresh output_state
    // commit from Biome itself), but this is a security-relevant gap if
    // skipped, not just polish.
    if (output->lock_rect != nullptr) {
        int width, height;
        wlr_output_effective_resolution(output->wlr, &width, &height);
        if (output->lock_rect->width != width || output->lock_rect->height != height) {
            wlr_scene_rect_set_size(output->lock_rect, width, height);
            if (output->lock_surface != nullptr) {
                wlr_session_lock_surface_v1_configure(
                    output->lock_surface, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
            }
        }
    }

    // Same reasoning as the lock-rect resize above: a layer surface's
    // exclusive-zone reservation and full-width/height stretch both depend
    // on the output's box, so a live resolution change needs its own
    // re-arrange - otherwise a shrunk output would leave a bar sized for
    // the old, larger box.
    arrange_layers(output);
}

static void output_destroy(wl_listener *listener, void *data) {
    (void)data;
    BiomeOutput *output = wl_container_of(listener, output, destroy);

    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    free(output);
}

// Searches wlr_output's advertised mode list for the best match to a
// configured width/height/refresh (refresh_mhz == 0 means "any refresh").
// Prefers an exact refresh match, then the driver's own preferred mode
// among same-size matches, then any same-size match. Returns nullptr if no
// mode of that size is advertised at all (e.g. the nested Wayland/X11 dev
// backends, which have no fixed mode list).
static wlr_output_mode *find_matching_mode(wlr_output *wlr_output, const OutputConfig::Mode &wanted) {
    wlr_output_mode *any_size_match = nullptr;
    wlr_output_mode *preferred_size_match = nullptr;
    wlr_output_mode *mode_iter;
    wl_list_for_each(mode_iter, &wlr_output->modes, link) {
        if (mode_iter->width != wanted.width || mode_iter->height != wanted.height) {
            continue;
        }
        if (wanted.refresh_mhz != 0 && mode_iter->refresh == wanted.refresh_mhz) {
            return mode_iter;
        }
        if (any_size_match == nullptr) {
            any_size_match = mode_iter;
        }
        if (mode_iter->preferred) {
            preferred_size_match = mode_iter;
        }
    }
    return preferred_size_match != nullptr ? preferred_size_match : any_size_match;
}

static void server_new_output(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, new_output);
    auto *wlr_output = static_cast<struct wlr_output *>(data);

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    OutputConfig cfg; // documented defaults if this connector has no config entry
    if (wlr_output->name != nullptr) {
        auto it = server->output_configs.find(wlr_output->name);
        if (it != server->output_configs.end()) {
            cfg = it->second;
        }
    }

    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, cfg.enabled);

    if (cfg.enabled) {
        if (cfg.mode.has_value()) {
            wlr_output_mode *chosen_mode = find_matching_mode(wlr_output, *cfg.mode);
            if (chosen_mode != nullptr) {
                wlr_output_state_set_mode(&state, chosen_mode);
            } else {
                // No advertised mode matches - either this backend has no
                // fixed mode list at all (the nested Wayland/X11 dev
                // backends) or this exact size isn't offered. Custom modes
                // "may result in visual artifacts" on real DRM/KMS per
                // wlr_output_state_set_custom_mode()'s own doc comment, but
                // are the only way to honor an exact user-requested
                // resolution the driver doesn't enumerate.
                wlr_log(WLR_ERROR,
                        "output %s: no matching mode for configured %dx%d@%d, using custom mode",
                        wlr_output->name, cfg.mode->width, cfg.mode->height, cfg.mode->refresh_mhz);
                wlr_output_state_set_custom_mode(&state, cfg.mode->width, cfg.mode->height,
                                                  cfg.mode->refresh_mhz);
            }
        } else {
            // Some backends (e.g. DRM+KMS) require a mode to be set before
            // use; just pick the monitor's preferred one.
            wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
            if (mode != nullptr) {
                wlr_output_state_set_mode(&state, mode);
            }
        }

        wlr_output_state_set_scale(&state, static_cast<float>(cfg.scale));
        wlr_output_state_set_transform(&state, cfg.transform);
    }

    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    auto *output = static_cast<BiomeOutput *>(calloc(1, sizeof(BiomeOutput)));
    output->wlr = wlr_output;
    output->server = server;

    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    output->request_state.notify = output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);
    output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    wl_list_insert(&server->outputs, &output->link);

    // add_auto arranges outputs left-to-right in the order they appear, and
    // adds a wl_output global for clients to query (DPI, scale, etc). A
    // configured position instead anchors the output there directly -
    // wlr_output_layout handles a mix of anchored and auto-arranged outputs
    // on its own (auto ones flow to the right of the rightmost anchored
    // one). Disabled outputs still get full layout/scene wiring - wlroots
    // withholds the wl_output global for a 0x0 output on its own, and
    // wlr_scene already treats a disabled output as invisible.
    wlr_output_layout_output *l_output =
        cfg.position.has_value()
            ? wlr_output_layout_add(server->output_layout, wlr_output, cfg.position->first,
                                     cfg.position->second)
            : wlr_output_layout_add_auto(server->output_layout, wlr_output);
    wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);

    // wlr-layer-shell-unstable-v1: one child tree per output-scoped global
    // layer, positioned at this output's layout coords - layer-shell
    // surfaces anchor to a specific output rather than placing themselves
    // in global coordinates the way toplevels do. See desktop/layer_shell.cpp.
    output->layer_background = wlr_scene_tree_create(server->layers.background);
    output->layer_bottom = wlr_scene_tree_create(server->layers.bottom);
    output->layer_top = wlr_scene_tree_create(server->layers.top);
    output->layer_overlay = wlr_scene_tree_create(server->layers.overlay);
    wlr_scene_node_set_position(&output->layer_background->node, l_output->x, l_output->y);
    wlr_scene_node_set_position(&output->layer_bottom->node, l_output->x, l_output->y);
    wlr_scene_node_set_position(&output->layer_top->node, l_output->x, l_output->y);
    wlr_scene_node_set_position(&output->layer_overlay->node, l_output->x, l_output->y);

    // ext-session-lock-v1: created unconditionally for every output, locked
    // or not, so a monitor that appears while already locked is blanked
    // from its very first frame with no special hotplug-during-lock code -
    // see desktop/session_lock.cpp. lock_rect is the opaque fallback layer;
    // a client's own lock surface (added later, as a sibling within
    // output->lock_tree) renders on top of it since it's created after.
    output->lock_tree = wlr_scene_tree_create(server->layers.session_lock);
    wlr_scene_node_set_position(&output->lock_tree->node, l_output->x, l_output->y);
    int lock_width, lock_height;
    wlr_output_effective_resolution(wlr_output, &lock_width, &lock_height);
    output->lock_rect =
        wlr_scene_rect_create(output->lock_tree, lock_width, lock_height, kSessionLockColor);

    // No layer surfaces can target this output yet (it was just created),
    // but this keeps output->usable_area initialized to the full output box
    // rather than a zeroed wlr_box from the moment the output exists.
    arrange_layers(output);
}
