// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/gamma.h"

namespace {

void handle_set_gamma(wl_listener *, void *data) {
    auto *event = static_cast<wlr_gamma_control_manager_v1_set_gamma_event *>(data);
    wlr_output *output = event->output;
    if (!output->enabled) {
        return; // wlroots keeps the control; the ramp applies once a commit enables it
    }
    wlr_output_state state;
    wlr_output_state_init(&state);
    // A null control resets to the default ramp.
    bool ok = wlr_gamma_control_v1_apply(event->control, &state) && wlr_output_commit_state(output, &state);
    wlr_output_state_finish(&state);
    if (!ok && event->control != nullptr) {
        wlr_gamma_control_v1_send_failed_and_destroy(event->control);
    }
}

} // namespace

void gamma_init(BiomeServer *server) {
    server->gamma_manager = wlr_gamma_control_manager_v1_create(server->display);
    server->gamma_set_gamma.notify = handle_set_gamma;
    wl_signal_add(&server->gamma_manager->events.set_gamma, &server->gamma_set_gamma);
}
