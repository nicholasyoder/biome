// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/layer_shell_internal.h"

#include "core/fade_config.h"
#include "core/output.h"
#include "desktop/toplevel.h"
#include "desktop/workspace.h"

#include <cstring>

// Namespace is the client's zwlr_layer_surface_v1 "namespace" argument
// (LayerShellQt::Window::setScope(); `namespace_` since `namespace` is a
// C++ keyword). Fixed policy read from config at startup - see
// core/fade_config.h.
enum class FadeKind { None, Opacity, ScanoutSnapshot };

static FadeKind layer_surface_fade_kind(const BiomeServer *server, const wlr_layer_surface_v1 *layer_surface) {
    if (layer_surface->namespace_ == nullptr) {
        return FadeKind::None;
    }
    if (server->fade_config.fading_namespaces.count(layer_surface->namespace_) != 0) {
        return FadeKind::Opacity;
    }
    if (server->fade_config.scanout_fading_namespaces.count(layer_surface->namespace_) != 0) {
        return FadeKind::ScanoutSnapshot;
    }
    return FadeKind::None;
}

// wlr_scene only stores opacity per wlr_scene_buffer (leaf) node, not per
// tree, so fading a whole surface means walking down to every leaf under it.
static void set_tree_opacity(wlr_scene_tree *tree, float opacity) {
    wlr_scene_node *node;
    wl_list_for_each(node, &tree->children, link) {
        if (node->type == WLR_SCENE_NODE_BUFFER) {
            wlr_scene_buffer_set_opacity(wlr_scene_buffer_from_node(node), opacity);
        } else if (node->type == WLR_SCENE_NODE_TREE) {
            set_tree_opacity(wlr_scene_tree_from_node(node), opacity);
        }
    }
}

// Recursively clones every buffer leaf under `src` into brand-new,
// independent wlr_scene_buffer nodes under `dst_parent`, preserving
// structure/position/opacity. Used instead of reparenting the original
// nodes for the Opacity fade-out: wlr_scene_layer_surface_v1_create() keeps
// its own commit listener wired to each leaf's wl_surface regardless of
// which tree it lives in, and wlroots fires `unmap` *before* that `commit`
// signal - so a reparented-but-original node gets its content nulled out
// moments later, within the same call. Cloning takes an independent lock
// on each leaf's current buffer instead, immune to that.
static void clone_buffer_leaves_into(wlr_scene_tree *src, wlr_scene_tree *dst_parent) {
    wlr_scene_node *node;
    wl_list_for_each(node, &src->children, link) {
        if (node->type == WLR_SCENE_NODE_BUFFER) {
            wlr_scene_buffer *src_buffer = wlr_scene_buffer_from_node(node);
            wlr_scene_buffer *clone = wlr_scene_buffer_create(dst_parent, src_buffer->buffer);
            wlr_scene_node_set_position(&clone->node, node->x, node->y);
            wlr_scene_buffer_set_opacity(clone, src_buffer->opacity);
            wlr_scene_buffer_set_source_box(clone, &src_buffer->src_box);
            wlr_scene_buffer_set_dest_size(clone, src_buffer->dst_width, src_buffer->dst_height);
            wlr_scene_buffer_set_transform(clone, src_buffer->transform);
        } else if (node->type == WLR_SCENE_NODE_TREE) {
            wlr_scene_tree *dst_child = wlr_scene_tree_create(dst_parent);
            wlr_scene_node_set_position(&dst_child->node, node->x, node->y);
            clone_buffer_leaves_into(wlr_scene_tree_from_node(node), dst_child);
        }
    }
}

static wlr_scene_tree *output_layer_tree(BiomeOutput *output, uint32_t layer) {
    switch (layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
        return output->layer_background;
    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
        return output->layer_bottom;
    case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
        return output->layer_overlay;
    case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
    default:
        return output->layer_top;
    }
}

void arrange_layers(BiomeOutput *output) {
    wlr_box full_area = {};
    wlr_output_effective_resolution(output->wlr, &full_area.width, &full_area.height);
    wlr_box usable_area = full_area;

    // Overlay -> top -> bottom -> background: among surfaces that both
    // claim a positive exclusive_zone, the higher layer's claim is resolved
    // first (against the still-full box), matching the convention that a
    // visually-topmost bar gets priority for reserved space over a
    // lower-layer one. wlr_scene_layer_surface_v1_configure() itself is
    // layer-agnostic - it just positions whatever surface it's given
    // against whichever full_area/usable_area it's passed and shrinks
    // usable_area if that surface's own exclusive_zone is positive - so
    // this call order is the only thing encoding that policy.
    static constexpr uint32_t kLayersMostToLeastPriority[] = {
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
    };
    // Two passes (as sway): every exclusive-zone claim across all layers is
    // resolved before any non-exclusive surface is placed, so e.g. an
    // overlay-layer popup still avoids a top-layer panel's reserved strip.
    for (bool exclusive_pass : {true, false}) {
        for (uint32_t layer : kLayersMostToLeastPriority) {
            BiomeLayerSurface *wrapper;
            wl_list_for_each(wrapper, &output->server->layer_surfaces, link) {
                if (wrapper->output != output || wrapper->layer_surface->current.layer != layer) {
                    continue;
                }
                // A surface only becomes `initialized` after its first real
                // commit - configuring it earlier (e.g. right at creation, before
                // any commit) sends a bogus default-state box that the client can
                // briefly render before its real commit corrects it. Skipping
                // here just means "wait for that"; handle_layer_surface_commit()
                // re-arranges once real state lands.
                if (!wrapper->layer_surface->initialized) {
                    continue;
                }
                if ((wrapper->layer_surface->current.exclusive_zone > 0) != exclusive_pass) {
                    continue;
                }
                wlr_scene_layer_surface_v1_configure(wrapper->scene_layer_surface, &full_area, &usable_area);
            }
        }
    }
    output->usable_area = usable_area;
}

// Re-fits maximized/fullscreen windows when a client's exclusive zone moved
// usable_area. Output changes re-fit via output_layout_settled() instead.
static void arrange_layers_refitting(BiomeOutput *output) {
    wlr_box before = output->usable_area;
    arrange_layers(output);
    if (!wlr_box_equal(&before, &output->usable_area)) {
        toplevels_relocate_for_layout(output->server);
    }
}

// Layout-relevant state only - a plain content commit (a client just
// repainting, e.g. every frame a wallpaper or panel widget redraws) leaves
// all of these bits clear and must not retrigger arrange_layers().
static constexpr uint32_t kLayoutRelevantState =
    WLR_LAYER_SURFACE_V1_STATE_DESIRED_SIZE | WLR_LAYER_SURFACE_V1_STATE_ANCHOR |
    WLR_LAYER_SURFACE_V1_STATE_EXCLUSIVE_ZONE | WLR_LAYER_SURFACE_V1_STATE_MARGIN |
    WLR_LAYER_SURFACE_V1_STATE_LAYER;

static void handle_layer_surface_commit(wl_listener *listener, void *data) {
    (void)data;
    BiomeLayerSurface *wrapper = wl_container_of(listener, wrapper, commit);
    wlr_layer_surface_v1 *layer_surface = wrapper->layer_surface;

    // wlr_scene's own surface_reconfigure() unconditionally resets opacity
    // to 1.0 on every commit from this surface (registered before Biome's
    // own commit listener, so it always runs first). Any commit the client
    // makes mid-fade-in snaps opacity back to 1.0 until our next scheduled
    // tick corrects it, which can flash a real frame at full brightness.
    // Re-asserting our current fraction synchronously here closes that
    // window instead of relying solely on periodic ticks to catch up.
    if (wrapper->fading_in) {
        float fraction = elapsed_fraction(wrapper->fade_start, kFadeDurationMs);
        set_tree_opacity(wrapper->scene_layer_surface->tree, fraction);
    }

    // wlr_layer_surface_v1_configure() always sends a fresh configure with a
    // new serial even when the box is byte-for-byte identical to the last
    // one (wlroots does no dedup) - calling arrange_layers() unconditionally
    // on every commit would make any commit from any layer surface on an
    // output reconfigure every surface on it, which acks and recommits in
    // turn: a self-sustaining reconfigure loop across the whole output. Only
    // actually re-arrange when something layout-relevant changed.
    if (!(layer_surface->current.committed & kLayoutRelevantState)) {
        return;
    }

    if (layer_surface->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
        // The client asked to move to a different layer - the scene node
        // wlr_scene_layer_surface_v1_create() made is a fixed child of
        // whatever tree it was given at creation, so a live layer change
        // needs an explicit reparent before the next arrange_layers() call
        // below will place it correctly (arrange_layers() itself only
        // matches surfaces to layers for *iteration order*, not for parenting).
        wlr_scene_node_reparent(&wrapper->scene_layer_surface->tree->node,
            output_layer_tree(wrapper->output, layer_surface->current.layer));
    }

    arrange_layers_refitting(wrapper->output);
}

static void handle_layer_surface_map(wl_listener *listener, void *data) {
    (void)data;
    BiomeLayerSurface *wrapper = wl_container_of(listener, wrapper, map);
    wlr_layer_surface_v1 *layer_surface = wrapper->layer_surface;

    // wlr_scene_layer_surface_v1_configure() only applies a positive
    // exclusive_zone to usable_area once the surface is actually mapped (see
    // layer_surface_exclusive_zone() in wlroots' own scene/layer_shell_v1.c),
    // so an exclusive-zone-reserving surface needs one arrange_layers() pass
    // right at the map transition to make sure that reservation actually
    // takes effect promptly - handle_layer_surface_commit() above is now
    // gated on layout-relevant committed state and won't reliably re-arrange
    // on its own right at this exact point.
    arrange_layers_refitting(wrapper->output);

    switch (layer_surface_fade_kind(wrapper->server, layer_surface)) {
    case FadeKind::Opacity: {
        wrapper->fading_in = true;
        clock_gettime(CLOCK_MONOTONIC, &wrapper->fade_start);
        set_tree_opacity(wrapper->scene_layer_surface->tree, 0.0f);
        // Nothing else guarantees another frame fires soon after a fresh
        // map with no other damage source - Biome's rendering is otherwise
        // damage-driven, not continuous (see output_frame() in core/output.cpp).
        wlr_output_schedule_frame(wrapper->output->wlr);
        break;
    }
    case FadeKind::ScanoutSnapshot:
        wrapper->scanout_fade = scanout_fade_create(wrapper);
        if (wrapper->scanout_fade != nullptr) {
            wlr_output_schedule_frame(wrapper->output->wlr);
        }
        break;
    case FadeKind::None:
        break;
    }

    // While locked, the lock surface holds keyboard focus (desktop/
    // session_lock.cpp) and nothing else may take it - matches the same
    // guard desktop/xwayland_shell.cpp's unmanaged surface map handler
    // applies to its own keyboard-focus grab, for the same reason (this is
    // a seat-focus concern, not something the layer stack's structural
    // z-order containment touches).
    if (!wrapper->server->session_locked
        && layer_surface->current.keyboard_interactive
        != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
        grant_keyboard_focus_to_non_toplevel(wrapper->server, layer_surface->surface);
    }
}

static void handle_layer_surface_unmap(wl_listener *listener, void *data) {
    (void)data;
    BiomeLayerSurface *wrapper = wl_container_of(listener, wrapper, unmap);

    switch (layer_surface_fade_kind(wrapper->server, wrapper->layer_surface)) {
    case FadeKind::Opacity: {
        // Clone (not reparent) into a standalone tree that outlives
        // `wrapper`: wlr_scene_layer_surface_v1_create() keeps each buffer
        // leaf's commit listener wired to its wl_surface regardless of
        // which tree it lives in, and `unmap` fires *before* `commit` -
        // so a reparented-but-original node would still get nulled out
        // moments later. clone_buffer_leaves_into() takes an independent
        // lock per leaf instead, immune to that.
        //
        // fade_from continues from wherever fade-in actually got to, not
        // from 1.0 - otherwise cancelling within the fade-in window (e.g.
        // clicking Cancel a few ms after the logout screen opens) would
        // visibly pop the surface up to full opacity before fading it back
        // down.
        float fade_from = wrapper->fading_in
            ? elapsed_fraction(wrapper->fade_start, kFadeDurationMs) : 1.0f;

        wlr_scene_tree *holding_tree =
            wlr_scene_tree_create(wrapper->scene_layer_surface->tree->node.parent);
        wlr_scene_node_set_position(&holding_tree->node,
            wrapper->scene_layer_surface->tree->node.x,
            wrapper->scene_layer_surface->tree->node.y);
        clone_buffer_leaves_into(wrapper->scene_layer_surface->tree, holding_tree);

        auto *fading = static_cast<BiomeFadingOutSurface *>(calloc(1, sizeof(BiomeFadingOutSurface)));
        fading->output = wrapper->output;
        fading->tree = holding_tree;
        fading->fade_from = fade_from;
        clock_gettime(CLOCK_MONOTONIC, &fading->fade_start);
        wl_list_insert(&wrapper->server->fading_out_layer_surfaces, &fading->link);

        wlr_output_schedule_frame(wrapper->output->wlr);
        break;
    }
    case FadeKind::ScanoutSnapshot:
        // No reparenting needed here. scanout_fade may be null if
        // scanout_fade_create() failed at map time (a no-op on null).
        scanout_fade_start_fade_out(wrapper->scanout_fade);
        // Now owned solely by scanout_fading_surfaces; may be freed before `wrapper`.
        wrapper->scanout_fade = nullptr;
        break;
    case FadeKind::None:
        break;
    }

    wlr_seat *seat = wrapper->server->seat;
    if (seat->keyboard_state.focused_surface != wrapper->layer_surface->surface) {
        return;
    }
    focus_topmost_on_active_workspace(wrapper->server);
}

static void handle_layer_surface_destroy(wl_listener *listener, void *data) {
    (void)data;
    BiomeLayerSurface *wrapper = wl_container_of(listener, wrapper, destroy);
    BiomeOutput *output = wrapper->output;

    wl_list_remove(&wrapper->link);
    wl_list_remove(&wrapper->commit.link);
    wl_list_remove(&wrapper->map.link);
    wl_list_remove(&wrapper->unmap.link);
    wl_list_remove(&wrapper->destroy.link);
    wl_list_remove(&wrapper->new_popup.link);
    free(wrapper);

    arrange_layers_refitting(output);
}

void layer_shell_handle_output_destroy(BiomeOutput *output) {
    // Before the surfaces below: their unmap would otherwise start a fade-out
    // here, and the fade's scene_buffer lives in this output's layer trees.
    BiomeScanoutFade *fade, *fade_tmp;
    wl_list_for_each_safe(fade, fade_tmp, &output->server->scanout_fading_surfaces, link) {
        if (fade->output == output) {
            scanout_fade_destroy(fade);
        }
    }

    BiomeLayerSurface *wrapper, *tmp;
    wl_list_for_each_safe(wrapper, tmp, &output->server->layer_surfaces, link) {
        if (wrapper->output == output) {
            // Fires handle_layer_surface_destroy() synchronously (unmap first
            // if mapped), which frees `wrapper` - safe here since `output`
            // itself is still valid until the caller frees it after this
            // returns.
            wlr_layer_surface_v1_destroy(wrapper->layer_surface);
        } else if (wrapper->home == output) {
            // Displaced here while its home was disabled; the home is gone now.
            wrapper->home = wrapper->output;
        }
    }
}

void layer_shell_reconcile_outputs(BiomeServer *server) {
    BiomeOutput *fallback = nullptr;
    BiomeOutput *candidate;
    wl_list_for_each(candidate, &server->outputs, link) {
        if (!candidate->disabled) {
            fallback = candidate;
            break;
        }
    }

    // A disabled output stops ticking, so its fades would never finish.
    BiomeScanoutFade *fade, *fade_tmp;
    wl_list_for_each_safe(fade, fade_tmp, &server->scanout_fading_surfaces, link) {
        if (fade->output->disabled) {
            scanout_fade_destroy(fade);
        }
    }

    BiomeLayerSurface *wrapper;
    wl_list_for_each(wrapper, &server->layer_surfaces, link) {
        BiomeOutput *desired = !wrapper->home->disabled ? wrapper->home : fallback;
        if (desired == nullptr || desired == wrapper->output) {
            continue;
        }
        // Scanout fades cache per-output swapchain/buffer state; end rather than move.
        if (wrapper->scanout_fade != nullptr) {
            scanout_fade_destroy(wrapper->scanout_fade);
        }
        BiomeOutput *previous = wrapper->output;
        wlr_scene_node_reparent(&wrapper->scene_layer_surface->tree->node,
            output_layer_tree(desired, wrapper->layer_surface->current.layer));
        wrapper->output = desired;
        wrapper->layer_surface->output = desired->wlr;
        arrange_layers(previous);
        arrange_layers(desired);
    }
}

static void handle_layer_surface_new_popup(wl_listener *listener, void *data) {
    BiomeLayerSurface *wrapper = wl_container_of(listener, wrapper, new_popup);
    auto *xdg_popup = static_cast<wlr_xdg_popup *>(data);

    // desktop/xdg_shell.cpp's server_new_xdg_popup already ran for this
    // popup (it fires for every xdg_popup, layer-shell-owned or not) but
    // deliberately skipped scene-node creation since xdg_popup->parent was
    // still null at that point - the parent only becomes known once
    // zwlr_layer_surface_v1.get_popup associates it, which is what fires
    // this signal. Its commit/destroy listeners are already wired; this
    // just supplies the scene node they were missing.
    xdg_popup->base->data = wlr_scene_xdg_surface_create(wrapper->scene_layer_surface->tree, xdg_popup->base);

    // Constrain the popup to its output so a client-requested anchor near
    // the screen edge (e.g. a panel launcher a few pixels from the corner)
    // gets slid back on-screen instead of hanging off it - without this,
    // the positioner's own constraint_adjustment (slide_x/slide_y, which
    // every client here requests by default) has no box to slide within
    // and is silently a no-op. The box must be expressed relative to the
    // popup's parent surface's own top-left, per
    // wlr_xdg_popup_unconstrain_from_box()'s contract - for a layer-shell
    // surface that's wherever arrange_layers() last placed it within the
    // output, which scene_layer_surface->tree->node.x/y already holds,
    // since that tree is a direct child of the output's own
    // layout-position-relative layer tree (see core/output.cpp).
    wlr_box full_area = {};
    wlr_output_effective_resolution(wrapper->output->wlr, &full_area.width, &full_area.height);
    wlr_box unconstrain_box = full_area;
    unconstrain_box.x = -wrapper->scene_layer_surface->tree->node.x;
    unconstrain_box.y = -wrapper->scene_layer_surface->tree->node.y;
    wlr_xdg_popup_unconstrain_from_box(xdg_popup, &unconstrain_box);
}

static void handle_new_layer_surface(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, new_layer_surface);
    auto *layer_surface = static_cast<wlr_layer_surface_v1 *>(data);

    BiomeOutput *output = nullptr;
    if (layer_surface->output != nullptr) {
        output = biome_output_from_wlr(server, layer_surface->output);
    } else if (!wl_list_empty(&server->outputs)) {
        // Per wlr_layer_shell_v1's own doc comment: the output may be null,
        // in which case it's the compositor's responsibility to assign one
        // before returning. Biome has no per-seat "active output" concept
        // yet (workspaces are global, not per-output - see
        // desktop/workspace.h), so this just picks the first output.
        output = wl_container_of(server->outputs.next, output, link);
        layer_surface->output = output->wlr;
    }
    if (output == nullptr) {
        wlr_layer_surface_v1_destroy(layer_surface);
        return;
    }

    auto *wrapper = static_cast<BiomeLayerSurface *>(calloc(1, sizeof(BiomeLayerSurface)));
    wrapper->server = server;
    wrapper->output = output;
    wrapper->home = output;
    wrapper->layer_surface = layer_surface;
    wrapper->scene_layer_surface = wlr_scene_layer_surface_v1_create(
        output_layer_tree(output, layer_surface->current.layer), layer_surface);
    layer_surface->data = wrapper;

    wl_list_insert(&server->layer_surfaces, &wrapper->link);

    wrapper->commit.notify = handle_layer_surface_commit;
    wl_signal_add(&layer_surface->surface->events.commit, &wrapper->commit);
    wrapper->map.notify = handle_layer_surface_map;
    wl_signal_add(&layer_surface->surface->events.map, &wrapper->map);
    wrapper->unmap.notify = handle_layer_surface_unmap;
    wl_signal_add(&layer_surface->surface->events.unmap, &wrapper->unmap);
    wrapper->destroy.notify = handle_layer_surface_destroy;
    wl_signal_add(&layer_surface->events.destroy, &wrapper->destroy);
    wrapper->new_popup.notify = handle_layer_surface_new_popup;
    wl_signal_add(&layer_surface->events.new_popup, &wrapper->new_popup);

    arrange_layers(output);
}

bool update_layer_surface_fades(BiomeOutput *output) {
    bool still_animating = false;
    BiomeServer *server = output->server;

    BiomeLayerSurface *wrapper;
    wl_list_for_each(wrapper, &server->layer_surfaces, link) {
        if (wrapper->output != output || !wrapper->fading_in) {
            continue;
        }
        float fraction = elapsed_fraction(wrapper->fade_start, kFadeDurationMs);
        set_tree_opacity(wrapper->scene_layer_surface->tree, fraction);
        if (fraction >= 1.0f) {
            wrapper->fading_in = false;
        } else {
            still_animating = true;
        }
    }

    BiomeFadingOutSurface *fading, *tmp;
    wl_list_for_each_safe(fading, tmp, &server->fading_out_layer_surfaces, link) {
        if (fading->output != output) {
            continue;
        }
        float fraction = elapsed_fraction(fading->fade_start, kFadeDurationMs);
        set_tree_opacity(fading->tree, fading->fade_from * (1.0f - fraction));
        if (fraction >= 1.0f) {
            wlr_scene_node_destroy(&fading->tree->node);
            wl_list_remove(&fading->link);
            free(fading);
        } else {
            still_animating = true;
        }
    }

    BiomeScanoutFade *scanout_fade, *scanout_tmp;
    wl_list_for_each_safe(scanout_fade, scanout_tmp, &server->scanout_fading_surfaces, link) {
        if (scanout_fade->output != output) {
            continue;
        }

        // Spend a handful of real ticks on invisible (fraction=0.0) content
        // before starting the timed ramp - see warmup_ticks_remaining's
        // comment.
        if (scanout_fade->phase == ScanoutFadePhase::In && scanout_fade->warmup_ticks_remaining > 0) {
            scanout_fade_render_tick(scanout_fade, 0.0f);
            scanout_fade->warmup_ticks_remaining--;
            if (scanout_fade->warmup_ticks_remaining == 0) {
                clock_gettime(CLOCK_MONOTONIC, &scanout_fade->fade_start);
            }
            still_animating = true;
            continue;
        }

        float fraction = elapsed_fraction(scanout_fade->fade_start, kFadeDurationMs);
        float effective = scanout_fade->phase == ScanoutFadePhase::In
            ? fraction
            : scanout_fade->fade_from * (1.0f - fraction);
        scanout_fade_render_tick(scanout_fade, effective);

        if (fraction >= 1.0f) {
            if (scanout_fade->phase == ScanoutFadePhase::Out) {
                scanout_fade_destroy(scanout_fade);
            }
            // else: fade-in complete - scene_buffer keeps showing the last-
            // rendered frame with no further per-tick cost until unmapped.
        } else {
            still_animating = true;
        }
    }

    return still_animating;
}

void layer_shell_init(BiomeServer *server) {
    wl_list_init(&server->layer_surfaces);
    wl_list_init(&server->fading_out_layer_surfaces);
    wl_list_init(&server->scanout_fading_surfaces);
    server->fade_config = load_fade_config();
    server->layer_shell = wlr_layer_shell_v1_create(server->display, 4);
    server->new_layer_surface.notify = handle_new_layer_surface;
    wl_signal_add(&server->layer_shell->events.new_surface, &server->new_layer_surface);
}
