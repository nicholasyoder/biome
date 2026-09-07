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

// wlr_output_commit_state() "may fail for any reason" per its own doc
// comment, even for a state that wlr_output_test_state() would accept - a
// busy DRM atomic commit on one connector is enough on real multi-monitor
// KMS setups. Retrying blind after a short delay clears that class of
// transient failure without needing to know why a given commit failed.
constexpr int kIdleBlankRetryDelayMs = 250;

// Returns false if any output's commit failed.
bool set_all_outputs_enabled(BiomeServer *server, bool enabled) {
    bool all_ok = true;
    BiomeOutput *output;
    wl_list_for_each(output, &server->outputs, link) {
        wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, enabled);
        if (enabled && output->wlr->current_mode == nullptr) {
            // A connector that reconnected while blanked (the idle_blanked
            // check in server_new_output, core/output.cpp) never got a real
            // modeset - it was committed disabled, and a DRM connector can't
            // take on a mode while inactive. Without a mode to hand the
            // backend here, this commit has nothing to turn on and fails.
            wlr_output_mode *mode = wlr_output_preferred_mode(output->wlr);
            if (mode != nullptr) {
                wlr_output_state_set_mode(&state, mode);
            }
        }
        if (!wlr_output_commit_state(output->wlr, &state)) {
            wlr_log(WLR_ERROR, "idle-blank: failed to %s output %s",
                    enabled ? "wake" : "blank", output->wlr->name);
            all_ok = false;
        }
        wlr_output_state_finish(&state);
    }
    return all_ok;
}

int on_retry_timeout(void *data) {
    auto *server = static_cast<BiomeServer *>(data);
    set_all_outputs_enabled(server, server->idle_blank_retry_target);
    return 0;
}

// Arms (or re-arms) a single retry attempt at the given target state. Not
// chained - if the retry itself fails, the outputs just stay wrong until the
// next idle/wake transition tries again, rather than risking a busy-loop of
// error logs against a monitor that never cooperates.
void schedule_retry(BiomeServer *server, bool enabled) {
    server->idle_blank_retry_target = enabled;
    if (server->idle_blank_retry_timer == nullptr) {
        wl_event_loop *event_loop = wl_display_get_event_loop(server->display);
        server->idle_blank_retry_timer = wl_event_loop_add_timer(event_loop, on_retry_timeout, server);
    }
    wl_event_source_timer_update(server->idle_blank_retry_timer, kIdleBlankRetryDelayMs);
}

// Fires after kIdleBlankTimeoutMs of no input. Blanks the outputs and then
// goes dormant - idle_blank_notify_activity() is what re-arms it.
int on_idle_timeout(void *data) {
    auto *server = static_cast<BiomeServer *>(data);
    if (!set_all_outputs_enabled(server, false)) {
        schedule_retry(server, false);
    }
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
        if (!set_all_outputs_enabled(server, true)) {
            schedule_retry(server, true);
        }
        server->idle_blanked = false;
    }
    wl_event_source_timer_update(server->idle_blank_timer, kIdleBlankTimeoutMs);
}
