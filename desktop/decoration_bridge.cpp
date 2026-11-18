// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/decoration_bridge.h"

#include "desktop/window_menu.h"

#include "core/cursor.h"
#include "decoration/renderer.h"
#include "decoration/theme.h"
#include "desktop/qt_buffer.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

// Cached per frame, so a read never restyles the shared widget.
static const biome_decoration::DecorationFrame::Metrics kNoDecoration;

static const biome_decoration::DecorationFrame::Metrics &metrics_of(const BiomeToplevel *toplevel, bool maximized) {
    return toplevel_decorated(toplevel) ? toplevel->decoration_frame->metrics(maximized) : kNoDecoration;
}

int decoration_border_width(const BiomeToplevel *toplevel, bool maximized) {
    return metrics_of(toplevel, maximized).left;
}

int decoration_titlebar_height(const BiomeToplevel *toplevel, bool maximized) {
    return metrics_of(toplevel, maximized).top;
}

int decoration_border_right_width(const BiomeToplevel *toplevel, bool maximized) {
    return metrics_of(toplevel, maximized).right;
}

int decoration_border_bottom_height(const BiomeToplevel *toplevel, bool maximized) {
    return metrics_of(toplevel, maximized).bottom;
}

using biome_decoration::Strip;

static wlr_scene_buffer *toplevel_strip(BiomeToplevel *toplevel, Strip strip) {
    return toplevel->decoration_strips[static_cast<int>(strip)];
}

static void clear_decoration_strips(BiomeToplevel *toplevel) {
    for (wlr_scene_buffer *strip : toplevel->decoration_strips) {
        wlr_scene_buffer_set_buffer(strip, nullptr);
    }
}

static void decoration_rescale_idle(void *data) {
    auto *toplevel = static_cast<BiomeToplevel *>(data);
    toplevel->decoration_rescale_idle = nullptr;
    render_toplevel_decoration(toplevel);
}

// Follows the top strip's primary output (largest overlap), the same one wlroots
// takes a client surface's preferred scale from. Deferred to idle: this fires
// from inside wlr_scene_buffer_set_buffer() and node moves.
static void handle_decoration_outputs_update(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, decoration_outputs_update);
    wlr_scene_output *primary = toplevel_strip(toplevel, Strip::Top)->primary_output;
    if (primary == nullptr || primary->output->scale == toplevel->decoration_scale) {
        return;
    }
    toplevel->decoration_scale = primary->output->scale;
    if (toplevel->decoration_rescale_idle == nullptr) {
        toplevel->decoration_rescale_idle = wl_event_loop_add_idle(
            wl_display_get_event_loop(toplevel->server->display), decoration_rescale_idle, toplevel);
    }
}

void create_toplevel_decoration(BiomeToplevel *toplevel) {
    toplevel->decoration_frame = biome_decoration::create_decoration_frame();
    for (wlr_scene_buffer *&strip : toplevel->decoration_strips) {
        strip = wlr_scene_buffer_create(toplevel->scene_tree, nullptr);
    }
    toplevel->decoration_scale = 1.0f;
    toplevel->decoration_outputs_update.notify = handle_decoration_outputs_update;
    wl_signal_add(&toplevel_strip(toplevel, Strip::Top)->events.outputs_update, &toplevel->decoration_outputs_update);
}

void destroy_toplevel_decoration(BiomeToplevel *toplevel) {
    wl_list_remove(&toplevel->decoration_outputs_update.link);
    if (toplevel->decoration_rescale_idle != nullptr) {
        wl_event_source_remove(toplevel->decoration_rescale_idle);
        toplevel->decoration_rescale_idle = nullptr;
    }
    delete toplevel->decoration_frame;
    toplevel->decoration_frame = nullptr;
}

void render_toplevel_decoration(BiomeToplevel *toplevel) {
    if (toplevel == nullptr || toplevel->decoration_frame == nullptr || !toplevel->placed) {
        return;
    }
    if (!toplevel_decorated(toplevel)) {
        // The client asked for no decoration - clear any previously-set
        // buffer too, in case this is a re-render after the hint changed.
        clear_decoration_strips(toplevel);
        toplevel->decoration_rendered_once = false; // else re-decorating hits the skip-cache
        if (toplevel->content_tree != nullptr) {
            wlr_scene_node_set_position(&toplevel->content_tree->node, 0, 0);
        }
        return;
    }
    wlr_box geo;
    toplevel_get_geometry(toplevel, &geo);
    int width = geo.width > 0 ? geo.width : 0;
    int height = geo.height > 0 ? geo.height : 0;

    // toplevel->maximized flips the instant set_toplevel_maximized() is
    // called, but for xdg-shell the frame's actual size/position lags behind
    // until the resized buffer lands (reposition_pending - see its
    // declaration). Rendering the border/titlebar art with the new state
    // while geo/scene_tree are still the old ones bakes in the wrong border
    // thickness (border painted 0px one render, then N px the next, while
    // still sized+positioned for the old state) - every interior element
    // (title, buttons, corner radius) shifts by that border delta inside a
    // frame that hasn't actually moved or resized yet, which is exactly the
    // demaximize position-jump bug. Render (and derive content_tree's
    // offset) using the state that matches whatever's still on screen until
    // xdg_toplevel_commit resolves the pending move and calls back in here
    // with the flag already clear, at which point both the art and the
    // frame's real position/size update together. Fullscreen doesn't need
    // the same treatment: toplevel_decorated() already forces border/
    // titlebar to 0 the instant toplevel->fullscreen flips, which is a
    // one-frame content_tree offset glitch rather than a wrong-border-
    // thickness one - not worth the extra plumbing this same trick would
    // need to also cover it.
    bool render_maximized = toplevel->reposition_pending
        ? !toplevel->maximized
        : toplevel->maximized;

    const char *title = toplevel_title(toplevel);

    if (!toplevel->decoration_rendered_once) {
        // No buffer yet, so no outputs_update to take the scale from.
        wlr_box frame_box;
        toplevel_get_frame_box(toplevel, &frame_box);
        wlr_output *output = output_with_largest_overlap(toplevel->server, frame_box);
        toplevel->decoration_scale = output != nullptr ? output->scale : 1.0f;
    }

    // Called on every surface commit, including plain content-only repaints
    // (e.g. scrolling) - skip the render below if nothing changed since last
    // time. Title/icon are compared by serial (see title_serial).
    const bool frame_same = toplevel->decoration_rendered_once &&
            toplevel->last_decoration_width == width &&
            toplevel->last_decoration_height == height &&
            toplevel->last_decoration_focused == toplevel->focused &&
            toplevel->last_decoration_urgent == toplevel->urgent &&
            toplevel->last_decoration_maximized == render_maximized &&
            toplevel->last_decoration_scale == toplevel->decoration_scale;
    // Only the titlebar shows these.
    const bool titlebar_same = toplevel->last_decoration_hovered == toplevel->hovered_region &&
            toplevel->last_decoration_pressed == toplevel->pressed_region &&
            toplevel->last_decoration_icon_serial == toplevel->icon_serial &&
            toplevel->last_decoration_title_serial == toplevel->title_serial;
    if (frame_same && titlebar_same) {
        return;
    }

    biome_decoration::DecorationFrame *widget = toplevel->decoration_frame;
    if (!biome_decoration::update_decoration(widget, width, height,
            toplevel->focused, toplevel->urgent, render_maximized, title, toplevel->icon,
            toplevel->hovered_region, toplevel->pressed_region)) {
        return;
    }
    for (int i = 0; i < biome_decoration::kStripCount; ++i) {
        const auto strip = static_cast<Strip>(i);
        wlr_scene_buffer *node = toplevel->decoration_strips[i];
        const QRect rect = widget->stripRect(strip);
        wlr_scene_node_set_position(&node->node, rect.x(), rect.y());
        if (frame_same && strip != Strip::Top) {
            continue;
        }
        if (!set_scene_frame(node, biome_decoration::render_decoration_strip(widget, strip, toplevel->decoration_scale))) {
            wlr_scene_buffer_set_buffer(node, nullptr); // the theme gives this edge no size
        }
    }

    toplevel->decoration_rendered_once = true;
    toplevel->last_decoration_width = width;
    toplevel->last_decoration_height = height;
    toplevel->last_decoration_focused = toplevel->focused;
    toplevel->last_decoration_urgent = toplevel->urgent;
    toplevel->last_decoration_maximized = render_maximized;
    toplevel->last_decoration_hovered = toplevel->hovered_region;
    toplevel->last_decoration_pressed = toplevel->pressed_region;
    toplevel->last_decoration_icon_serial = toplevel->icon_serial;
    toplevel->last_decoration_title_serial = toplevel->title_serial;
    toplevel->last_decoration_scale = toplevel->decoration_scale;

    // Re-syncs content_tree to this render's border/titlebar metrics rather
    // than trusting the snapshot taken at creation, in case the theme sizes
    // the maximized/non-maximized state differently.
    if (toplevel->content_tree != nullptr) {
        wlr_scene_node_set_position(&toplevel->content_tree->node,
            decoration_border_width(toplevel, render_maximized), decoration_titlebar_height(toplevel, render_maximized));
    }
}

void reload_decoration_theme(BiomeServer *server, const QString &dir, const QString &name) {
    // Content stays put: remember where each window's content sits, restyle,
    // then move the frame so the content origin is unchanged.
    std::vector<std::pair<BiomeToplevel *, wlr_box>> origins;
    BiomeToplevel *toplevel;
    wl_list_for_each(toplevel, &server->toplevels, link) {
        origins.push_back({toplevel, {
            static_cast<int>(toplevel->scene_tree->node.x) + decoration_border_width(toplevel, toplevel->maximized),
            static_cast<int>(toplevel->scene_tree->node.y) + decoration_titlebar_height(toplevel, toplevel->maximized),
            0, 0}});
    }

    biome_decoration::load_decoration_theme(dir, name);

    for (auto &[tl, origin] : origins) {
        tl->decoration_frame->layoutFor(200, 200);
        tl->decoration_frame->invalidateMetrics();
        tl->decoration_rendered_once = false;
        if (!tl->placed) {
            continue;
        }
        toplevel_set_position(tl, origin.x - decoration_border_width(tl, tl->maximized),
            origin.y - decoration_titlebar_height(tl, tl->maximized));
        render_toplevel_decoration(tl);
    }
    // Re-fit maximized/fullscreen windows to the new metrics.
    toplevels_relocate_for_layout(server);
}

BiomeToplevel *decoration_toplevel_at_node(
        wlr_scene_node *node, double sx, double sy, biome_decoration::Region *out_region) {
    if (node == nullptr || node->type != WLR_SCENE_NODE_BUFFER) {
        return nullptr;
    }
    // Strips are direct children of scene_tree, whose data is the toplevel.
    wlr_scene_tree *tree = node->parent;
    BiomeToplevel *toplevel = tree ? static_cast<BiomeToplevel *>(tree->node.data) : nullptr;
    if (toplevel == nullptr || toplevel->scene_tree != tree) {
        return nullptr;
    }
    wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    if (std::find(std::begin(toplevel->decoration_strips), std::end(toplevel->decoration_strips), scene_buffer)
            == std::end(toplevel->decoration_strips)) {
        return nullptr;
    }
    // Strip-local to frame-local.
    sx += node->x;
    sy += node->y;

    wlr_box geo;
    toplevel_get_geometry(toplevel, &geo);
    int width = geo.width > 0 ? geo.width : 0;
    int height = geo.height > 0 ? geo.height : 0;
    biome_decoration::Region region = toplevel->decoration_frame->hitTest(
        static_cast<int>(sx), static_cast<int>(sy), width, height, toplevel->maximized);
    if (region == biome_decoration::Region::None) {
        return nullptr;
    }
    *out_region = region;
    return toplevel;
}

BiomeToplevel *decoration_toplevel_at(
        BiomeServer *server, double lx, double ly, biome_decoration::Region *out_region) {
    double sx, sy;
    wlr_scene_node *node = scene_node_at(server, lx, ly, &sx, &sy);
    return decoration_toplevel_at_node(node, sx, sy, out_region);
}

static bool is_button_region(biome_decoration::Region region) {
    using biome_decoration::Region;
    return region == Region::ButtonMinimize || region == Region::ButtonMaximize
        || region == Region::ButtonClose;
}

void update_decoration_hover(BiomeServer *server, BiomeToplevel *toplevel,
        biome_decoration::Region region) {
    BiomeToplevel *new_hovered = is_button_region(region) ? toplevel : nullptr;
    biome_decoration::Region new_region =
        new_hovered != nullptr ? region : biome_decoration::Region::None;

    BiomeToplevel *old_hovered = server->decor.hovered;
    if (old_hovered == new_hovered && (old_hovered == nullptr || old_hovered->hovered_region == new_region)) {
        return;
    }

    if (old_hovered != nullptr && old_hovered != new_hovered) {
        old_hovered->hovered_region = biome_decoration::Region::None;
        render_toplevel_decoration(old_hovered);
    }
    if (new_hovered != nullptr) {
        new_hovered->hovered_region = new_region;
        render_toplevel_decoration(new_hovered);
    }
    server->decor.hovered = new_hovered;
}

void set_decoration_pressed(BiomeServer *server, BiomeToplevel *toplevel,
        biome_decoration::Region region) {
    BiomeToplevel *new_pressed = is_button_region(region) ? toplevel : nullptr;
    biome_decoration::Region new_region =
        new_pressed != nullptr ? region : biome_decoration::Region::None;

    BiomeToplevel *old_pressed = server->decor.pressed;
    if (old_pressed == new_pressed && (old_pressed == nullptr || old_pressed->pressed_region == new_region)) {
        return;
    }

    if (old_pressed != nullptr && old_pressed != new_pressed) {
        old_pressed->pressed_region = biome_decoration::Region::None;
        render_toplevel_decoration(old_pressed);
    }
    if (new_pressed != nullptr) {
        new_pressed->pressed_region = new_region;
        render_toplevel_decoration(new_pressed);
    }
    server->decor.pressed = new_pressed;
}

void refresh_decoration_hover(BiomeServer *server) {
    biome_decoration::Region region = biome_decoration::Region::None;
    BiomeToplevel *toplevel = decoration_toplevel_at(
        server, server->cursor->x, server->cursor->y, &region);
    update_decoration_hover(server, toplevel, region);
}

void clear_decoration_tracking(BiomeServer *server, BiomeToplevel *toplevel) {
    if (server->decor.hovered == toplevel) {
        server->decor.hovered = nullptr;
    }
    if (server->decor.pressed == toplevel) {
        server->decor.pressed = nullptr;
    }
}

const char *resize_cursor_name(biome_decoration::Region region) {
    using biome_decoration::Region;
    switch (region) {
    case Region::ResizeN: return "n-resize";
    case Region::ResizeS: return "s-resize";
    case Region::ResizeE: return "e-resize";
    case Region::ResizeW: return "w-resize";
    case Region::ResizeNE: return "ne-resize";
    case Region::ResizeNW: return "nw-resize";
    case Region::ResizeSE: return "se-resize";
    case Region::ResizeSW: return "sw-resize";
    default: return "default";
    }
}

// Actually performs the action for region on toplevel - the one place that
// knows what each Region does. Called from both handle_decoration_press
// (titlebar/resize, which act on press) and handle_decoration_release
// (buttons, which act on a matching release) below.
static void handle_decoration_click(BiomeToplevel *toplevel, biome_decoration::Region region) {
    using biome_decoration::Region;
    switch (region) {
    case Region::Titlebar:
        // A plain click-and-release must NOT unmaximize (only double-click
        // does - see server_cursor_button); dragging restores-under-cursor
        // on the first real motion instead, in process_cursor_move.
        begin_interactive(toplevel, BiomeCursorMode::Move, 0, false);
        break;
    case Region::Icon: {
        wlr_box frame;
        toplevel_get_frame_box(toplevel, &frame);
        int titlebar = decoration_titlebar_height(toplevel, toplevel->maximized);
        window_menu_open(toplevel, frame.x, frame.y + titlebar, titlebar);
        break;
    }
    case Region::ResizeN:
        begin_interactive(toplevel, BiomeCursorMode::Resize, WLR_EDGE_TOP, false);
        break;
    case Region::ResizeS:
        begin_interactive(toplevel, BiomeCursorMode::Resize, WLR_EDGE_BOTTOM, false);
        break;
    case Region::ResizeE:
        begin_interactive(toplevel, BiomeCursorMode::Resize, WLR_EDGE_RIGHT, false);
        break;
    case Region::ResizeW:
        begin_interactive(toplevel, BiomeCursorMode::Resize, WLR_EDGE_LEFT, false);
        break;
    case Region::ResizeNE:
        begin_interactive(toplevel, BiomeCursorMode::Resize, WLR_EDGE_TOP | WLR_EDGE_RIGHT, false);
        break;
    case Region::ResizeNW:
        begin_interactive(toplevel, BiomeCursorMode::Resize, WLR_EDGE_TOP | WLR_EDGE_LEFT, false);
        break;
    case Region::ResizeSE:
        begin_interactive(toplevel, BiomeCursorMode::Resize, WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT, false);
        break;
    case Region::ResizeSW:
        begin_interactive(toplevel, BiomeCursorMode::Resize, WLR_EDGE_BOTTOM | WLR_EDGE_LEFT, false);
        break;
    case Region::ButtonMaximize:
        set_toplevel_maximized(toplevel, !toplevel->maximized);
        break;
    case Region::ButtonClose:
        close_toplevel(toplevel);
        break;
    case Region::ButtonMinimize:
        set_toplevel_minimized(toplevel, true);
        break;
    case Region::None:
        break;
    }
}

void open_snap_menu(BiomeToplevel *toplevel, bool armed) {
    BiomeServer *server = toplevel->server;
    wlr_box frame;
    toplevel_get_frame_box(toplevel, &frame);
    int titlebar = decoration_titlebar_height(toplevel, toplevel->maximized);
    window_menu_open_snap(toplevel, static_cast<int>(server->cursor->x), frame.y + titlebar, titlebar, armed);
}

// Press-and-hold on the maximize button opens the snap menu.
static int snap_hold_fired(void *data) {
    auto *server = static_cast<BiomeServer *>(data);
    BiomeToplevel *pressed = server->decor.pressed;
    if (pressed == nullptr || pressed->pressed_region != biome_decoration::Region::ButtonMaximize) {
        return 0;
    }
    set_decoration_pressed(server, nullptr, biome_decoration::Region::None);
    open_snap_menu(pressed, true);
    return 0;
}

static void snap_hold_arm(BiomeServer *server) {
    constexpr int kHoldMs = 500;
    if (server->decor.hold_timer == nullptr) {
        server->decor.hold_timer = wl_event_loop_add_timer(wl_display_get_event_loop(server->display), snap_hold_fired, server);
    }
    if (server->decor.hold_timer != nullptr) {
        wl_event_source_timer_update(server->decor.hold_timer, kHoldMs);
    }
}

void decoration_finish(BiomeServer *server) {
    if (server->decor.hold_timer != nullptr) {
        wl_event_source_remove(server->decor.hold_timer);
        server->decor.hold_timer = nullptr;
    }
}

void handle_decoration_press(BiomeToplevel *toplevel, biome_decoration::Region region) {
    if (is_button_region(region)) {
        if (region == biome_decoration::Region::ButtonMaximize) {
            snap_hold_arm(toplevel->server);
        }
        // Armed via set_decoration_pressed (called by the caller before
        // this); the actual action fires on a matching release instead, see
        // handle_decoration_release.
        return;
    }
    handle_decoration_click(toplevel, region);
}

void handle_decoration_release(BiomeServer *server) {
    if (server->decor.hold_timer != nullptr) {
        wl_event_source_timer_update(server->decor.hold_timer, 0);
    }
    BiomeToplevel *pressed = server->decor.pressed;
    if (pressed == nullptr) {
        return;
    }
    biome_decoration::Region release_region = biome_decoration::Region::None;
    BiomeToplevel *released_over = decoration_toplevel_at(
        server, server->cursor->x, server->cursor->y, &release_region);
    if (released_over == pressed && release_region == pressed->pressed_region) {
        handle_decoration_click(pressed, release_region);
    }
}
