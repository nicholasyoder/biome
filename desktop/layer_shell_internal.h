// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Private to layer_shell.cpp/layer_shell_fade.cpp: the structs and helpers
// both files need. Not part of layer_shell.h's public API - core/output.cpp
// and other external callers only ever need the handful of functions that
// header declares.

#pragma once

#include "desktop/layer_shell.h"

#include <algorithm>
#include <ctime>

// Per-wlr_layer_surface_v1 bookkeeping - not exposed outside this file.
// Kept on BiomeServer::layer_surfaces (all outputs together, not split into
// per-output/per-layer lists) since arrange_layers() below just filters by
// output+layer and the expected surface count is small.
struct BiomeLayerSurface {
    wl_list link = {};
    BiomeServer *server = nullptr;
    BiomeOutput *output = nullptr;
    // Where the client asked to be; `output` differs only while home is
    // disabled (see layer_shell_reconcile_outputs).
    BiomeOutput *home = nullptr;
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

// The second fade mechanism (FadeKind::ScanoutSnapshot in layer_shell.cpp):
// bakes the fade into an opaque buffer's own pixels each tick and presents it
// at opacity 1.0, instead of animating opacity on the client's own
// translucent content. Necessary because wlr_scene's direct-scanout path
// requires the render list to collapse to one entry, and any node with
// opacity != 1 blocks that (scene_node_opaque_region()) - so an opaque
// buffer is the only way to make a fading surface scanout-eligible. See
// scanout_fade_create()/scanout_fade_render_tick() (layer_shell_fade.cpp)
// for the mechanics.
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

static float elapsed_fraction(const timespec &start, int duration_ms) {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed_ms = (now.tv_sec - start.tv_sec) * 1000.0
        + (now.tv_nsec - start.tv_nsec) / 1e6;
    return std::clamp(static_cast<float>(elapsed_ms / duration_ms), 0.0f, 1.0f);
}

// Defined in layer_shell_fade.cpp; called from layer_shell.cpp's map/unmap
// handlers and update_layer_surface_fades().
BiomeScanoutFade *scanout_fade_create(BiomeLayerSurface *wrapper);
void scanout_fade_start_fade_out(BiomeScanoutFade *fade);
void scanout_fade_render_tick(BiomeScanoutFade *fade, float fraction);
void scanout_fade_destroy(BiomeScanoutFade *fade);
