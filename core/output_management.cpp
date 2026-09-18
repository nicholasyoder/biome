// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/output_management.h"

#include "core/idle_blank.h"
#include "core/output.h"

namespace {

void publish_configuration(BiomeServer *server) {
    wlr_output_configuration_v1 *config = wlr_output_configuration_v1_create();
    BiomeOutput *output;
    wl_list_for_each(output, &server->outputs, link) {
        wlr_output_configuration_head_v1 *head = wlr_output_configuration_head_v1_create(config, output->wlr);
        // wlr->enabled is false while idle-blanked; report the logical state.
        head->state.enabled = !output->disabled;
        head->state.x = output->layout_x;
        head->state.y = output->layout_y;
    }
    wlr_output_manager_v1_set_configuration(server->output_manager, config);
}

void on_publish_idle(void *data) {
    auto *server = static_cast<BiomeServer *>(data);
    server->output_publish_idle = nullptr;
    publish_configuration(server);
}

// A cursor left outside every output (e.g. its output was disabled or moved)
// would otherwise sit unreachable until the next motion event.
void rescue_cursor(BiomeServer *server) {
    wlr_box layout_box = {};
    wlr_output_layout_get_box(server->output_layout, nullptr, &layout_box);
    if (wlr_box_empty(&layout_box) ||
        wlr_output_layout_output_at(server->output_layout, server->cursor->x, server->cursor->y) != nullptr) {
        return;
    }
    wlr_cursor_warp_closest(server->cursor, nullptr, server->cursor->x, server->cursor->y);
}

// A batched wlr_backend_test/commit doesn't allocate a buffer the way
// wlr_output_commit_state does, so enabling or re-moding an output fails
// ("No primary frame buffer") without one. Unchanged outputs get none, to
// avoid a black flash.
void attach_buffer_if_needed(wlr_backend_output_state *state) {
    wlr_output *output = state->output;
    wlr_output_state &base = state->base;
    if (!base.enabled || !(base.committed & WLR_OUTPUT_STATE_ENABLED) || (base.committed & WLR_OUTPUT_STATE_BUFFER)) {
        return;
    }
    bool mode_changed = false;
    int width = output->width;
    int height = output->height;
    if (base.committed & WLR_OUTPUT_STATE_MODE) {
        if (base.mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM) {
            width = base.custom_mode.width;
            height = base.custom_mode.height;
            mode_changed = width != output->width || height != output->height ||
                base.custom_mode.refresh != output->refresh;
        } else if (base.mode != nullptr) {
            width = base.mode->width;
            height = base.mode->height;
            mode_changed = base.mode != output->current_mode;
        }
    }
    if (output->enabled && !mode_changed) {
        return;
    }
    wlr_render_pass *pass = wlr_output_begin_render_pass(output, &base, nullptr, nullptr);
    if (pass == nullptr) {
        return;
    }
    wlr_render_rect_options clear = {};
    clear.box.width = width;
    clear.box.height = height;
    clear.color.a = 1.0f;
    clear.blend_mode = WLR_RENDER_BLEND_MODE_NONE;
    wlr_render_pass_add_rect(pass, &clear);
    wlr_render_pass_submit(pass);
}

void handle_configuration(BiomeServer *server, wlr_output_configuration_v1 *config, bool commit) {
    // Not input, so the idle timer wouldn't wake the outputs itself, and a
    // modeset on a blanked DRM connector fails.
    if (commit) {
        idle_blank_notify_activity(server);
    }

    size_t states_len = 0;
    wlr_backend_output_state *states = wlr_output_configuration_v1_build_state(config, &states_len);
    bool ok = states != nullptr;

    for (size_t i = 0; ok && i < states_len; ++i) {
        wlr_output_state &base = states[i].base;
        // A disabled head has no current mode, so clients send none and
        // wlroots turns that into a 0x0 custom mode.
        bool has_mode = (base.committed & WLR_OUTPUT_STATE_MODE) &&
            (base.mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM
                 ? base.custom_mode.width > 0 && base.custom_mode.height > 0
                 : base.mode != nullptr);
        if ((base.committed & WLR_OUTPUT_STATE_ENABLED) && base.enabled && !has_mode) {
            wlr_output *wlr_output = states[i].output;
            wlr_output_mode *mode =
                wlr_output->current_mode != nullptr ? wlr_output->current_mode : wlr_output_preferred_mode(wlr_output);
            if (mode != nullptr) {
                wlr_output_state_set_mode(&base, mode);
            }
        }
        attach_buffer_if_needed(&states[i]);
    }

    ok = ok && wlr_backend_test(server->backend, states, states_len);
    if (ok && commit) {
        ok = wlr_backend_commit(server->backend, states, states_len);
    }

    if (ok && commit) {
        wlr_output_configuration_head_v1 *head;
        wl_list_for_each(head, &config->heads, link) {
            BiomeOutput *output = biome_output_from_wlr(server, head->state.output);
            if (output == nullptr) {
                continue;
            }
            if (head->state.enabled) {
                output_set_enabled(output, true, std::make_pair(head->state.x, head->state.y));
            } else if (!output->disabled) {
                output_set_enabled(output, false);
            }
        }
        rescue_cursor(server);
    }

    if (states != nullptr) {
        for (size_t i = 0; i < states_len; ++i) {
            wlr_output_state_finish(&states[i].base);
        }
        free(states);
    }

    if (ok) {
        wlr_output_configuration_v1_send_succeeded(config);
    } else {
        wlr_log(WLR_ERROR, "output-management: %s failed", commit ? "apply" : "test");
        wlr_output_configuration_v1_send_failed(config);
    }
    wlr_output_configuration_v1_destroy(config);
    output_management_schedule_publish(server);
}

void handle_apply(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, output_apply);
    handle_configuration(server, static_cast<wlr_output_configuration_v1 *>(data), true);
}

void handle_test(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, output_test);
    handle_configuration(server, static_cast<wlr_output_configuration_v1 *>(data), false);
}

} // namespace

void output_management_init(BiomeServer *server) {
    server->output_manager = wlr_output_manager_v1_create(server->display);
    server->output_apply.notify = handle_apply;
    wl_signal_add(&server->output_manager->events.apply, &server->output_apply);
    server->output_test.notify = handle_test;
    wl_signal_add(&server->output_manager->events.test, &server->output_test);
}

void output_management_schedule_publish(BiomeServer *server) {
    if (server->output_manager == nullptr || server->output_publish_idle != nullptr) {
        return;
    }
    wl_event_loop *event_loop = wl_display_get_event_loop(server->display);
    server->output_publish_idle = wl_event_loop_add_idle(event_loop, on_publish_idle, server);
}
