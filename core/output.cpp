// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/output.h"

#include <ctime>

static void server_new_output(wl_listener *listener, void *data);

void output_manager_init(BiomeServer *server) {
    server->output_layout = wlr_output_layout_create(server->display);

    wl_list_init(&server->outputs);
    server->new_output.notify = server_new_output;
    wl_signal_add(&server->backend->events.new_output, &server->new_output);

    // Handles all rendering and damage tracking; things get added to it at
    // the proper positions and wlr_scene_output_commit() renders a frame.
    server->scene = wlr_scene_create();
    server->scene_layout = wlr_scene_attach_output_layout(server->scene, server->output_layout);
}

// Called at the output's refresh rate (e.g. 60Hz).
static void output_frame(wl_listener *listener, void *data) {
    (void)data;
    BiomeOutput *output = wl_container_of(listener, output, frame);
    wlr_scene *scene = output->server->scene;

    wlr_scene_output *scene_output = wlr_scene_get_scene_output(scene, output->wlr);
    wlr_scene_output_commit(scene_output, nullptr);

    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

// e.g. Wayland/X11 backends request a new mode when the output window resizes.
static void output_request_state(wl_listener *listener, void *data) {
    BiomeOutput *output = wl_container_of(listener, output, request_state);
    auto *event = static_cast<const wlr_output_event_request_state *>(data);
    wlr_output_commit_state(output->wlr, event->state);
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

static void server_new_output(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, new_output);
    auto *wlr_output = static_cast<struct wlr_output *>(data);

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    // Some backends (e.g. DRM+KMS) require a mode to be set before use; just
    // pick the monitor's preferred one.
    wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode != nullptr) {
        wlr_output_state_set_mode(&state, mode);
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
    // adds a wl_output global for clients to query (DPI, scale, etc).
    wlr_output_layout_output *l_output =
        wlr_output_layout_add_auto(server->output_layout, wlr_output);
    wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);
}
