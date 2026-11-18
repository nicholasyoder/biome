// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Shared by layer_shell.cpp and layer_shell_fade.cpp only.

#pragma once

#include "desktop/layer_shell.h"

#include <algorithm>
#include <ctime>

// On server->layer.surfaces (all outputs; arrange_layers() filters).
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
    // fade_start stays zero until the output's fade warmup ends.
    bool fading_in = false;
    timespec fade_start = {};

    // ScanoutSnapshot kind only; outlives this wrapper (server->layer.scanout_fading).
    struct BiomeScanoutFade *scanout_fade = nullptr;
};

// A layer-shell surface's rendered content, kept alive and fading out for
// kFadeDurationMs after the client unmaps it (see handle_layer_surface_unmap()).
// `tree` is an orphaned holding tree owned solely by this struct, freed once
// the fade-out finishes.
struct BiomeFadingOutSurface {
    wl_list link = {}; // server->layer.fading_out
    BiomeOutput *output = nullptr;
    wlr_scene_tree *tree = nullptr;
    timespec fade_start = {};
    float fade_from = 1.0f; // starting opacity - see handle_layer_surface_unmap
};

// FadeKind::ScanoutSnapshot: bakes each fade step into an opaque buffer,
// since any opacity != 1 node blocks direct scanout. Outlives its
// BiomeLayerSurface; scene_buffer is a sibling of the surface's tree, so
// wlroots' teardown of that tree never touches it.
enum class ScanoutFadePhase { In, Out };

struct BiomeScanoutFade {
    wl_list link = {}; // server->layer.scanout_fading
    BiomeOutput *output = nullptr;
    // Null once fade-out begins.
    BiomeLayerSurface *wrapper = nullptr;

    // Replaced every tick.
    wlr_scene_buffer *scene_buffer = nullptr;
    // The desktop behind it, captured at map and again when fade-out starts
    // (forest-startup's cover maps before the desktop exists).
    wlr_texture *snapshot_texture = nullptr;
    // The client's content frozen when fade-out starts (its surface goes
    // right after unmap). Null during fade-in.
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
    timespec fade_start = {}; // zero during fade-in until the output's fade warmup ends
    float fade_from = 0.0f; // continuity value - same role as BiomeFadingOutSurface::fade_from
};

// Must stay comfortably under forest-logout's 250ms post-close quit delay
// (logout.cpp's cancel()/start_action()) - the fade-out needs the client
// process to still be alive for its own duration, or it gets cut short.
static constexpr int kFadeDurationMs = 220;

static bool fade_started(const timespec &start) {
    return start.tv_sec != 0 || start.tv_nsec != 0;
}

// 0 for a fade still waiting out BiomeOutput::fade_warmup_ticks.
static float elapsed_fraction(const timespec &start, int duration_ms) {
    if (!fade_started(start)) {
        return 0.0f;
    }
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
