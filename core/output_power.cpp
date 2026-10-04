// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/output_power.h"

#include "core/output.h"
#include "desktop/session_lock.h"

namespace {

// wlr_output_commit_state() can fail transiently even for a valid state (a
// busy DRM commit on another connector); one delayed retry clears that.
constexpr int kRetryDelayMs = 250;

// Longer than an HPD bounce, short enough that a deliberate replug comes up lit.
constexpr time_t kReconnectOffWindowSec = 10;

// Commits wlr enabled = !powered_off. Returns false on failure.
bool apply_power(BiomeOutput *output) {
    if (output->disabled) {
        return true; // disabled since the retry was armed
    }
    bool on = !output->powered_off;
    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, on);
    // A connector that reconnected while off was never modeset.
    if (on && output->wlr->current_mode == nullptr) {
        if (wlr_output_mode *mode = wlr_output_preferred_mode(output->wlr)) {
            wlr_output_state_set_mode(&state, mode);
        }
    }
    bool ok = wlr_output_commit_state(output->wlr, &state);
    wlr_output_state_finish(&state);
    if (!ok) {
        wlr_log(WLR_ERROR, "output-power: failed to turn output %s %s", output->wlr->name, on ? "on" : "off");
        return false;
    }
    if (!on) {
        // An off output will never present the locked frame it's waiting on.
        output->pending_lock_frame = false;
        session_lock_maybe_send_locked(output->server);
    }
    return true;
}

int on_retry_timeout(void *data) {
    apply_power(static_cast<BiomeOutput *>(data));
    return 0;
}

void cancel_retry(BiomeOutput *output) {
    if (output->power_retry_timer != nullptr) {
        wl_event_source_remove(output->power_retry_timer);
        output->power_retry_timer = nullptr;
    }
}

void set_power(BiomeOutput *output, bool on) {
    output->powered_off = !on;
    cancel_retry(output);
    if (output->wlr->enabled == on || apply_power(output)) {
        return;
    }
    wl_event_loop *event_loop = wl_display_get_event_loop(output->server->display);
    output->power_retry_timer = wl_event_loop_add_timer(event_loop, on_retry_timeout, output);
    wl_event_source_timer_update(output->power_retry_timer, kRetryDelayMs);
}

void handle_set_mode(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, output_power_set_mode);
    auto *event = static_cast<wlr_output_power_v1_set_mode_event *>(data);
    BiomeOutput *output = biome_output_from_wlr(server, event->output);
    if (output == nullptr || output->disabled) {
        return; // a logically disabled output stays off regardless
    }
    set_power(output, event->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
}

} // namespace

void output_power_init(BiomeServer *server) {
    server->output_power_manager = wlr_output_power_manager_v1_create(server->display);
    server->output_power_set_mode.notify = handle_set_mode;
    wl_signal_add(&server->output_power_manager->events.set_mode, &server->output_power_set_mode);
}

void output_power_wake_all(BiomeServer *server) {
    BiomeOutput *output;
    wl_list_for_each(output, &server->outputs, link) {
        if (output->powered_off) {
            set_power(output, true);
        }
    }
}

void output_power_handle_output_destroy(BiomeOutput *output) {
    cancel_retry(output);
    if (output->powered_off && output->wlr->name != nullptr) {
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        output->server->recently_unplugged_off[output->wlr->name] = now;
    }
}

bool output_power_reconnected_off(BiomeServer *server, const char *name) {
    if (name == nullptr) {
        return false;
    }
    auto it = server->recently_unplugged_off.find(name);
    if (it == server->recently_unplugged_off.end()) {
        return false;
    }
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    bool recent = now.tv_sec - it->second.tv_sec < kReconnectOffWindowSec;
    server->recently_unplugged_off.erase(it);
    return recent;
}
