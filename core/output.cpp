// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/output.h"

#include <ctime>

static void server_new_output(wl_listener *listener, void *data);

void output_manager_init(BiomeServer *server) {
    // Creates an output layout, which a wlroots utility for working with an
    // arrangement of screens in a physical layout.
    server->output_layout = wlr_output_layout_create(server->display);

    // Configure a listener to be notified when new outputs are available on
    // the backend.
    wl_list_init(&server->outputs);
    server->new_output.notify = server_new_output;
    wl_signal_add(&server->backend->events.new_output, &server->new_output);

    // Create a scene graph. This is a wlroots abstraction that handles all
    // rendering and damage tracking. All the compositor author needs to do
    // is add things that should be rendered to the scene graph at the
    // proper positions and then call wlr_scene_output_commit() to render a
    // frame if necessary.
    server->scene = wlr_scene_create();
    server->scene_layout = wlr_scene_attach_output_layout(server->scene, server->output_layout);
}

static void output_frame(wl_listener *listener, void *data) {
    (void)data;
    // This function is called every time an output is ready to display a
    // frame, generally at the output's refresh rate (e.g. 60Hz).
    BiomeOutput *output = wl_container_of(listener, output, frame);
    wlr_scene *scene = output->server->scene;

    wlr_scene_output *scene_output = wlr_scene_get_scene_output(scene, output->wlr);

    // Render the scene if needed and commit the output
    wlr_scene_output_commit(scene_output, nullptr);

    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(wl_listener *listener, void *data) {
    // This function is called when the backend requests a new state for the
    // output. For example, Wayland and X11 backends request a new mode when
    // the output window is resized.
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
    // This event is raised by the backend when a new output (aka a display
    // or monitor) becomes available.
    BiomeServer *server = wl_container_of(listener, server, new_output);
    auto *wlr_output = static_cast<struct wlr_output *>(data);

    // Configures the output created by the backend to use our allocator and
    // our renderer. Must be done once, before committing the output
    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    // The output may be disabled, switch it on.
    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    // Some backends don't have modes. DRM+KMS does, and we need to set a
    // mode before we can use the output. The mode is a tuple of (width,
    // height, refresh rate), and each monitor supports only a specific set
    // of modes. We just pick the monitor's preferred mode, a more
    // sophisticated compositor would let the user configure it.
    wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode != nullptr) {
        wlr_output_state_set_mode(&state, mode);
    }

    // Atomically applies the new output state.
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    // Allocates and configures our state for this output
    auto *output = static_cast<BiomeOutput *>(calloc(1, sizeof(BiomeOutput)));
    output->wlr = wlr_output;
    output->server = server;

    // Sets up a listener for the frame event.
    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);

    // Sets up a listener for the state request event.
    output->request_state.notify = output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);

    // Sets up a listener for the destroy event.
    output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    wl_list_insert(&server->outputs, &output->link);

    // Adds this to the output layout. The add_auto function arranges
    // outputs from left-to-right in the order they appear. A more
    // sophisticated compositor would let the user configure the arrangement
    // of outputs in the layout.
    //
    // The output layout utility automatically adds a wl_output global to
    // the display, which Wayland clients can see to find out information
    // about the output (such as DPI, scale factor, manufacturer, etc).
    wlr_output_layout_output *l_output =
        wlr_output_layout_add_auto(server->output_layout, wlr_output);
    wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);
}
