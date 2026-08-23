// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/cursor.h"

#include "desktop/decoration_bridge.h"
#include "desktop/toplevel.h"

#include <linux/input-event-codes.h>

static void server_cursor_motion(wl_listener *listener, void *data);
static void server_cursor_motion_absolute(wl_listener *listener, void *data);
static void server_cursor_button(wl_listener *listener, void *data);
static void server_cursor_axis(wl_listener *listener, void *data);
static void server_cursor_frame(wl_listener *listener, void *data);

void cursor_init(BiomeServer *server) {
    server->cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
    server->cursor_mgr = wlr_xcursor_manager_create(nullptr, 24);

    server->cursor_mode = BiomeCursorMode::Passthrough;
    server->cursor_motion.notify = server_cursor_motion;
    wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
    server->cursor_motion_absolute.notify = server_cursor_motion_absolute;
    wl_signal_add(&server->cursor->events.motion_absolute, &server->cursor_motion_absolute);
    server->cursor_button.notify = server_cursor_button;
    wl_signal_add(&server->cursor->events.button, &server->cursor_button);
    server->cursor_axis.notify = server_cursor_axis;
    wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
    server->cursor_frame.notify = server_cursor_frame;
    wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);
}

void reset_cursor_mode(BiomeServer *server) {
    server->cursor_mode = BiomeCursorMode::Passthrough;
    server->grabbed_toplevel = nullptr;
}

static void drag_icon_tree_handle_destroy(wl_listener *listener, void *data) {
    (void)data;
    BiomeServer *server = wl_container_of(listener, server, drag_icon_tree_destroy);
    wl_list_remove(&server->drag_icon_tree_destroy.link);
    server->drag_icon_tree = nullptr;
}

void drag_icon_create(BiomeServer *server, wlr_drag_icon *drag_icon) {
    server->drag_icon_tree = wlr_scene_drag_icon_create(&server->scene->tree, drag_icon);
    if (server->drag_icon_tree == nullptr) {
        return;
    }
    wlr_scene_node_set_position(&server->drag_icon_tree->node, server->cursor->x, server->cursor->y);
    server->drag_icon_tree_destroy.notify = drag_icon_tree_handle_destroy;
    wl_signal_add(&server->drag_icon_tree->node.events.destroy, &server->drag_icon_tree_destroy);
}

void begin_interactive(BiomeToplevel *toplevel, BiomeCursorMode mode, uint32_t edges,
        bool check_pointer_focus) {
    BiomeServer *server = toplevel->server;
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
    server->grabbed_toplevel = toplevel;
    server->cursor_mode = mode;

    if (mode == BiomeCursorMode::Move) {
        server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
        server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
    } else {
        wlr_box geo_box;
        toplevel_get_geometry(toplevel, &geo_box);

        double border_x = (toplevel->scene_tree->node.x + decoration_border_width(toplevel, toplevel->maximized) + geo_box.x) +
            ((edges & WLR_EDGE_RIGHT) ? geo_box.width : 0);
        double border_y = (toplevel->scene_tree->node.y + decoration_titlebar_height(toplevel, toplevel->maximized) + geo_box.y) +
            ((edges & WLR_EDGE_BOTTOM) ? geo_box.height : 0);
        server->grab_x = server->cursor->x - border_x;
        server->grab_y = server->cursor->y - border_y;

        server->grab_geobox = geo_box;
        server->grab_geobox.x +=
            static_cast<int>(toplevel->scene_tree->node.x) + decoration_border_width(toplevel, toplevel->maximized);
        server->grab_geobox.y +=
            static_cast<int>(toplevel->scene_tree->node.y) + decoration_titlebar_height(toplevel, toplevel->maximized);

        server->resize_edges = edges;
    }
}

static void process_cursor_move(BiomeServer *server, uint32_t time) {
    (void)time;
    BiomeToplevel *toplevel = server->grabbed_toplevel;
    if (toplevel->maximized) {
        // A titlebar press left the maximized state untouched (see
        // handle_decoration_click) so a plain click-and-release doesn't
        // unmaximize. Now that the pointer has actually moved, restore the
        // window under the cursor: capture what fraction of the maximized
        // frame the cursor was holding, then re-anchor the grab to that same
        // fraction of the just-restored frame - standard "drag titlebar to
        // restore" WM convention.
        wlr_box old_geo;
        toplevel_get_geometry(toplevel, &old_geo);
        double old_frame_w =
            old_geo.width + decoration_border_width(toplevel, true) + decoration_border_right_width(toplevel, true);
        double old_frame_h =
            old_geo.height + decoration_titlebar_height(toplevel, true) + decoration_border_bottom_height(toplevel, true);
        double fraction_x = (server->cursor->x - toplevel->scene_tree->node.x) / old_frame_w;
        double fraction_y = (server->cursor->y - toplevel->scene_tree->node.y) / old_frame_h;

        // Use restore_box (the pre-maximize size) rather than
        // toplevel_get_geometry() right after restoring: for xdg-shell
        // toplevels the client applies its new size asynchronously (see
        // process_cursor_resize below), so the surface geometry doesn't
        // reflect the restored size yet once set_toplevel_maximized() returns.
        wlr_box restore_box = toplevel->restore_box;

        set_toplevel_maximized(toplevel, false);

        double new_frame_w =
            restore_box.width + decoration_border_width(toplevel, false) + decoration_border_right_width(toplevel, false);
        double new_frame_h =
            restore_box.height + decoration_titlebar_height(toplevel, false) + decoration_border_bottom_height(toplevel, false);
        server->grab_x = fraction_x * new_frame_w;
        server->grab_y = fraction_y * new_frame_h;
    }
    int x = static_cast<int>(server->cursor->x - server->grab_x);
    int y = static_cast<int>(server->cursor->y - server->grab_y);
    if (toplevel->maximize_reposition_pending) {
        // set_toplevel_maximized() above requested the restore but (xdg-shell)
        // hasn't heard back yet - moving the frame now would show its
        // still-maximized-size buffer following the cursor. Track the target
        // without moving anything; xdg_toplevel_commit applies it once the
        // matching buffer lands.
        toplevel->maximize_pending_x = x;
        toplevel->maximize_pending_y = y;
        return;
    }
    wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
    // The X server has no notion of our border - tell it about the visible
    // content position, not the container's.
    toplevel_sync_position(
        toplevel, x + decoration_border_width(toplevel, toplevel->maximized), y + decoration_titlebar_height(toplevel, toplevel->maximized));
}

static void process_cursor_resize(BiomeServer *server, uint32_t time) {
    (void)time;
    BiomeToplevel *toplevel = server->grabbed_toplevel;
    double border_x = server->cursor->x - server->grab_x;
    double border_y = server->cursor->y - server->grab_y;
    int new_left = server->grab_geobox.x;
    int new_right = server->grab_geobox.x + server->grab_geobox.width;
    int new_top = server->grab_geobox.y;
    int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

    if (server->resize_edges & WLR_EDGE_TOP) {
        new_top = static_cast<int>(border_y);
        if (new_top >= new_bottom) {
            new_top = new_bottom - 1;
        }
    } else if (server->resize_edges & WLR_EDGE_BOTTOM) {
        new_bottom = static_cast<int>(border_y);
        if (new_bottom <= new_top) {
            new_bottom = new_top + 1;
        }
    }
    if (server->resize_edges & WLR_EDGE_LEFT) {
        new_left = static_cast<int>(border_x);
        if (new_left >= new_right) {
            new_left = new_right - 1;
        }
    } else if (server->resize_edges & WLR_EDGE_RIGHT) {
        new_right = static_cast<int>(border_x);
        if (new_right <= new_left) {
            new_right = new_left + 1;
        }
    }

    wlr_box geo_box;
    toplevel_get_geometry(toplevel, &geo_box);

    int new_width = new_right - new_left;
    int new_height = new_bottom - new_top;

    if (toplevel->type != BiomeToplevelType::Xdg) {
        // Xwayland surfaces own their absolute position and have no async
        // commit to defer to (unlike xdg-shell below), so position and size
        // are sent together immediately.
        wlr_scene_node_set_position(&toplevel->scene_tree->node,
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
    if (server->drag_icon_tree != nullptr) {
        wlr_scene_node_set_position(&server->drag_icon_tree->node, server->cursor->x, server->cursor->y);
    }

    if (server->cursor_mode == BiomeCursorMode::Move) {
        process_cursor_move(server, time);
        return;
    } else if (server->cursor_mode == BiomeCursorMode::Resize) {
        process_cursor_resize(server, time);
        return;
    }

    double sx, sy;
    wlr_seat *seat = server->seat;
    wlr_surface *surface = nullptr;
    BiomeToplevel *toplevel = desktop_toplevel_at(server,
        server->cursor->x, server->cursor->y, &surface, &sx, &sy);
    if (!toplevel) {
        // No client surface under the cursor - either nothing at all, or our
        // own decoration. Update the cursor image and hover state either way,
        // including resize-direction hints over a window's edges.
        biome_decoration::Region region = biome_decoration::Region::None;
        BiomeToplevel *decoration_toplevel =
            decoration_toplevel_at(server, server->cursor->x, server->cursor->y, &region);
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, resize_cursor_name(region));
        update_decoration_hover(server, decoration_toplevel, region);
    } else {
        update_decoration_hover(server, nullptr, biome_decoration::Region::None);
    }
    if (surface) {
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, time, sx, sy);
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
    BiomeServer *server = wl_container_of(listener, server, cursor_motion);
    auto *event = static_cast<wlr_pointer_motion_event *>(data);
    wlr_cursor_move(server->cursor, &event->pointer->base,
        event->delta_x, event->delta_y);
    process_cursor_motion(server, event->time_msec);
}

void server_cursor_motion_absolute(wl_listener *listener, void *data) {
    // Forwarded by the cursor for an _absolute_ motion event (0..1 on each
    // axis) - e.g. wlroots running nested in a Wayland/X11 window, where the
    // mouse can enter from any edge, so the cursor has to be warped there.
    BiomeServer *server = wl_container_of(listener, server, cursor_motion_absolute);
    auto *event = static_cast<wlr_pointer_motion_absolute_event *>(data);
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    process_cursor_motion(server, event->time_msec);
}

void server_cursor_button(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, cursor_button);
    auto *event = static_cast<wlr_pointer_button_event *>(data);
    wlr_seat_pointer_notify_button(server->seat,
        event->time_msec, event->button, event->state);

    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        // Commits an armed button's action (min/max/close) only if this
        // release lands back on the same button that was pressed - armed
        // state was only ever set from a BTN_LEFT press, so a release of
        // some other button while BTN_LEFT is still held shouldn't commit.
        if (event->button == BTN_LEFT) {
            handle_decoration_release(server);
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
            focus_toplevel(decoration_toplevel, toplevel_surface(decoration_toplevel));
            if (event->button == BTN_LEFT) {
                set_decoration_pressed(server, decoration_toplevel, region);
                constexpr uint32_t kDoubleClickMs = 400;
                if (region == biome_decoration::Region::Titlebar &&
                        server->last_left_click_toplevel == decoration_toplevel &&
                        event->time_msec - server->last_left_click_time <= kDoubleClickMs) {
                    // Double-click on the titlebar toggles maximize, standard
                    // WM convention - consume the click pair so a third quick
                    // click doesn't immediately toggle it back again.
                    set_toplevel_maximized(decoration_toplevel, !decoration_toplevel->maximized);
                    server->last_left_click_toplevel = nullptr;
                } else {
                    server->last_left_click_toplevel = decoration_toplevel;
                    server->last_left_click_time = event->time_msec;
                    handle_decoration_press(decoration_toplevel, region);
                }
            }
            return;
        }

        double sx, sy;
        wlr_surface *surface = nullptr;
        BiomeToplevel *toplevel = desktop_toplevel_at(server,
            server->cursor->x, server->cursor->y, &surface, &sx, &sy);
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
            if (!unmanaged_xwayland || server->session_locked) {
                // The plain (non-notify_*) enter bypasses any active seat
                // keyboard grab - needed so this reliably wins even when
                // `surface` is an xdg_popup that requested xdg_popup.grab,
                // whose own keyboard grab makes wlr_seat_keyboard_notify_enter()
                // a deliberate no-op (see desktop/xdg_shell.cpp's
                // xdg_popup_map for the full explanation). Equivalent to the
                // notify_* variant whenever no such grab is active, which
                // covers every other case reaching here (the panel, a lock
                // surface).
                wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
                wlr_seat_keyboard_enter(server->seat, surface,
                    keyboard ? keyboard->keycodes : nullptr,
                    keyboard ? keyboard->num_keycodes : 0,
                    keyboard ? &keyboard->modifiers : nullptr);
            }
        }
        focus_toplevel(toplevel, surface);
    }
}

void server_cursor_axis(wl_listener *listener, void *data) {
    // Forwarded by the cursor for an axis event, e.g. a scroll wheel.
    BiomeServer *server = wl_container_of(listener, server, cursor_axis);
    auto *event = static_cast<wlr_pointer_axis_event *>(data);
    wlr_seat_pointer_notify_axis(server->seat,
        event->time_msec, event->orientation, event->delta,
        event->delta_discrete, event->source, event->relative_direction);
}

void server_cursor_frame(wl_listener *listener, void *data) {
    (void)data;
    // Frame events group preceding pointer events sent in the same batch
    // (e.g. simultaneous axis events).
    BiomeServer *server = wl_container_of(listener, server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}
