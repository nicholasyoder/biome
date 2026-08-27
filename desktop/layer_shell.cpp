// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/layer_shell.h"

#include "core/output.h"
#include "desktop/toplevel.h"

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
};

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

    // wlr_layer_surface_v1_configure() (called from arrange_layers() below,
    // via wlr_scene_layer_surface_v1_configure()) always sends a fresh
    // configure with a new serial, even when the box it computes is
    // byte-for-byte identical to the last one - wlroots does no such
    // deduplication itself. Combined with calling arrange_layers()
    // unconditionally on every commit, that made *any* commit from *any*
    // layer surface on an output reconfigure every layer surface on it,
    // which the client then acks and recommits in response - a
    // self-sustaining reconfigure/recommit loop across every layer surface
    // on the output, all day, paced only by buffer-release/vsync timing
    // (so it stayed cheap on CPU while still starving other event-loop work,
    // like pointer motion, on Biome's single thread - found the hard way via
    // a WAYLAND_DEBUG trace during Workstream A's Forest-side bring-up,
    // see biome/docs/phase4-plan.md). Only actually re-arrange when
    // something layout-relevant changed.
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
    wlr_seat *seat = wrapper->server->seat;
    if (seat->keyboard_state.focused_surface != wrapper->layer_surface->surface) {
        return;
    }
    // Same fallback desktop/xwayland_shell.cpp's unmanaged_unmap uses: hand
    // focus back to the topmost managed toplevel, if any.
    if (!wl_list_empty(&wrapper->server->toplevels)) {
        BiomeToplevel *top = wl_container_of(wrapper->server->toplevels.next, top, link);
        focus_toplevel(top, toplevel_surface(top));
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

void layer_shell_init(BiomeServer *server) {
    wl_list_init(&server->layer_surfaces);
    server->layer_shell = wlr_layer_shell_v1_create(server->display, 4);
    server->new_layer_surface.notify = handle_new_layer_surface;
    wl_signal_add(&server->layer_shell->events.new_surface, &server->new_layer_surface);
}
