// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/output.h"

#include "core/layers.h"
#include "core/output_arrange.h"
#include "core/output_management.h"
#include "core/output_power.h"
#include "desktop/idle.h"
#include "desktop/layer_shell.h"
#include "desktop/session_lock.h"
#include "desktop/toplevel.h"

#include <algorithm>
#include <ctime>
#include <vector>

static void server_new_output(wl_listener *listener, void *data);
static void output_sync_geometry(BiomeOutput *output);

// A cursor left outside every output (e.g. its output was disabled or moved)
// would otherwise sit unreachable until the next motion event.
static void rescue_cursor(BiomeServer *server) {
    wlr_box layout_box = {};
    wlr_output_layout_get_box(server->output_layout, nullptr, &layout_box);
    if (wlr_box_empty(&layout_box) ||
        wlr_output_layout_output_at(server->output_layout, server->cursor->x, server->cursor->y) != nullptr) {
        return;
    }
    wlr_cursor_warp_closest(server->cursor, nullptr, server->cursor->x, server->cursor->y);
}

void output_layout_settled(BiomeServer *server) {
    // Layer surfaces first: their exclusive zones feed the maximize targets.
    layer_shell_reconcile_outputs(server);
    toplevels_relocate_for_layout(server);
    rescue_cursor(server);
}

static const OutputConfig *output_config_of(BiomeServer *server, BiomeOutput *output) {
    if (output->wlr->name == nullptr) {
        return nullptr;
    }
    auto it = server->output_configs.find(output->wlr->name);
    return it != server->output_configs.end() ? &it->second : nullptr;
}

void output_relayout(BiomeServer *server) {
    wlr_output_layout *layout = server->output_layout;

    unsigned newest = 0;
    BiomeOutput *output;
    wl_list_for_each(output, &server->outputs, link) {
        const OutputConfig *cfg = output_config_of(server, output);
        if (!output->disabled && cfg != nullptr && cfg->position.has_value()) {
            newest = std::max(newest, cfg->position_generation);
        }
    }

    // Back to the last live layout; positions predating the latest apply are auto-placed instead.
    std::vector<BiomeOutput *> manual;
    wl_list_for_each(output, &server->outputs, link) {
        wlr_output_layout_output *l_output = wlr_output_layout_get(layout, output->wlr);
        if (output->disabled || l_output == nullptr) {
            continue;
        }
        const OutputConfig *cfg = output_config_of(server, output);
        if (cfg == nullptr || !cfg->position.has_value()) {
            if (!l_output->auto_configured) {
                manual.push_back(output);
            }
            continue;
        }
        if (cfg->position_generation < newest) {
            if (!l_output->auto_configured) {
                wlr_log(WLR_INFO, "output %s: position predates the last layout change, auto-placing",
                        output->wlr->name);
                wlr_output_layout_add_auto(layout, output->wlr);
            }
            continue;
        }
        auto [x, y] = *cfg->position;
        if (l_output->auto_configured || l_output->x != x || l_output->y != y) {
            wlr_output_layout_add(layout, output->wlr, x, y);
        }
        manual.push_back(output);
    }
    // Auto-placed outputs are chained right of the rightmost manual one, so can't cause a gap.
    if (manual.size() < 2) {
        return;
    }

    // Top-left anchor, so the result doesn't depend on hotplug order.
    std::vector<wlr_box> boxes(manual.size());
    size_t anchor = 0;
    for (size_t i = 0; i < manual.size(); ++i) {
        wlr_output_layout_get_box(layout, manual[i]->wlr, &boxes[i]);
        if (boxes[i].x < boxes[anchor].x || (boxes[i].x == boxes[anchor].x && boxes[i].y < boxes[anchor].y)) {
            anchor = i;
        }
    }

    std::vector<wlr_box> arranged = boxes;
    bool ok = close_layout_gaps(arranged, anchor);
    std::vector<int> components = layout_components(arranged);
    for (size_t i = 0; i < manual.size(); ++i) {
        const char *name = manual[i]->wlr->name;
        if (!ok && components[i] != components[anchor]) {
            wlr_log(WLR_INFO, "output %s: can't close layout gap without overlap, auto-placing", name);
            wlr_output_layout_add_auto(layout, manual[i]->wlr);
        } else if (arranged[i].x != boxes[i].x || arranged[i].y != boxes[i].y) {
            wlr_log(WLR_INFO, "output %s: closed layout gap, moved to %d,%d", name, arranged[i].x, arranged[i].y);
            wlr_output_layout_add(layout, manual[i]->wlr, arranged[i].x, arranged[i].y);
        }
    }
}

static void output_layout_changed(wl_listener *listener, void *data) {
    (void)data;
    BiomeServer *server = wl_container_of(listener, server, output_layout_change);
    BiomeOutput *output;
    wl_list_for_each(output, &server->outputs, link) {
        output_sync_geometry(output);
    }
    output_management_schedule_publish(server);
}

BiomeOutput *biome_output_from_wlr(BiomeServer *server, wlr_output *wlr_output) {
    BiomeOutput *candidate;
    wl_list_for_each(candidate, &server->outputs, link) {
        if (candidate->wlr == wlr_output) {
            return candidate;
        }
    }
    return nullptr;
}

void output_manager_init(BiomeServer *server) {
    server->output_layout = wlr_output_layout_create(server->display);

    wl_list_init(&server->outputs);
    server->output_configs = load_output_configs();
    server->new_output.notify = server_new_output;
    wl_signal_add(&server->backend->events.new_output, &server->new_output);

    // Handles all rendering and damage tracking; things get added to it at
    // the proper positions and wlr_scene_output_commit() renders a frame.
    server->scene = wlr_scene_create();
    if (server->linux_dmabuf != nullptr) {
        wlr_scene_set_linux_dmabuf_v1(server->scene, server->linux_dmabuf);
    }
    server->scene_layout = wlr_scene_attach_output_layout(server->scene, server->output_layout);
    server->output_layout_change.notify = output_layout_changed;
    wl_signal_add(&server->output_layout->events.change, &server->output_layout_change);
    scene_layers_init(server);
    output_management_init(server);

    // Self-contained: listens to output_layout's own add/change/destroy
    // signals itself, so nothing else needs to touch the returned pointer
    // after creation (same one-call shape as
    // wlr_primary_selection_v1_device_manager_create in core/main.cpp).
    wlr_xdg_output_manager_v1_create(server->display, server->output_layout);
}

// Diagnostic only - logs how often output_frame fires vs. how often it
// actually rendered a new buffer (wlr_scene_output_commit() returns false
// when there's no damage, meaning the frame event fired but nothing was
// redrawn). A high fire count with a low render count would mean something
// is calling wlr_output_schedule_frame() more than it needs to; a high
// render count would mean real damage is churning. Logged every ~10s at
// WLR_DEBUG (default only in dev builds; see BIOME_DEBUG).
namespace {
uint64_t g_frame_fires_since_log = 0;
uint64_t g_frame_renders_since_log = 0;
timespec g_frame_last_log{};
} // namespace

// Called whenever this output has a frame scheduled and ready (not
// unconditionally at the refresh rate - see the damage-driven note below).
static void output_frame(wl_listener *listener, void *data) {
    (void)data;
    BiomeOutput *output = wl_container_of(listener, output, frame);
    BiomeServer *server = output->server;
    wlr_scene *scene = server->scene;

    bool still_fading = update_layer_surface_fades(output);
    idle_update_inhibited(server);

    wlr_scene_output *scene_output = wlr_scene_get_scene_output(scene, output->wlr);
    if (scene_output == nullptr) {
        return; // disabled: out of the layout, no scene output
    }
    bool rendered = wlr_scene_output_commit(scene_output, nullptr);
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);

    g_frame_fires_since_log++;
    if (rendered) {
        g_frame_renders_since_log++;
    }
    if (now.tv_sec - g_frame_last_log.tv_sec >= 10) {
        wlr_log(WLR_DEBUG, "output-frame: %llu fires, %llu actually rendered, over last ~10s",
                static_cast<unsigned long long>(g_frame_fires_since_log),
                static_cast<unsigned long long>(g_frame_renders_since_log));
        g_frame_fires_since_log = 0;
        g_frame_renders_since_log = 0;
        g_frame_last_log = now;
    }

    // Biome's rendering is otherwise damage-driven, not continuous - an
    // in-progress layer-surface fade (desktop/layer_shell.cpp) needs to
    // explicitly keep the frame loop alive for its duration.
    if (still_fading) {
        wlr_output_schedule_frame(output->wlr);
    }

    // ext-session-lock-v1: the `locked` event must not be sent until a
    // locked frame has actually been presented on every output (not just
    // scheduled) - see desktop/session_lock.cpp. This waits for every
    // currently-enabled output to commit at least one frame after the lock
    // began before declaring it satisfied.
    if (server->session_locked && output->pending_lock_frame) {
        output->pending_lock_frame = false;
        session_lock_maybe_send_locked(server);
    }
}

// e.g. Wayland/X11 backends request a new mode when the output window resizes.
static void output_request_state(wl_listener *listener, void *data) {
    BiomeOutput *output = wl_container_of(listener, output, request_state);
    auto *event = static_cast<const wlr_output_event_request_state *>(data);
    wlr_output_commit_state(output->wlr, event->state);

    output_sync_geometry(output);
    output_relayout(output->server);
    output_layout_settled(output->server);
    output_management_schedule_publish(output->server);
}

static void output_destroy(wl_listener *listener, void *data) {
    (void)data;
    BiomeOutput *output = wl_container_of(listener, output, destroy);

    // Must run before this output is freed below - any layer surface still
    // bound to it (BiomeLayerSurface::output) would otherwise dangle until
    // its own destroy listener fires later (e.g. its client disconnecting),
    // which then dereferences freed memory via arrange_layers(). See
    // desktop/layer_shell.cpp for the full explanation.
    layer_shell_handle_output_destroy(output);
    output_power_handle_output_destroy(output);

    // Leaves the layout now (rather than in the layout's own destroy
    // listener, which runs after this one) so the settle below sees it gone.
    output_set_enabled(output, false);

    // Otherwise they leak at (0,0), and the opaque lock_rect covers whatever output sits
    // there while locked. The layer trees are empty by now (surfaces destroyed above).
    wlr_scene_node_destroy(&output->layer_background->node);
    wlr_scene_node_destroy(&output->layer_bottom->node);
    wlr_scene_node_destroy(&output->layer_top->node);
    wlr_scene_node_destroy(&output->layer_overlay->node);
    wlr_scene_node_destroy(&output->lock_tree->node);

    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    BiomeServer *server = output->server;
    free(output);
    output_relayout(server);
    output_layout_settled(server);
    output_management_schedule_publish(server);
}

// The single place output-position-dependent state is updated; idempotent.
static void output_sync_geometry(BiomeOutput *output) {
    wlr_output_layout_output *l_output = wlr_output_layout_get(output->server->output_layout, output->wlr);
    int x = l_output != nullptr ? l_output->x : 0;
    int y = l_output != nullptr ? l_output->y : 0;
    if (l_output != nullptr) {
        output->layout_x = x;
        output->layout_y = y;
    }
    wlr_scene_node_set_position(&output->layer_background->node, x, y);
    wlr_scene_node_set_position(&output->layer_bottom->node, x, y);
    wlr_scene_node_set_position(&output->layer_top->node, x, y);
    wlr_scene_node_set_position(&output->layer_overlay->node, x, y);
    wlr_scene_node_set_position(&output->lock_tree->node, x, y);

    // A stale-sized blank rect would expose desktop content around its edges
    // while locked.
    int width, height;
    wlr_output_effective_resolution(output->wlr, &width, &height);
    if (output->lock_rect->width != width || output->lock_rect->height != height) {
        wlr_scene_rect_set_size(output->lock_rect, width, height);
        if (output->lock_surface != nullptr) {
            wlr_session_lock_surface_v1_configure(output->lock_surface, static_cast<uint32_t>(width),
                                                   static_cast<uint32_t>(height));
        }
    }

    arrange_layers(output);
}

void output_set_enabled(BiomeOutput *output, bool enabled, std::optional<std::pair<int, int>> position) {
    BiomeServer *server = output->server;
    if (enabled) {
        wlr_output_layout_output *l_output =
            position.has_value()
                ? wlr_output_layout_add(server->output_layout, output->wlr, position->first, position->second)
                : wlr_output_layout_add_auto(server->output_layout, output->wlr);
        // Disabling destroys the scene output, so an existing one is already
        // wired to the layout (re-adding it asserts).
        if (wlr_scene_get_scene_output(server->scene, output->wlr) == nullptr) {
            wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, output->wlr);
            wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);
        }
        output->disabled = false;
    } else {
        wlr_output_layout_remove(server->output_layout, output->wlr);
        // Not guaranteed to have been destroyed along with the layout entry.
        if (wlr_scene_output *scene_output = wlr_scene_get_scene_output(server->scene, output->wlr)) {
            wlr_scene_output_destroy(scene_output);
        }
        output->disabled = true;
        // A disabled output will never present the locked frame it's waiting on.
        output->pending_lock_frame = false;
        session_lock_maybe_send_locked(server);
    }
    output_sync_geometry(output);
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
    wlr_log(WLR_INFO, "output %s connected: enabled=%d scale=%.2f pos=%s%d,%d", wlr_output->name, cfg.enabled,
            cfg.scale, cfg.position ? "" : "auto ", cfg.position ? cfg.position->first : 0,
            cfg.position ? cfg.position->second : 0);

    wlr_output_state state;
    wlr_output_state_init(&state);

    // Always bring the output up enabled on this first commit, even one
    // configured enabled=false - committing enabled=false as a connector's
    // very first-ever state (mode included or not) crashes the backend on
    // real DRM/KMS. A config-disabled output gets blanked below instead.
    wlr_output_state_set_enabled(&state, true);

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

    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    // Second commit to blank: configured off, or an HPD bounce on power-off
    // (some DP monitors/docks do this) that must not re-light the screen.
    bool reconnected_off = output_power_reconnected_off(server, wlr_output->name) && cfg.enabled;
    if (!cfg.enabled || reconnected_off) {
        wlr_output_state blank_state;
        wlr_output_state_init(&blank_state);
        wlr_output_state_set_enabled(&blank_state, false);
        wlr_output_commit_state(wlr_output, &blank_state);
        wlr_output_state_finish(&blank_state);
    }

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

    // wlr-layer-shell: one child tree per output-scoped layer, positioned at
    // this output's layout coords by output_sync_geometry().
    output->layer_background = wlr_scene_tree_create(server->layers.background);
    output->layer_bottom = wlr_scene_tree_create(server->layers.bottom);
    output->layer_top = wlr_scene_tree_create(server->layers.top);
    output->layer_overlay = wlr_scene_tree_create(server->layers.overlay);

    // ext-session-lock-v1: created for every output, locked or not, so a
    // monitor that appears while locked is blanked from its first frame.
    // lock_rect is the opaque fallback under any client lock surface.
    output->lock_tree = wlr_scene_tree_create(server->layers.session_lock);
    int lock_width, lock_height;
    wlr_output_effective_resolution(wlr_output, &lock_width, &lock_height);
    output->lock_rect =
        wlr_scene_rect_create(output->lock_tree, lock_width, lock_height, kSessionLockColor);

    // A disabled output stays out of output_layout entirely: it has a
    // current_mode, so layout membership would give it real hit-testable space
    // that toplevel placement (desktop/toplevel.cpp) could target.
    output->disabled = !cfg.enabled;
    output->powered_off = reconnected_off;
    output_set_enabled(output, cfg.enabled, cfg.position);
    // Can move already-present outputs (restoring the last live layout), hence the settle.
    output_relayout(server);
    output_layout_settled(server);
    output_management_schedule_publish(server);
}
