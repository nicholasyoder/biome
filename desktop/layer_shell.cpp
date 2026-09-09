// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/layer_shell.h"

#include "core/fade_config.h"
#include "core/output.h"
#include "desktop/toplevel.h"

#include <drm_fourcc.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

// Per-wlr_layer_surface_v1 bookkeeping - not exposed outside this file.
// Kept on BiomeServer::layer_surfaces (all outputs together, not split into
// per-output/per-layer lists) since arrange_layers() below just filters by
// output+layer and the expected surface count is small.
struct BiomeLayerSurface {
    wl_list link = {};
    BiomeServer *server = nullptr;
    BiomeOutput *output = nullptr;
    wlr_layer_surface_v1 *layer_surface = nullptr;
    wlr_scene_layer_surface_v1 *scene_layer_surface = nullptr;

    wl_listener commit = {};
    wl_listener map = {};
    wl_listener unmap = {};
    wl_listener destroy = {};
    wl_listener new_popup = {};

    // Set only for the Opacity fade kind (see layer_surface_fade_kind()).
    bool fading_in = false;
    timespec fade_start = {};

    // Set only for the ScanoutSnapshot fade kind, cleared when fade-out
    // starts. Lives independently on BiomeServer::scanout_fading_surfaces
    // since it must outlive this wrapper across unmap -> destroy.
    struct BiomeScanoutFade *scanout_fade = nullptr;
};

// A layer-shell surface's rendered content, kept alive and fading out for
// kFadeDurationMs after the client unmaps it (see handle_layer_surface_unmap()).
// `tree` is an orphaned holding tree owned solely by this struct, freed once
// the fade-out finishes.
struct BiomeFadingOutSurface {
    wl_list link = {}; // BiomeServer::fading_out_layer_surfaces
    BiomeOutput *output = nullptr;
    wlr_scene_tree *tree = nullptr;
    timespec fade_start = {};
    float fade_from = 1.0f; // starting opacity - see handle_layer_surface_unmap
};

// The second fade mechanism (FadeKind::ScanoutSnapshot below): bakes the
// fade into an opaque buffer's own pixels each tick and presents it at
// opacity 1.0, instead of animating opacity on the client's own translucent
// content. Necessary because wlr_scene's direct-scanout path requires the
// render list to collapse to one entry, and any node with opacity != 1
// blocks that (scene_node_opaque_region()) - so an opaque buffer is the
// only way to make a fading surface scanout-eligible. See
// scanout_fade_create()/scanout_fade_render_tick() for the mechanics.
//
// Lives independently of BiomeLayerSurface, on
// BiomeServer::scanout_fading_surfaces, for the same reason
// BiomeFadingOutSurface does: it must outlive the wrapper across unmap ->
// destroy. Unlike BiomeFadingOutSurface, no reparenting is needed -
// scene_buffer is a *sibling* of scene_layer_surface->tree, never a child,
// so wlroots' teardown of that tree never touches it.
enum class ScanoutFadePhase { In, Out };

struct BiomeScanoutFade {
    wl_list link = {}; // BiomeServer::scanout_fading_surfaces
    BiomeOutput *output = nullptr;
    // Non-null only while the owning BiomeLayerSurface is mapped; nulled by
    // scanout_fade_start_fade_out() when fade-out begins (see
    // frozen_client_texture for why fade-out can't just keep reading it).
    BiomeLayerSurface *wrapper = nullptr;

    // Compositor-owned scene node - sibling of (never child of)
    // scene_layer_surface->tree, same parent, positioned to match it. Its
    // buffer is replaced every render tick via wlr_scene_buffer_set_buffer().
    wlr_scene_buffer *scene_buffer = nullptr;
    // The pre-overlay desktop. Captured at map time for the fade-in, and
    // re-captured fresh in scanout_fade_start_fade_out() right as fade-out
    // begins - a surface like forest-startup's cover maps before anything
    // meaningful is behind it and only reveals the real desktop once other
    // clients have mapped in the meantime, so reusing the map-time capture
    // for fade-out would reveal a stale, mostly-empty frame instead. For a
    // logout dim (background already static for the surface's whole
    // lifetime) the re-capture is a no-op in practice - same image either
    // way.
    wlr_texture *snapshot_texture = nullptr;
    // A frozen copy of the client's content, captured right as fade-out
    // begins, used instead of a live wlr_surface_get_texture() lookup.
    // Necessary: wlroots tears down a closing client's surface almost
    // immediately after unmap, so a live lookup would lose the content on
    // fade-out's first tick. Null during fade-in.
    wlr_texture *frozen_client_texture = nullptr;
    // Per-instance pool of opaque (DRM_FORMAT_XRGB8888) buffers each tick's
    // blend renders into - opaque so wlr_buffer_is_opaque() (format-derived,
    // not content-derived) reports true regardless of what's painted.
    wlr_swapchain *swapchain = nullptr;

    // The real configured box (arrange_layers()'s result), not an assumed
    // full-output box. Direct scanout requires the *entire* output's render
    // list to collapse to one entry, so this only actually reaches scanout
    // on an output where this box is the only visible content.
    wlr_box logical_box = {};
    int buffer_width = 0;  // logical_box size * output scale
    int buffer_height = 0;

    ScanoutFadePhase phase = ScanoutFadePhase::In;
    timespec fade_start = {};
    float fade_from = 0.0f; // continuity value - same role as BiomeFadingOutSurface::fade_from

    // Cold-start compensation: a fresh BiomeScanoutFade's first couple of
    // real ticks are delivered much slower than steady state (measured:
    // ~70-90ms each vs. a clean 16.7ms once warmed up) - a one-time cost in
    // getting new buffers flowing through the display pipeline, present
    // even without direct scan-out. update_layer_surface_fades() spends
    // this many ticks rendering fraction 0.0 (invisible) before starting
    // the timed ramp, so the cold-start cost lands before the animation
    // starts instead of eating into it. Set well above the 2 slow ticks
    // actually measured - an extra warmup tick only costs one steady-state
    // vsync, while under-padding brings the visible blink right back.
    int warmup_ticks_remaining = 6;
};

// Must stay comfortably under forest-logout's 250ms post-close quit delay
// (logout.cpp's cancel()/start_action()) - the fade-out needs the client
// process to still be alive for its own duration, or it gets cut short.
static constexpr int kFadeDurationMs = 220;

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

static float elapsed_fraction(const timespec &start, int duration_ms) {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed_ms = (now.tv_sec - start.tv_sec) * 1000.0
        + (now.tv_nsec - start.tv_nsec) / 1e6;
    return std::clamp(static_cast<float>(elapsed_ms / duration_ms), 0.0f, 1.0f);
}

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

// Tears down a BiomeScanoutFade once its fade-out finishes. Order matters:
// the scene node must be destroyed before the swapchain, since
// wlr_scene_node_destroy() drops the node's lock on its last-set buffer (a
// swapchain slot), which must unwind against a still-live swapchain.
static void scanout_fade_destroy(BiomeScanoutFade *fade) {
    if (fade->wrapper != nullptr) {
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
static void scanout_fade_render_tick(BiomeScanoutFade *fade, float fraction) {
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
static BiomeScanoutFade *scanout_fade_create(BiomeLayerSurface *wrapper) {
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
static void scanout_fade_start_fade_out(BiomeScanoutFade *fade) {
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
            wlr_scene_layer_surface_v1_configure(wrapper->scene_layer_surface, &full_area, &usable_area);
        }
    }
    output->usable_area = usable_area;
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

    arrange_layers(wrapper->output);
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
    arrange_layers(wrapper->output);

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
        break;
    case FadeKind::None:
        break;
    }

    wlr_seat *seat = wrapper->server->seat;
    if (seat->keyboard_state.focused_surface != wrapper->layer_surface->surface) {
        return;
    }
    // Same fallback desktop/xwayland_shell.cpp's unmanaged_unmap uses: hand
    // focus back to the topmost managed toplevel, if any.
    if (!wl_list_empty(&wrapper->server->toplevels)) {
        BiomeToplevel *top = wl_container_of(wrapper->server->toplevels.next, top, link);
        focus_toplevel(top);
    } else {
        wlr_seat_keyboard_notify_clear_focus(seat);
    }
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

    arrange_layers(output);
}

void layer_shell_handle_output_destroy(BiomeOutput *output) {
    BiomeLayerSurface *wrapper, *tmp;
    wl_list_for_each_safe(wrapper, tmp, &output->server->layer_surfaces, link) {
        if (wrapper->output == output) {
            // Fires handle_layer_surface_destroy() synchronously (unmap first
            // if mapped), which frees `wrapper` - safe here since `output`
            // itself is still valid until the caller frees it after this
            // returns.
            wlr_layer_surface_v1_destroy(wrapper->layer_surface);
        }
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
