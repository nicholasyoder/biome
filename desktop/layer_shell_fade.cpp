// SPDX-License-Identifier: LGPL-3.0-or-later
//
// The ScanoutSnapshot fade mechanism's GPU compositing pipeline: swapchain
// management, texture capture/blend, frame-timing/warmup compensation.
// Needed specifically to keep a fading wallpaper/logout-dim surface
// scanout-eligible - see BiomeScanoutFade's doc comment (layer_shell_internal.h)
// for why. Driven from layer_shell.cpp's map/unmap handlers and
// update_layer_surface_fades(), which own BiomeLayerSurface's lifecycle;
// this file only ever reads BiomeLayerSurface as data.

#include "desktop/layer_shell_internal.h"

#include <drm_fourcc.h>

#include <cmath>

// Picks a DRM_FORMAT_XRGB8888 modifier list `output`'s primary plane can
// display and builds a swapchain of that format/size. XRGB8888 itself is
// renderable by every wlroots renderer backend, so what's genuinely
// output-specific here is which *modifiers* the primary plane supports.
// Returns nullptr if no swapchain could be created - callers must skip the
// fade for this surface rather than push on with a broken swapchain.
static wlr_swapchain *create_scanout_swapchain(BiomeServer *server, BiomeOutput *output, int width, int height) {
    const wlr_drm_format_set *display_formats =
        wlr_output_get_primary_formats(output->wlr, server->allocator->buffer_caps);
    const wlr_drm_format *format =
        display_formats != nullptr ? wlr_drm_format_set_get(display_formats, DRM_FORMAT_XRGB8888) : nullptr;

    wlr_drm_format_set fallback_set = {};
    if (format == nullptr || format->len == 0) {
        // No constraint advertised, or the primary plane doesn't list
        // XRGB8888 explicitly - DRM_FORMAT_MOD_INVALID is the universal
        // implicit-modifier fallback every allocator must recognize
        // (wlr_allocator_create_buffer()'s own doc comment).
        wlr_drm_format_set_add(&fallback_set, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_INVALID);
        format = wlr_drm_format_set_get(&fallback_set, DRM_FORMAT_XRGB8888);
    }

    // wlr_swapchain_create() copies the format it's given, so fallback_set
    // can be torn down immediately after - the swapchain owns its own copy.
    wlr_swapchain *swapchain = (format != nullptr && format->len > 0)
        ? wlr_swapchain_create(server->allocator, width, height, format)
        : nullptr;
    wlr_drm_format_set_finish(&fallback_set);
    return swapchain;
}

// Tears down a BiomeScanoutFade, normally once its fade-out finishes; if
// ended early while still mapped, the client's own content is shown again.
// Order matters: the scene node must be destroyed before the swapchain, since
// wlr_scene_node_destroy() drops the node's lock on its last-set buffer (a
// swapchain slot), which must unwind against a still-live swapchain.
void scanout_fade_destroy(BiomeScanoutFade *fade) {
    if (fade->wrapper != nullptr) {
        wlr_scene_node_set_enabled(&fade->wrapper->scene_layer_surface->tree->node, true);
        fade->wrapper->scanout_fade = nullptr;
    }
    wlr_scene_node_destroy(&fade->scene_buffer->node);
    wlr_texture_destroy(fade->snapshot_texture);
    if (fade->frozen_client_texture != nullptr) {
        wlr_texture_destroy(fade->frozen_client_texture);
    }
    wlr_swapchain_destroy(fade->swapchain);
    wl_list_remove(&fade->link);
    delete fade;
}

// Renders `texture` faithfully - preserving its own per-pixel alpha rather
// than flattening it against the destination - into a fresh ARGB8888
// buffer and imports the result as an independent texture. Used to freeze
// the client's content into a compositor-owned copy right as fade-out
// begins, since the live surface/texture can be destroyed by wlroots at
// any point afterward.
//
// Deliberately doesn't reuse fade->swapchain (opaque XRGB8888) for this:
// the dim content is genuinely translucent, and rendering it into an
// alpha-less destination would collapse that alpha, flashing black at the
// start of fade-out as the translucent content blends against undefined
// memory in a reused buffer slot.
//
// Returns nullptr on any failure (fade-out falls back to no dim content).
static wlr_texture *capture_frozen_client_texture(BiomeServer *server, wlr_texture *texture, int width, int height) {
    if (texture == nullptr) {
        return nullptr;
    }

    wlr_drm_format_set argb_formats = {};
    wlr_drm_format_set_add(&argb_formats, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_INVALID);
    const wlr_drm_format *format = wlr_drm_format_set_get(&argb_formats, DRM_FORMAT_ARGB8888);
    wlr_buffer *copy_buffer = format != nullptr
        ? wlr_allocator_create_buffer(server->allocator, width, height, format) : nullptr;
    wlr_drm_format_set_finish(&argb_formats);
    if (copy_buffer == nullptr) {
        return nullptr;
    }

    wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(server->renderer, copy_buffer, nullptr);
    if (pass == nullptr) {
        wlr_buffer_drop(copy_buffer);
        return nullptr;
    }

    // Explicit transparent-black overwrite first - BLEND_MODE_NONE, since
    // the default premultiplied blend would treat a fully-transparent
    // source color as a no-op, leaving the destination's undefined initial
    // contents untouched. Needed so the faithful copy below starts from a
    // known-zero base rather than blending against garbage.
    wlr_render_rect_options clear_options = {};
    clear_options.box.width = width;
    clear_options.box.height = height;
    clear_options.blend_mode = WLR_RENDER_BLEND_MODE_NONE;
    wlr_render_pass_add_rect(pass, &clear_options);

    wlr_render_texture_options copy_options = {};
    copy_options.texture = texture;
    copy_options.dst_box.width = width;
    copy_options.dst_box.height = height;
    wlr_render_pass_add_texture(pass, &copy_options);

    if (!wlr_render_pass_submit(pass)) {
        wlr_buffer_drop(copy_buffer);
        return nullptr;
    }

    wlr_texture *frozen = wlr_texture_from_buffer(server->renderer, copy_buffer);
    wlr_buffer_drop(copy_buffer);
    return frozen;
}

// Renders blend(snapshot, dim content, fraction) into a freshly-acquired
// opaque buffer and swaps it onto fade->scene_buffer. Blends toward the
// client's actual painted texture (not a hardcoded black fill), so it
// works for either of forest-logout's DimLevels without the compositor
// needing to know which - the render pass's scalar `alpha` does the rest.
void scanout_fade_render_tick(BiomeScanoutFade *fade, float fraction) {
    // Buffer age is a damage-tracking hint for partial-redraw renderers;
    // irrelevant here since every tick fully repaints the buffer.
    int unused_age = 0;
    wlr_buffer *frame_buffer = wlr_swapchain_acquire(fade->swapchain, &unused_age);
    if (frame_buffer == nullptr) {
        return; // swapchain exhausted - shouldn't happen at one render/tick; skip this tick
    }

    wlr_render_pass *pass =
        wlr_renderer_begin_buffer_pass(fade->output->server->renderer, frame_buffer, nullptr);
    if (pass == nullptr) {
        wlr_buffer_unlock(frame_buffer);
        return;
    }

    // Base layer: the pre-overlay desktop, fully opaque.
    wlr_render_texture_options snapshot_options = {};
    snapshot_options.texture = fade->snapshot_texture;
    wlr_render_pass_add_texture(pass, &snapshot_options);

    // Dim content on top, scaled by fraction. Live lookup during fade-in;
    // the frozen copy during fade-out, since the live surface/texture can
    // be destroyed by wlroots at any point after unmap.
    wlr_texture *client_texture = fade->frozen_client_texture != nullptr
        ? fade->frozen_client_texture
        : (fade->wrapper != nullptr ? wlr_surface_get_texture(fade->wrapper->layer_surface->surface) : nullptr);
    if (client_texture != nullptr) {
        wlr_render_texture_options client_options = {};
        client_options.texture = client_texture;
        client_options.dst_box.width = fade->buffer_width;
        client_options.dst_box.height = fade->buffer_height;
        client_options.alpha = &fraction;
        wlr_render_pass_add_texture(pass, &client_options);
    }

    if (!wlr_render_pass_submit(pass)) {
        wlr_buffer_unlock(frame_buffer);
        return;
    }

    // wlr_scene_buffer_set_buffer() takes its own lock on the buffer, so
    // dropping this call's own acquire-reference right after matches
    // wlroots' own internal idiom after every set_buffer call.
    wlr_scene_buffer_set_buffer(fade->scene_buffer, frame_buffer);
    wlr_buffer_unlock(frame_buffer);
}

// Captures a fresh, fully opaque snapshot of everything on `fade`'s output
// except this surface's own content (kept disabled in the scene graph for
// the surface's whole mapped lifetime - see scanout_fade_create()).
// wlr_output_lock_attach_render() brackets the render because the rest of
// the scene might already be scanout-eligible on its own - without it,
// build_state() could take the direct-scanout branch instead of rendering
// into our swapchain at all. Returns nullptr on any allocation/format
// failure - callers must not assume it succeeded.
static wlr_texture *capture_background_snapshot(BiomeScanoutFade *fade) {
    // fade->scene_buffer doesn't exist yet on the very first (map-time)
    // call. On a fade-out re-capture it's the currently-visible overlay -
    // showing our own last-rendered tick - and must be hidden for this
    // capture the same way the client's own content is hidden below, or
    // "the background" ends up being our own overlay instead of what's
    // actually behind it.
    if (fade->scene_buffer != nullptr) {
        wlr_scene_node_set_enabled(&fade->scene_buffer->node, false);
    }

    wlr_scene_output *scene_output = wlr_scene_get_scene_output(fade->output->server->scene, fade->output->wlr);
    wlr_output_lock_attach_render(fade->output->wlr, true);

    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_scene_output_state_options options = {};
    options.swapchain = fade->swapchain;
    bool built = wlr_scene_output_build_state(scene_output, &state, &options);

    wlr_output_lock_attach_render(fade->output->wlr, false);

    if (fade->scene_buffer != nullptr) {
        wlr_scene_node_set_enabled(&fade->scene_buffer->node, true);
    }

    wlr_buffer *snapshot_buffer = nullptr;
    if (built) {
        // Take our own reference before wlr_output_state_finish() drops
        // build_state's own lock on state.buffer - reversing this order is
        // a use-after-free.
        snapshot_buffer = wlr_buffer_lock(state.buffer);
    }
    wlr_output_state_finish(&state);

    if (snapshot_buffer == nullptr) {
        return nullptr;
    }

    wlr_texture *texture = wlr_texture_from_buffer(fade->output->server->renderer, snapshot_buffer);
    // wlr_texture_from_buffer() locks the buffer itself if it still needs
    // it, so dropping our reference immediately is safe.
    wlr_buffer_unlock(snapshot_buffer);
    return texture;
}

// Starts the ScanoutSnapshot fade for a newly-mapped surface: disables the
// client's own scene content, captures the initial pre-overlay snapshot for
// the fade-in (re-captured fresh in scanout_fade_start_fade_out() when
// fade-out begins - see BiomeScanoutFade::snapshot_texture), and renders
// tick 0 synchronously so the node is never briefly empty. Returns nullptr
// (treat like FadeKind::None) on any allocation/format failure.
BiomeScanoutFade *scanout_fade_create(BiomeLayerSurface *wrapper) {
    BiomeOutput *output = wrapper->output;
    BiomeServer *server = wrapper->server;
    wlr_scene_tree *client_tree = wrapper->scene_layer_surface->tree;

    auto *fade = new BiomeScanoutFade();
    fade->output = output;
    fade->wrapper = wrapper;

    fade->logical_box.x = client_tree->node.x;
    fade->logical_box.y = client_tree->node.y;
    fade->logical_box.width = static_cast<int>(wrapper->layer_surface->current.actual_width);
    fade->logical_box.height = static_cast<int>(wrapper->layer_surface->current.actual_height);
    fade->buffer_width = std::lround(fade->logical_box.width * output->wlr->scale);
    fade->buffer_height = std::lround(fade->logical_box.height * output->wlr->scale);

    // The client's real content is never shown again for this surface's
    // whole mapped lifetime - fade->scene_buffer fully replaces it visually.
    wlr_scene_node_set_enabled(&client_tree->node, false);

    fade->swapchain = create_scanout_swapchain(server, output, fade->buffer_width, fade->buffer_height);
    if (fade->swapchain == nullptr) {
        wlr_log(WLR_ERROR, "Biome: no opaque scanout-capable format available for '%s', skipping scanout fade",
            wrapper->layer_surface->namespace_);
        wlr_scene_node_set_enabled(&client_tree->node, true);
        delete fade;
        return nullptr;
    }

    // Node already disabled above so it contributes nothing to this capture.
    fade->snapshot_texture = capture_background_snapshot(fade);
    if (fade->snapshot_texture == nullptr) {
        wlr_log(WLR_ERROR, "Biome: failed to capture pre-overlay snapshot for '%s', skipping scanout fade",
            wrapper->layer_surface->namespace_);
        wlr_scene_node_set_enabled(&client_tree->node, true);
        wlr_swapchain_destroy(fade->swapchain);
        delete fade;
        return nullptr;
    }

    fade->scene_buffer = wlr_scene_buffer_create(client_tree->node.parent, nullptr);
    wlr_scene_node_set_position(&fade->scene_buffer->node, fade->logical_box.x, fade->logical_box.y);
    // Explicit dest size in *logical* (scene-graph) units, even though the
    // buffer itself is allocated at physical pixel size (buffer_width/
    // buffer_height above). Leaving dest size unset makes wlr_scene treat
    // the buffer's raw pixel dimensions as its logical scene-graph footprint
    // (scene_node_get_size() in wlroots' types/scene/wlr_scene.c falls back
    // to buffer_width/buffer_height when dst_width/dst_height are 0) - the
    // render/scanout path then multiplies that by the output's scale *again*
    // (transform_output_box() -> scale_box()) to get the physical
    // destination. At scale 1 those two bugs cancel out, but at any other
    // scale (e.g. 1.5) the node ends up sized by scale^2 instead of scale,
    // which is what made this fade balloon past the actual output bounds on
    // a scaled display. Setting dest size explicitly to the logical box
    // keeps the buffer/physical-destination 1:1 match direct-scanout
    // eligibility wants (logical_box * scale == buffer_width/buffer_height,
    // by construction above) while fixing the scene-graph footprint.
    wlr_scene_buffer_set_dest_size(fade->scene_buffer, fade->logical_box.width, fade->logical_box.height);

    fade->phase = ScanoutFadePhase::In;
    fade->fade_from = 0.0f;
    wl_list_insert(&server->scanout_fading_surfaces, &fade->link);

    // Render tick 0 synchronously so the node is never briefly empty - this
    // pays the first wlr_swapchain_acquire()'s real allocation cost up
    // front. fade_start is left unset here; update_layer_surface_fades()
    // only stamps it once warmup_ticks_remaining reaches 0 (see that
    // field's comment - one tick alone doesn't cover the cold-start cost).
    scanout_fade_render_tick(fade, 0.0f);

    return fade;
}

// Flips an in-progress ScanoutSnapshot fade to fade-out, continuing from
// wherever it actually reached rather than snapping to full first. No
// reparenting needed here - scene_buffer is a sibling of, never a child of,
// scene_layer_surface->tree, so wlroots' teardown never touches it.
void scanout_fade_start_fade_out(BiomeScanoutFade *fade) {
    if (fade == nullptr) {
        return;
    }
    // While warmup_ticks_remaining > 0, fade_start isn't stamped yet -
    // elapsed_fraction() on a zero timespec would clamp to 1.0 (fully
    // elapsed), wrongly jumping to full opacity instead of "not visible yet".
    float current = fade->warmup_ticks_remaining > 0 ? 0.0f
        : fade->phase == ScanoutFadePhase::In
        ? elapsed_fraction(fade->fade_start, kFadeDurationMs)
        : fade->fade_from * (1.0f - elapsed_fraction(fade->fade_start, kFadeDurationMs));

    // Freeze the client's content before nulling wrapper below - see
    // frozen_client_texture's comment for why a live lookup can't be
    // trusted for the rest of the fade-out.
    wlr_texture *live_texture = fade->wrapper != nullptr
        ? wlr_surface_get_texture(fade->wrapper->layer_surface->surface) : nullptr;
    fade->frozen_client_texture = capture_frozen_client_texture(
        fade->output->server, live_texture, fade->buffer_width, fade->buffer_height);

    // Re-capture the background right as the reveal begins rather than
    // trusting the one taken at map time - see snapshot_texture's comment.
    // Falls back to the map-time capture on failure rather than leaving
    // nothing to show.
    wlr_texture *fresh_snapshot = capture_background_snapshot(fade);
    if (fresh_snapshot != nullptr) {
        wlr_texture_destroy(fade->snapshot_texture);
        fade->snapshot_texture = fresh_snapshot;
    }

    fade->phase = ScanoutFadePhase::Out;
    fade->fade_from = current;
    clock_gettime(CLOCK_MONOTONIC, &fade->fade_start);
    // wrapper may be destroyed at any point from here on;
    // frozen_client_texture above is what render_tick sources from now.
    fade->wrapper = nullptr;
    wlr_output_schedule_frame(fade->output->wlr);
}
