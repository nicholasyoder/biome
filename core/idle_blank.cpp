// SPDX-License-Identifier: LGPL-3.0-or-later
//
// STOPGAP(idle-blank) - see idle_blank.h for why this exists and what to
// delete when Phase 6 replaces it.

#include "core/idle_blank.h"

namespace {

// Hardcoded per the stopgap request - no config, no protocol. For manual
// testing, temporarily shrink this (e.g. 10 * 1000 for 10s) and revert
// before committing.
constexpr int kIdleBlankTimeoutMs = 10 * 60 * 1000;

void set_all_outputs_enabled(BiomeServer *server, bool enabled) {
    BiomeOutput *output;
    wl_list_for_each(output, &server->outputs, link) {
        wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, enabled);
        wlr_output_commit_state(output->wlr, &state);
        wlr_output_state_finish(&state);
    }
}

// Fires after kIdleBlankTimeoutMs of no input. Blanks the outputs and then
// goes dormant - idle_blank_notify_activity() is what re-arms it.
int on_idle_timeout(void *data) {
    auto *server = static_cast<BiomeServer *>(data);
    set_all_outputs_enabled(server, false);
    server->idle_blanked = true;
    return 0;
}

} // namespace

void idle_blank_init(BiomeServer *server) {
    wl_event_loop *event_loop = wl_display_get_event_loop(server->display);
    server->idle_blank_timer = wl_event_loop_add_timer(event_loop, on_idle_timeout, server);
    wl_event_source_timer_update(server->idle_blank_timer, kIdleBlankTimeoutMs);
}

void idle_blank_notify_activity(BiomeServer *server) {
    if (server->idle_blanked) {
        set_all_outputs_enabled(server, true);
        server->idle_blanked = false;
    }
    wl_event_source_timer_update(server->idle_blank_timer, kIdleBlankTimeoutMs);
}
