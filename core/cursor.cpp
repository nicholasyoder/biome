// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/cursor.h"

#include "core/pointer_constraints.h"
#include "desktop/decoration_bridge.h"
#include "desktop/idle.h"
#include "desktop/toplevel.h"
#include "desktop/xdg_activation.h"
#include "desktop/window_menu.h"
#include "desktop/xdg_shell.h"

#include <algorithm>
#include <linux/input-event-codes.h>

#include <cstdlib>
#include <ctime>

static void server_cursor_motion(wl_listener *listener, void *data);
static void server_cursor_motion_absolute(wl_listener *listener, void *data);
static void server_cursor_button(wl_listener *listener, void *data);
static void server_cursor_axis(wl_listener *listener, void *data);
static void server_cursor_frame(wl_listener *listener, void *data);
static void cursor_shape_manager_request_set_shape(wl_listener *listener, void *data);
static void server_cursor_swipe_begin(wl_listener *listener, void *data);
static void server_cursor_swipe_update(wl_listener *listener, void *data);
static void server_cursor_swipe_end(wl_listener *listener, void *data);
static void server_cursor_pinch_begin(wl_listener *listener, void *data);
static void server_cursor_pinch_update(wl_listener *listener, void *data);
static void server_cursor_pinch_end(wl_listener *listener, void *data);
static void server_cursor_hold_begin(wl_listener *listener, void *data);
static void server_cursor_hold_end(wl_listener *listener, void *data);

// wlr_xcursor_theme_load(NULL, size) falls back to the literal theme name
// "default" (resolved via ~/.icons/default/index.theme's Inherits= line) -
// it does not consult XCURSOR_THEME itself, so that env var is read here to
// match the convention every other wlroots compositor (sway, river,
// Hyprland) follows for clients that rely on it directly.
static uint32_t xcursor_size_from_env() {
    const char *size_env = getenv("XCURSOR_SIZE");
    if (size_env == nullptr) {
        return 24;
    }
    int parsed = atoi(size_env);
    return parsed > 0 ? static_cast<uint32_t>(parsed) : 24;
}

void cursor_init(BiomeServer *server) {
    server->pointer.server = server;
    server->cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
    server->cursor_mgr = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"), xcursor_size_from_env());
    wlr_xcursor_manager_load(server->cursor_mgr, 1);

    server->grab.mode = BiomeCursorMode::Passthrough;
    server->pointer.motion.notify = server_cursor_motion;
    wl_signal_add(&server->cursor->events.motion, &server->pointer.motion);
    server->pointer.motion_absolute.notify = server_cursor_motion_absolute;
    wl_signal_add(&server->cursor->events.motion_absolute, &server->pointer.motion_absolute);
    server->pointer.button.notify = server_cursor_button;
    wl_signal_add(&server->cursor->events.button, &server->pointer.button);
    server->pointer.axis.notify = server_cursor_axis;
    wl_signal_add(&server->cursor->events.axis, &server->pointer.axis);
    server->pointer.frame.notify = server_cursor_frame;
    wl_signal_add(&server->cursor->events.frame, &server->pointer.frame);

    server->pointer.gestures = wlr_pointer_gestures_v1_create(server->display);
    auto add = [&](wl_listener *l, wl_signal *sig, wl_notify_func_t fn) {
        l->notify = fn;
        wl_signal_add(sig, l);
    };
    auto &ev = server->cursor->events;
    add(&server->pointer.swipe_begin, &ev.swipe_begin, server_cursor_swipe_begin);
    add(&server->pointer.swipe_update, &ev.swipe_update, server_cursor_swipe_update);
    add(&server->pointer.swipe_end, &ev.swipe_end, server_cursor_swipe_end);
    add(&server->pointer.pinch_begin, &ev.pinch_begin, server_cursor_pinch_begin);
    add(&server->pointer.pinch_update, &ev.pinch_update, server_cursor_pinch_update);
    add(&server->pointer.pinch_end, &ev.pinch_end, server_cursor_pinch_end);
    add(&server->pointer.hold_begin, &ev.hold_begin, server_cursor_hold_begin);
    add(&server->pointer.hold_end, &ev.hold_end, server_cursor_hold_end);

    server->pointer.shape_manager = wlr_cursor_shape_manager_v1_create(server->display, 1);
    server->pointer.request_set_shape.notify = cursor_shape_manager_request_set_shape;
    wl_signal_add(&server->pointer.shape_manager->events.request_set_shape, &server->pointer.request_set_shape);
}

void cursor_finish(BiomeServer *server) {
    wlr_xcursor_manager_destroy(server->cursor_mgr);
    wl_list_remove(&server->pointer.motion.link);
    wl_list_remove(&server->pointer.motion_absolute.link);
    wl_list_remove(&server->pointer.button.link);
    wl_list_remove(&server->pointer.axis.link);
    wl_list_remove(&server->pointer.frame.link);
    wl_list_remove(&server->pointer.swipe_begin.link);
    wl_list_remove(&server->pointer.swipe_update.link);
    wl_list_remove(&server->pointer.swipe_end.link);
    wl_list_remove(&server->pointer.pinch_begin.link);
    wl_list_remove(&server->pointer.pinch_update.link);
    wl_list_remove(&server->pointer.pinch_end.link);
    wl_list_remove(&server->pointer.hold_begin.link);
    wl_list_remove(&server->pointer.hold_end.link);
    wl_list_remove(&server->pointer.request_set_shape.link);
    wlr_cursor_destroy(server->cursor);
}

void cursor_set_xcursor(BiomeServer *server, const char *name) {
    snprintf(server->pointer.xcursor_name, sizeof(server->pointer.xcursor_name), "%s", name);
    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, name);
}

void cursor_reload_theme(BiomeServer *server, const char *theme, uint32_t size) {
    wlr_xcursor_manager *new_mgr = wlr_xcursor_manager_create(theme, size);
    wlr_xcursor_manager_load(new_mgr, 1);
    // wlr_cursor keeps the manager pointer; re-point it before freeing the old one.
    // Empty name = a client surface is showing, which doesn't reference the manager.
    wlr_xcursor_manager *old_mgr = server->cursor_mgr;
    server->cursor_mgr = new_mgr;
    if (server->pointer.xcursor_name[0] != '\0') {
        wlr_cursor_set_xcursor(server->cursor, new_mgr, server->pointer.xcursor_name);
    }
    wlr_xcursor_manager_destroy(old_mgr);
    // The pointer's on-screen image only repaints on the next motion event -
    // matches sway's own historical behavior for its `xcursor_theme`
    // command (a live compositor-side reload with no forced immediate
    // redraw), not worth extra plumbing to force here.
}

static void cursor_shape_manager_request_set_shape(wl_listener *listener, void *data) {
    BiomeServer *server = biome_server_from(listener, BiomePointer, request_set_shape);
    auto *event = static_cast<wlr_cursor_shape_manager_v1_request_set_shape_event *>(data);
    if (event->device_type != WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER) {
        // Biome has no tablet-tool support elsewhere either.
        return;
    }
    // Same focused-client check as seat_request_cursor (core/input.cpp) for
    // the wl_pointer.set_cursor path this parallels - any client can send
    // this request, so only honor it from the one that actually has
    // pointer focus.
    if (server->seat->pointer_state.focused_client != event->seat_client) {
        return;
    }
    cursor_set_xcursor(server, wlr_cursor_shape_v1_name(event->shape));
}

void reset_cursor_mode(BiomeServer *server) {
    server->grab.mode = BiomeCursorMode::Passthrough;
    server->grab.toplevel = nullptr;
    server->grab.from_menu = false;
    snap_preview_hide(server);
}

static void drag_icon_tree_handle_destroy(wl_listener *listener, void *data) {
    (void)data;
    BiomeServer *server = biome_server_from(listener, BiomePointer, drag_icon_destroy);
    wl_list_remove(&server->pointer.drag_icon_destroy.link);
    server->pointer.drag_icon = nullptr;
}

void drag_icon_create(BiomeServer *server, wlr_drag_icon *drag_icon) {
    server->pointer.drag_icon = wlr_scene_drag_icon_create(server->layers.osd, drag_icon);
    if (server->pointer.drag_icon == nullptr) {
        return;
    }
    wlr_scene_node_set_position(&server->pointer.drag_icon->node, server->cursor->x, server->cursor->y);
    server->pointer.drag_icon_destroy.notify = drag_icon_tree_handle_destroy;
    wl_signal_add(&server->pointer.drag_icon->node.events.destroy, &server->pointer.drag_icon_destroy);
}

void begin_interactive(BiomeToplevel *toplevel, BiomeCursorMode mode, uint32_t edges,
        bool check_pointer_focus) {
    BiomeServer *server = toplevel->server;
    if (toplevel->fullscreen) {
        return;
    }
    if (check_pointer_focus) {
        wlr_surface *focused_surface = server->seat->pointer_state.focused_surface;
        // wlr_surface_get_root_surface() dereferences its argument
        // unconditionally - nothing having pointer focus at all trivially
        // fails the focus check too, so check for null first rather than
        // passing it through.
        if (focused_surface == nullptr ||
                toplevel_surface(toplevel) != wlr_surface_get_root_surface(focused_surface)) {
            // Deny move/resize requests from unfocused clients.
            return;
        }
    }
    server->grab.toplevel = toplevel;
    server->grab.mode = mode;
    if (mode == BiomeCursorMode::Resize) {
        toplevel_clear_snap(toplevel);
    }

    if (mode == BiomeCursorMode::Move) {
        server->grab.x = server->cursor->x - toplevel->scene_tree->node.x;
        server->grab.y = server->cursor->y - toplevel->scene_tree->node.y;
    } else {
        wlr_box geo_box;
        toplevel_get_geometry(toplevel, &geo_box);

        double border_x = (toplevel->scene_tree->node.x + decoration_border_width(toplevel, toplevel->maximized) + geo_box.x) +
            ((edges & WLR_EDGE_RIGHT) ? geo_box.width : 0);
        double border_y = (toplevel->scene_tree->node.y + decoration_titlebar_height(toplevel, toplevel->maximized) + geo_box.y) +
            ((edges & WLR_EDGE_BOTTOM) ? geo_box.height : 0);
        server->grab.x = server->cursor->x - border_x;
        server->grab.y = server->cursor->y - border_y;

        server->grab.geobox = geo_box;
        server->grab.geobox.x +=
            static_cast<int>(toplevel->scene_tree->node.x) + decoration_border_width(toplevel, toplevel->maximized);
        server->grab.geobox.y +=
            static_cast<int>(toplevel->scene_tree->node.y) + decoration_titlebar_height(toplevel, toplevel->maximized);

        server->grab.resize_edges = edges;
    }
}

static void process_cursor_move(BiomeServer *server, uint32_t time) {
    (void)time;
    BiomeToplevel *toplevel = server->grab.toplevel;
    if (toplevel->maximized || toplevel->snap_zone != SnapZone::None) {
        // A titlebar press left the maximized state untouched (see
        // handle_decoration_click) so a plain click-and-release doesn't
        // unmaximize. Now that the pointer has actually moved, restore the
        // window under the cursor: capture what fraction of the maximized
        // frame the cursor was holding, then re-anchor the grab to that same
        // fraction of the just-restored frame - standard "drag titlebar to
        // restore" WM convention.
        wlr_box old_geo;
        toplevel_get_geometry(toplevel, &old_geo);
        bool was_maximized = toplevel->maximized;
        double old_frame_w = old_geo.width + decoration_border_width(toplevel, was_maximized) +
            decoration_border_right_width(toplevel, was_maximized);
        double old_frame_h = old_geo.height + decoration_titlebar_height(toplevel, was_maximized) +
            decoration_border_bottom_height(toplevel, was_maximized);
        double fraction_x = (server->cursor->x - toplevel->scene_tree->node.x) / old_frame_w;
        double fraction_y = (server->cursor->y - toplevel->scene_tree->node.y) / old_frame_h;

        // Use restore_box (the pre-maximize size) rather than
        // toplevel_get_geometry() right after restoring: for xdg-shell
        // toplevels the client applies its new size asynchronously (see
        // process_cursor_resize below), so the surface geometry doesn't
        // reflect the restored size yet once set_toplevel_maximized() returns.
        wlr_box restore_box = toplevel->restore_box;

        if (was_maximized) {
            set_toplevel_maximized(toplevel, false);
        } else {
            set_toplevel_snapped(toplevel, SnapZone::None);
        }

        double new_frame_w =
            restore_box.width + decoration_border_width(toplevel, false) + decoration_border_right_width(toplevel, false);
        double new_frame_h =
            restore_box.height + decoration_titlebar_height(toplevel, false) + decoration_border_bottom_height(toplevel, false);
        server->grab.x = fraction_x * new_frame_w;
        server->grab.y = fraction_y * new_frame_h;
    }
    if (!server->grab.from_menu) {
        snap_drag_motion(server, server->cursor->x, server->cursor->y);
    }
    int x = static_cast<int>(server->cursor->x - server->grab.x);
    int y = static_cast<int>(server->cursor->y - server->grab.y);
    toplevel->reposition_center = false; // else a window mapped maximized snaps to center on ack
    if (toplevel->reposition_pending) {
        // set_toplevel_maximized() above requested the restore but (xdg-shell)
        // hasn't heard back yet - moving the frame now would show its
        // still-maximized-size buffer following the cursor. Track the target
        // without moving anything; xdg_toplevel_commit applies it once the
        // matching buffer lands.
        toplevel->reposition_pending_x = x;
        toplevel->reposition_pending_y = y;
        return;
    }
    toplevel_set_position(toplevel, x, y);
    // The X server has no notion of our border - tell it about the visible
    // content position, not the container's.
    toplevel_sync_position(
        toplevel, x + decoration_border_width(toplevel, toplevel->maximized), y + decoration_titlebar_height(toplevel, toplevel->maximized));
}

static void process_cursor_resize(BiomeServer *server, uint32_t time) {
    (void)time;
    BiomeToplevel *toplevel = server->grab.toplevel;
    double border_x = server->cursor->x - server->grab.x;
    double border_y = server->cursor->y - server->grab.y;
    int new_left = server->grab.geobox.x;
    int new_right = server->grab.geobox.x + server->grab.geobox.width;
    int new_top = server->grab.geobox.y;
    int new_bottom = server->grab.geobox.y + server->grab.geobox.height;

    if (server->grab.resize_edges & WLR_EDGE_TOP) {
        new_top = static_cast<int>(border_y);
        if (new_top >= new_bottom) {
            new_top = new_bottom - 1;
        }
    } else if (server->grab.resize_edges & WLR_EDGE_BOTTOM) {
        new_bottom = static_cast<int>(border_y);
        if (new_bottom <= new_top) {
            new_bottom = new_top + 1;
        }
    }
    if (server->grab.resize_edges & WLR_EDGE_LEFT) {
        new_left = static_cast<int>(border_x);
        if (new_left >= new_right) {
            new_left = new_right - 1;
        }
    } else if (server->grab.resize_edges & WLR_EDGE_RIGHT) {
        new_right = static_cast<int>(border_x);
        if (new_right <= new_left) {
            new_right = new_left + 1;
        }
    }

    wlr_box geo_box;
    toplevel_get_geometry(toplevel, &geo_box);

    int new_width = new_right - new_left;
    int new_height = new_bottom - new_top;

    if (toplevel->type == BiomeToplevelType::Xdg) {
        // 0 means unset; the opposite edge stays anchored when clamping.
        const wlr_xdg_toplevel_state &state = toplevel->xdg_toplevel->current;
        int width = new_width;
        int height = new_height;
        if (state.max_width > 0) {
            width = std::min(width, state.max_width);
        }
        width = std::max(width, state.min_width);
        if (state.max_height > 0) {
            height = std::min(height, state.max_height);
        }
        height = std::max(height, state.min_height);
        if (server->grab.resize_edges & WLR_EDGE_LEFT) {
            new_left = new_right - width;
        }
        if (server->grab.resize_edges & WLR_EDGE_TOP) {
            new_top = new_bottom - height;
        }
        new_width = width;
        new_height = height;
    }

    if (toplevel->type == BiomeToplevelType::Xwayland) {
        // WM_NORMAL_HINTS min/max, same anchoring as the xdg clamp above.
        const xcb_size_hints_t *hints = toplevel->xwayland_surface->size_hints;
        if (hints != nullptr) {
            int width = new_width;
            int height = new_height;
            if (hints->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) {
                if (hints->max_width > 0) width = std::min(width, hints->max_width);
                if (hints->max_height > 0) height = std::min(height, hints->max_height);
            }
            if (hints->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) {
                width = std::max(width, hints->min_width);
                height = std::max(height, hints->min_height);
            }
            if (server->grab.resize_edges & WLR_EDGE_LEFT) new_left = new_right - width;
            if (server->grab.resize_edges & WLR_EDGE_TOP) new_top = new_bottom - height;
            new_width = width;
            new_height = height;
        }
    }

    if (toplevel->type != BiomeToplevelType::Xdg) {
        // Xwayland surfaces own their absolute position and have no async
        // commit to defer to (unlike xdg-shell below), so position and size
        // are sent together immediately.
        toplevel_set_position(toplevel,
            new_left - geo_box.x - decoration_border_width(toplevel, toplevel->maximized),
            new_top - geo_box.y - decoration_titlebar_height(toplevel, toplevel->maximized));
    }
    // For xdg-shell, moving the window now - ahead of the client's own
    // matching commit - would show the old, not-yet-resized buffer at the
    // new position (the client applies its new size on its own schedule).
    // xdg_toplevel_commit repositions once a buffer matching this size
    // lands, so only the size request goes out here.
    toplevel_set_size(toplevel, new_left - geo_box.x, new_top - geo_box.y, new_width, new_height);
    render_toplevel_decoration(toplevel);
}

static void process_cursor_motion(BiomeServer *server, uint32_t time) {
    if (server->pointer.drag_icon != nullptr) {
        wlr_scene_node_set_position(&server->pointer.drag_icon->node, server->cursor->x, server->cursor->y);
    }

    if (window_menu_active(server)) {
        window_menu_motion(server, server->cursor->x, server->cursor->y);
        return;
    }

    if (server->grab.mode == BiomeCursorMode::Move) {
        process_cursor_move(server, time);
        return;
    } else if (server->grab.mode == BiomeCursorMode::Resize) {
        process_cursor_resize(server, time);
        return;
    }

    wlr_seat *seat = server->seat;

    if (seat->pointer_state.button_count > 0 && seat->pointer_state.focused_surface != nullptr &&
            seat->drag == nullptr) {
        // Implicit grab: while a button is held, keep routing motion to the
        // already-focused surface via its cached origin instead of
        // re-hit-testing, so a drag (e.g. text selection) isn't cut short at
        // the surface edge. Skipped during DnD (seat->drag set), which needs
        // the hit test below to track the drop target across windows.
        double sx = server->cursor->x - server->pointer.focus_origin_x;
        double sy = server->cursor->y - server->pointer.focus_origin_y;
        wlr_seat_pointer_notify_motion(seat, time, sx, sy);
        return;
    }

    // One scene-graph hit test shared below between the client-surface and
    // decoration classifications, instead of each re-walking the scene from
    // scratch - this runs on every motion event, so a second walk here was
    // pure waste on the (very common) no-client-surface path.
    double sx, sy;
    wlr_scene_node *node = scene_node_at(server, server->cursor->x, server->cursor->y, &sx, &sy);
    wlr_surface *surface = nullptr;
    BiomeToplevel *toplevel = desktop_toplevel_at_node(node, &surface);
    if (!toplevel) {
        // No client surface under the cursor - either nothing at all, or our
        // own decoration. Update the cursor image and hover state either way,
        // including resize-direction hints over a window's edges.
        biome_decoration::Region region = biome_decoration::Region::None;
        BiomeToplevel *decoration_toplevel =
            decoration_toplevel_at_node(node, sx, sy, &region);
        // Skip when a non-toplevel surface (layer-shell, popup) is under the
        // cursor: it owns the cursor image, and resetting it here every motion
        // would wipe what the client set on enter.
        if (!surface)
            cursor_set_xcursor(server, resize_cursor_name(region));
        update_decoration_hover(server, decoration_toplevel, region);
    } else {
        update_decoration_hover(server, nullptr, biome_decoration::Region::None);
    }
    if (surface) {
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, time, sx, sy);
        // Cached for the implicit-grab branch above.
        server->pointer.focus_origin_x = server->cursor->x - sx;
        server->pointer.focus_origin_y = server->cursor->y - sy;
    } else {
        // Clear pointer focus so future button events and such are not sent
        // to the last client to have the cursor over it. The _notify_
        // variant (rather than the raw wlr_seat_pointer_clear_focus) is
        // required here: it defers to the active pointer grab, which during
        // a drag-and-drop is what actually sends wl_data_device.leave to
        // the previously-hovered drop target and keeps wlr_drag's focus
        // tracking in sync with the seat's real pointer state.
        wlr_seat_pointer_notify_clear_focus(seat);
    }
}

void server_cursor_motion(wl_listener *listener, void *data) {
    // Forwarded by the cursor for a _relative_ pointer motion event (a delta).
    BiomeServer *server = biome_server_from(listener, BiomePointer, motion);
    auto *event = static_cast<wlr_pointer_motion_event *>(data);
    // Sent unclamped - relative motion is exactly what a locked client wants.
    wlr_relative_pointer_manager_v1_send_relative_motion(server->constraints.relative_manager, server->seat,
        static_cast<uint64_t>(event->time_msec) * 1000, event->delta_x, event->delta_y,
        event->unaccel_dx, event->unaccel_dy);
    double dx = event->delta_x;
    double dy = event->delta_y;
    pointer_constraints_clamp_motion(server, &dx, &dy);
    wlr_cursor_move(server->cursor, &event->pointer->base, dx, dy);
    process_cursor_motion(server, event->time_msec);
}

void server_cursor_motion_absolute(wl_listener *listener, void *data) {
    // Forwarded by the cursor for an _absolute_ motion event (0..1 on each
    // axis) - e.g. wlroots running nested in a Wayland/X11 window, where the
    // mouse can enter from any edge, so the cursor has to be warped there.
    BiomeServer *server = biome_server_from(listener, BiomePointer, motion_absolute);
    auto *event = static_cast<wlr_pointer_motion_absolute_event *>(data);
    // Turned into a delta so constraints (and relative-pointer clients) work
    // nested too.
    double lx, ly;
    wlr_cursor_absolute_to_layout_coords(server->cursor, &event->pointer->base, event->x, event->y, &lx, &ly);
    double dx = lx - server->cursor->x;
    double dy = ly - server->cursor->y;
    wlr_relative_pointer_manager_v1_send_relative_motion(server->constraints.relative_manager, server->seat,
        static_cast<uint64_t>(event->time_msec) * 1000, dx, dy, dx, dy);
    pointer_constraints_clamp_motion(server, &dx, &dy);
    wlr_cursor_move(server->cursor, &event->pointer->base, dx, dy);
    process_cursor_motion(server, event->time_msec);
}

void cursor_rebase(BiomeServer *server) {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    process_cursor_motion(server, static_cast<uint32_t>(now.tv_sec * 1000 + now.tv_nsec / 1000000));
}

void server_cursor_button(wl_listener *listener, void *data) {
    BiomeServer *server = biome_server_from(listener, BiomePointer, button);
    auto *event = static_cast<wlr_pointer_button_event *>(data);
    const bool pressed = event->state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (window_menu_active(server)) {
        window_menu_button(server, event->time_msec, event->button, pressed);
        return;
    }
    if (server->grab.from_menu) {
        if (pressed) {
            reset_cursor_mode(server);
            server->grab.swallow_release = true;
        }
        return;
    }
    if (server->grab.swallow_release && !pressed) {
        server->grab.swallow_release = false;
        return;
    }
    wlr_seat_pointer_notify_button(server->seat,
        event->time_msec, event->button, event->state);

    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        // Commits an armed button's action (min/max/close) only if this
        // release lands back on the same button that was pressed - armed
        // state was only ever set from a BTN_LEFT press, so a release of
        // some other button while BTN_LEFT is still held shouldn't commit.
        if (event->button == BTN_LEFT) {
            handle_decoration_release(server);
            snap_drag_finish(server);
        }
        set_decoration_pressed(server, nullptr, biome_decoration::Region::None);
        reset_cursor_mode(server);

        // Covers the synchronous cases (e.g. Xwayland maximize, which
        // repositions immediately) - see refresh_decoration_hover's comment
        // for the async xdg-shell case this alone doesn't catch.
        refresh_decoration_hover(server);
        return;
    }

    // A press that lands while a drag-and-drop is in progress is not a real
    // window-interaction press - wlr_drag's own pointer grab already
    // consumed the button that's driving the drag (via
    // wlr_seat_pointer_notify_button above), and any other button pressed
    // mid-drag should not be able to refocus or double-click-maximize a
    // window out from under it.
    if (server->seat->drag == nullptr) {
        // A press over our own decoration (titlebar, border, or a button) is
        // handled entirely by Biome - it never reaches desktop_toplevel_at's
        // client-surface lookup below.
        biome_decoration::Region region = biome_decoration::Region::None;
        BiomeToplevel *decoration_toplevel = decoration_toplevel_at(
            server, server->cursor->x, server->cursor->y, &region);
        if (decoration_toplevel != nullptr) {
            // Counts as a press to that window's client for xdg-activation.
            wlr_surface *decorated = toplevel_surface(decoration_toplevel);
            xdg_activation_note_press(server, decorated != nullptr ? wl_resource_get_client(decorated->resource) : nullptr);
            focus_toplevel(decoration_toplevel);
            if (event->button == BTN_RIGHT && region == biome_decoration::Region::ButtonMaximize) {
                open_snap_menu(decoration_toplevel, false);
                return;
            }
            if (event->button == BTN_RIGHT && (region == biome_decoration::Region::Titlebar ||
                    region == biome_decoration::Region::Icon)) {
                window_menu_open(decoration_toplevel, static_cast<int>(server->cursor->x),
                    static_cast<int>(server->cursor->y));
                return;
            }
            if (event->button == BTN_LEFT) {
                set_decoration_pressed(server, decoration_toplevel, region);
                constexpr uint32_t kDoubleClickMs = 400;
                if (region == biome_decoration::Region::Titlebar &&
                        server->pointer.last_click_toplevel == decoration_toplevel &&
                        event->time_msec - server->pointer.last_click_time <= kDoubleClickMs) {
                    // Double-click on the titlebar toggles maximize, standard
                    // WM convention - consume the click pair so a third quick
                    // click doesn't immediately toggle it back again.
                    set_toplevel_maximized(decoration_toplevel, !decoration_toplevel->maximized);
                    server->pointer.last_click_toplevel = nullptr;
                } else {
                    server->pointer.last_click_toplevel = decoration_toplevel;
                    server->pointer.last_click_time = event->time_msec;
                    handle_decoration_press(decoration_toplevel, region);
                }
            }
            return;
        }

        double sx, sy;
        wlr_surface *surface = nullptr;
        BiomeToplevel *toplevel = desktop_toplevel_at(server,
            server->cursor->x, server->cursor->y, &surface, &sx, &sy);
        xdg_activation_note_press(server, surface != nullptr ? wl_resource_get_client(surface->resource) : nullptr);
        if (toplevel == nullptr && surface != nullptr) {
            // A click on a surface with no BiomeToplevel of its own -
            // a layer-shell surface (e.g. the panel), an xdg_popup (e.g. one
            // of its menus), or a session-lock surface (a different
            // monitor's password prompt) - none of which focus_toplevel()
            // below applies to. Excluded outside session lock: an
            // override-redirect Xwayland surface (positions/dismisses
            // itself; clicking one is not expected to steal focus, matching
            // its X11 click-through behavior), identified by being
            // Xwayland-backed - a *managed* Xwayland toplevel would already
            // have resolved via desktop_toplevel_at above, so reaching here
            // with one means it's unmanaged.
            bool unmanaged_xwayland = wlr_xwayland_surface_try_from_wlr_surface(surface) != nullptr;

            // See popup_wants_keyboard_focus() (desktop/xdg_shell.cpp).
            // root_surface resolves through any subsurface (e.g. Chromium
            // renders a popup's content as one) to the actual role surface,
            // both for the check below and for what gets granted focus.
            wlr_surface *root_surface = wlr_surface_get_root_surface(surface);
            wlr_xdg_surface *xdg_surface = wlr_xdg_surface_try_from_wlr_surface(root_surface);
            bool unwanted_popup_focus = xdg_surface != nullptr &&
                xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP &&
                xdg_surface->popup != nullptr && !popup_wants_keyboard_focus(xdg_surface->popup);
            // Spec: a NONE layer surface (e.g. wallpaper) never takes focus, even on click.
            wlr_layer_surface_v1 *layer_surface = wlr_layer_surface_v1_try_from_wlr_surface(root_surface);
            bool unwanted_layer_focus = layer_surface != nullptr &&
                layer_surface->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;

            if ((!unmanaged_xwayland || server->session_locked) && !unwanted_popup_focus && !unwanted_layer_focus) {
                // An ordinary click on the panel reliably lands here
                // (toplevel==nullptr, surface==the panel's) - see
                // grant_keyboard_focus_to_non_toplevel()'s doc comment
                // (desktop/toplevel.h) for why this must go through it
                // rather than calling wlr_seat_keyboard_enter() directly.
                grant_keyboard_focus_to_non_toplevel(server, root_surface);
            }
        }
        focus_toplevel(toplevel);
    }
}

void server_cursor_axis(wl_listener *listener, void *data) {
    // Forwarded by the cursor for an axis event, e.g. a scroll wheel.
    BiomeServer *server = biome_server_from(listener, BiomePointer, axis);
    auto *event = static_cast<wlr_pointer_axis_event *>(data);
    if (window_menu_active(server)) {
        return;
    }
    wlr_seat_pointer_notify_axis(server->seat,
        event->time_msec, event->orientation, event->delta,
        event->delta_discrete, event->source, event->relative_direction);
}

void server_cursor_frame(wl_listener *listener, void *data) {
    (void)data;
    // Frame events group preceding pointer events sent in the same batch
    // (e.g. simultaneous axis events).
    BiomeServer *server = biome_server_from(listener, BiomePointer, frame);
    // libinput emits a frame after every motion/button/axis event.
    idle_notify_activity(server);
    wlr_seat_pointer_notify_frame(server->seat);
}

// Gestures go to the focused client only; during a compositor move/resize
// grab they're dropped (as in sway). Sent events are closed by the
// cursor's frame event.
#define GESTURE_HANDLER(name, event_type, ...)                                              \
    void server_cursor_##name(wl_listener *listener, void *data) {                            \
        BiomeServer *server = biome_server_from(listener, BiomePointer, name);                \
        auto *event = static_cast<event_type *>(data);                                        \
        idle_notify_activity(server);                                                         \
        if (server->grab.mode != BiomeCursorMode::Passthrough) {                           \
            return;                                                                           \
        }                                                                                     \
        wlr_pointer_gestures_v1_send_##name(server->pointer.gestures, server->seat,           \
            event->time_msec, __VA_ARGS__);                                                   \
    }

GESTURE_HANDLER(swipe_begin, wlr_pointer_swipe_begin_event, event->fingers)
GESTURE_HANDLER(swipe_update, wlr_pointer_swipe_update_event, event->dx, event->dy)
GESTURE_HANDLER(swipe_end, wlr_pointer_swipe_end_event, event->cancelled)
GESTURE_HANDLER(pinch_begin, wlr_pointer_pinch_begin_event, event->fingers)
GESTURE_HANDLER(pinch_update, wlr_pointer_pinch_update_event, event->dx, event->dy,
    event->scale, event->rotation)
GESTURE_HANDLER(pinch_end, wlr_pointer_pinch_end_event, event->cancelled)
GESTURE_HANDLER(hold_begin, wlr_pointer_hold_begin_event, event->fingers)
GESTURE_HANDLER(hold_end, wlr_pointer_hold_end_event, event->cancelled)

#undef GESTURE_HANDLER
