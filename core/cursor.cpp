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
    // Creates a cursor, which is a wlroots utility for tracking the cursor
    // image shown on screen.
    server->cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server->cursor, server->output_layout);

    // Creates an xcursor manager, another wlroots utility which loads up
    // Xcursor themes to source cursor images from and makes sure that
    // cursor images are available at all scale factors on the screen
    // (necessary for HiDPI support).
    server->cursor_mgr = wlr_xcursor_manager_create(nullptr, 24);

    // wlr_cursor *only* displays an image on screen. It does not move
    // around when the pointer moves. However, we can attach input devices
    // to it, and it will generate aggregate events for all of them. In
    // these events, we can choose how we want to process them, forwarding
    // them to clients and moving the cursor around.
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
    // Reset the cursor mode to passthrough.
    server->cursor_mode = BiomeCursorMode::Passthrough;
    server->grabbed_toplevel = nullptr;
}

void begin_interactive(BiomeToplevel *toplevel, BiomeCursorMode mode, uint32_t edges,
        bool check_pointer_focus) {
    // This function sets up an interactive move or resize operation, where
    // the compositor stops propagating pointer events to clients and
    // instead consumes them itself, to move or resize windows.
    BiomeServer *server = toplevel->server;
    if (check_pointer_focus) {
        wlr_surface *focused_surface = server->seat->pointer_state.focused_surface;
        if (toplevel_surface(toplevel) != wlr_surface_get_root_surface(focused_surface)) {
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
    // Move the grabbed toplevel to the new position.
    BiomeToplevel *toplevel = server->grabbed_toplevel;
    if (toplevel->maximized) {
        // The titlebar press that started this grab left the maximized
        // state untouched (see handle_decoration_click) so that a plain
        // click-and-release doesn't unmaximize. Now that the pointer has
        // actually moved, restore the window under the cursor instead of
        // snapping it back to wherever it sat before being maximized:
        // capture what fraction of the maximized frame the cursor was
        // holding, then re-anchor the grab to that same fraction of the
        // just-restored frame, so the window appears to shrink under the
        // pointer - standard "drag titlebar to restore" WM convention.
        wlr_box old_geo;
        toplevel_get_geometry(toplevel, &old_geo);
        double old_frame_w =
            old_geo.width + decoration_border_width(toplevel, true) + decoration_border_right_width(toplevel, true);
        double old_frame_h =
            old_geo.height + decoration_titlebar_height(toplevel, true) + decoration_border_bottom_height(toplevel, true);
        double fraction_x = (server->cursor->x - toplevel->scene_tree->node.x) / old_frame_w;
        double fraction_y = (server->cursor->y - toplevel->scene_tree->node.y) / old_frame_h;

        // toplevel->restore_box is the pre-maximize size set_toplevel_maximized
        // is about to apply - use it rather than toplevel_get_geometry() right
        // after restoring: for xdg-shell toplevels the client applies its new
        // size asynchronously (see process_cursor_resize's comment on this),
        // so the surface geometry doesn't reflect the restored size yet by the
        // time set_toplevel_maximized() returns, which previously made the
        // "new" frame look identical to the old (maximized) one and put the
        // restored window back at its pre-maximize corner instead of under
        // the cursor.
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
        // set_toplevel_maximized() above just requested the restore but
        // (being xdg-shell) hasn't heard back yet - moving the frame now
        // would show its still-maximized-size buffer following the cursor
        // instead of the restored size. Keep tracking the latest cursor-
        // relative target without moving anything; xdg_toplevel_commit
        // applies it in one step once the matching buffer actually lands,
        // same flash this whole block exists to avoid.
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
    // Resizing the grabbed toplevel can be a little bit complicated, because
    // we could be resizing from any corner or edge. This not only resizes
    // the toplevel on one or two axes, but can also move the toplevel if you
    // resize from the top or left edges (or top-left corner).
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
        // Xwayland surfaces own their absolute position - X11 has no
        // equivalent to xdg-shell's async commit to defer to (see
        // xdg_toplevel_commit for that case), so position and size are
        // still sent together immediately, same as before.
        wlr_scene_node_set_position(&toplevel->scene_tree->node,
            new_left - geo_box.x - decoration_border_width(toplevel, toplevel->maximized),
            new_top - geo_box.y - decoration_titlebar_height(toplevel, toplevel->maximized));
    }
    // For xdg-shell, moving the window now - ahead of the client's own
    // matching commit - would show the *old*, not-yet-resized buffer at the
    // *new* position (the client applies this size on its own schedule via
    // a normal wl_surface.commit, not synchronously with this request).
    // That's a visible wobble on whichever edge is being dragged.
    // xdg_toplevel_commit repositions once the buffer that actually matches
    // this size lands, so position and content always change together;
    // only the size request goes out here.
    toplevel_set_size(toplevel, new_left - geo_box.x, new_top - geo_box.y, new_width, new_height);
    render_toplevel_decoration(toplevel);
}

static void process_cursor_motion(BiomeServer *server, uint32_t time) {
    // If the mode is non-passthrough, delegate to those functions.
    if (server->cursor_mode == BiomeCursorMode::Move) {
        process_cursor_move(server, time);
        return;
    } else if (server->cursor_mode == BiomeCursorMode::Resize) {
        process_cursor_resize(server, time);
        return;
    }

    // Otherwise, find the toplevel under the pointer and send the event along.
    double sx, sy;
    wlr_seat *seat = server->seat;
    wlr_surface *surface = nullptr;
    BiomeToplevel *toplevel = desktop_toplevel_at(server,
        server->cursor->x, server->cursor->y, &surface, &sx, &sy);
    if (!toplevel) {
        // No client surface under the cursor - either nothing at all, or
        // our own decoration (border/titlebar/buttons). Either way, this is
        // what makes the cursor image appear/update as it moves around,
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
        // Send pointer enter and motion events.
        //
        // The enter event gives the surface "pointer focus", which is
        // distinct from keyboard focus. You get pointer focus by moving the
        // pointer over a window.
        //
        // Note that wlroots will avoid sending duplicate enter/motion
        // events if the surface already has pointer focus or if the client
        // is already aware of the coordinates passed.
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, time, sx, sy);
    } else {
        // Clear pointer focus so future button events and such are not sent
        // to the last client to have the cursor over it.
        wlr_seat_pointer_clear_focus(seat);
    }
}

void server_cursor_motion(wl_listener *listener, void *data) {
    // This event is forwarded by the cursor when a pointer emits a
    // _relative_ pointer motion event (i.e. a delta)
    BiomeServer *server = wl_container_of(listener, server, cursor_motion);
    auto *event = static_cast<wlr_pointer_motion_event *>(data);
    // The cursor doesn't move unless we tell it to. The cursor
    // automatically handles constraining the motion to the output layout,
    // as well as any special configuration applied for the specific input
    // device which generated the event. You can pass NULL for the device if
    // you want to move the cursor around without any input.
    wlr_cursor_move(server->cursor, &event->pointer->base,
        event->delta_x, event->delta_y);
    process_cursor_motion(server, event->time_msec);
}

void server_cursor_motion_absolute(wl_listener *listener, void *data) {
    // This event is forwarded by the cursor when a pointer emits an
    // _absolute_ motion event, from 0..1 on each axis. This happens, for
    // example, when wlroots is running under a Wayland window rather than
    // KMS+DRM, and you move the mouse over the window. You could enter the
    // window from any edge, so we have to warp the mouse there. There is
    // also some hardware which emits these events.
    BiomeServer *server = wl_container_of(listener, server, cursor_motion_absolute);
    auto *event = static_cast<wlr_pointer_motion_absolute_event *>(data);
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    process_cursor_motion(server, event->time_msec);
}

void server_cursor_button(wl_listener *listener, void *data) {
    // This event is forwarded by the cursor when a pointer emits a button event.
    BiomeServer *server = wl_container_of(listener, server, cursor_button);
    auto *event = static_cast<wlr_pointer_button_event *>(data);
    // Notify the client with pointer focus that a button press has occurred
    wlr_seat_pointer_notify_button(server->seat,
        event->time_msec, event->button, event->state);

    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        set_decoration_pressed(server, nullptr, biome_decoration::Region::None);
        // If you released any buttons, we exit interactive move/resize mode.
        reset_cursor_mode(server);
        return;
    }

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
                handle_decoration_click(decoration_toplevel, region);
            }
        }
        return;
    }

    double sx, sy;
    wlr_surface *surface = nullptr;
    BiomeToplevel *toplevel = desktop_toplevel_at(server,
        server->cursor->x, server->cursor->y, &surface, &sx, &sy);
    // Focus that client if the button was _pressed_
    focus_toplevel(toplevel, surface);
}

void server_cursor_axis(wl_listener *listener, void *data) {
    // This event is forwarded by the cursor when a pointer emits an axis
    // event, for example when you move the scroll wheel.
    BiomeServer *server = wl_container_of(listener, server, cursor_axis);
    auto *event = static_cast<wlr_pointer_axis_event *>(data);
    // Notify the client with pointer focus of the axis event.
    wlr_seat_pointer_notify_axis(server->seat,
        event->time_msec, event->orientation, event->delta,
        event->delta_discrete, event->source, event->relative_direction);
}

void server_cursor_frame(wl_listener *listener, void *data) {
    (void)data;
    // This event is forwarded by the cursor when a pointer emits an frame
    // event. Frame events are sent after regular pointer events to group
    // multiple events together. For instance, two axis events may happen at
    // the same time, in which case a frame event won't be sent in between.
    BiomeServer *server = wl_container_of(listener, server, cursor_frame);
    // Notify the client with pointer focus of the frame event.
    wlr_seat_pointer_notify_frame(server->seat);
}
