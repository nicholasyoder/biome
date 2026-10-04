// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/output_management.h"

#include "core/idle_blank.h"
#include "core/output.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace {

// Records an output's committed state as its config, for server_new_output()
// to re-apply if the connector reconnects.
void remember_live_state(BiomeOutput *output) {
    wlr_output *wlr_output = output->wlr;
    if (wlr_output->name == nullptr) {
        return;
    }
    OutputConfig &cfg = output->server->output_configs[wlr_output->name];
    cfg.enabled = !output->disabled;
    if (wlr_output->current_mode != nullptr) {
        cfg.mode = OutputConfig::Mode{wlr_output->current_mode->width, wlr_output->current_mode->height,
                                      wlr_output->current_mode->refresh};
    }
    cfg.scale = wlr_output->scale;
    cfg.transform = wlr_output->transform;
    if (cfg.enabled) {
        cfg.position = std::make_pair(output->layout_x, output->layout_y);
    }
}

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

// The layout box a head would occupy once applied. Mirrors
// wlr_output_effective_resolution (integer truncation included).
wlr_box head_layout_box(const wlr_output_head_v1_state &state) {
    int width = 0, height = 0;
    if (state.mode != nullptr) {
        width = state.mode->width;
        height = state.mode->height;
    } else if (state.custom_mode.width > 0 && state.custom_mode.height > 0) {
        width = state.custom_mode.width;
        height = state.custom_mode.height;
    } else {
        // Same fallback handle_configuration uses for a mode-less enabled head.
        wlr_output_mode *mode =
            state.output->current_mode != nullptr ? state.output->current_mode : wlr_output_preferred_mode(state.output);
        if (mode != nullptr) {
            width = mode->width;
            height = mode->height;
        }
    }
    if (state.transform & 1) {
        std::swap(width, height);
    }
    float scale = state.scale > 0.0f ? state.scale : 1.0f;
    return wlr_box{state.x, state.y, static_cast<int>(width / scale), static_cast<int>(height / scale)};
}

// A gap between outputs traps the cursor (wlr_cursor clamps to the closest
// layout point), so the enabled outputs must form one edge-connected group.
// Overlap is allowed (mirroring).
bool layout_is_connected(BiomeServer *server, wlr_output_configuration_v1 *config) {
    std::vector<wlr_box> boxes;
    BiomeOutput *output;
    wl_list_for_each(output, &server->outputs, link) {
        wlr_output_configuration_head_v1 *configured = nullptr;
        wlr_output_configuration_head_v1 *head;
        wl_list_for_each(head, &config->heads, link) {
            if (head->state.output == output->wlr) {
                configured = head;
                break;
            }
        }
        if (configured != nullptr) {
            if (configured->state.enabled) {
                boxes.push_back(head_layout_box(configured->state));
            }
        } else if (!output->disabled) {
            wlr_box box = {};
            wlr_output_layout_get_box(server->output_layout, output->wlr, &box);
            boxes.push_back(box);
        }
    }
    if (boxes.empty()) {
        wlr_log(WLR_ERROR, "output-management: configuration leaves no enabled output");
        return false;
    }

    std::vector<bool> reached(boxes.size(), false);
    std::vector<size_t> pending = {0};
    reached[0] = true;
    while (!pending.empty()) {
        const wlr_box a = boxes[pending.back()];
        pending.pop_back();
        for (size_t i = 0; i < boxes.size(); ++i) {
            if (reached[i]) {
                continue;
            }
            const wlr_box &b = boxes[i];
            int overlap_w = std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x);
            int overlap_h = std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y);
            if ((overlap_w > 0 && overlap_h >= 0) || (overlap_h > 0 && overlap_w >= 0)) {
                reached[i] = true;
                pending.push_back(i);
            }
        }
    }
    if (std::find(reached.begin(), reached.end(), false) != reached.end()) {
        wlr_log(WLR_ERROR, "output-management: layout has a gap between outputs; positions must be edge-adjacent");
        return false;
    }
    return true;
}

void handle_configuration(BiomeServer *server, wlr_output_configuration_v1 *config, bool commit) {
    // Not input, so the idle timer wouldn't wake the outputs itself, and a
    // modeset on a blanked DRM connector fails.
    if (commit) {
        idle_blank_notify_activity(server);
    }

    size_t states_len = 0;
    wlr_backend_output_state *states = wlr_output_configuration_v1_build_state(config, &states_len);
    bool ok = states != nullptr && layout_is_connected(server, config);

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
            remember_live_state(output);
        }
        output_layout_settled(server);
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
