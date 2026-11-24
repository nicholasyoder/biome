// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/touch.h"

#include "core/cursor.h"
#include "core/input.h"
#include "core/touch_map.h"
#include "core/touch_store.h"
#include "desktop/idle.h"
#include "desktop/toplevel.h"
#include "desktop/touch_identify.h"
#include "desktop/window_menu.h"

#include <algorithm>
#include <string>
#include <type_traits>

// new'd, not calloc'd: holds strings.
struct BiomeTouchDevice {
    wl_list link = {};
    BiomeServer *server = nullptr;
    wlr_touch *wlr = nullptr;
    wl_listener destroy = {};
    wlr_output *mapped = nullptr; // null: the whole layout
    std::string mapped_to; // for logging changes
    TouchRemembered remembered;
    bool unresolved = false; // no rule placed it among several outputs
    bool dismissed = false; // prompt skipped; don't ask again this session
};
static_assert(std::is_standard_layout_v<BiomeTouchDevice>);

static void touch_handle_down(wl_listener *listener, void *data);
static void touch_handle_up(wl_listener *listener, void *data);
static void touch_handle_motion(wl_listener *listener, void *data);
static void touch_handle_cancel(wl_listener *listener, void *data);
static void touch_handle_frame(wl_listener *listener, void *data);
static void touch_handle_layout_change(wl_listener *listener, void *data);
static int identify_timer_fired(void *data);

void touch_init(BiomeServer *server) {
    BiomeTouch &t = server->touch;
    t.server = server;
    wl_list_init(&t.devices);
    auto add = [](wl_listener *l, wl_signal *sig, wl_notify_func_t fn) {
        l->notify = fn;
        wl_signal_add(sig, l);
    };
    auto &ev = server->cursor->events;
    add(&t.down, &ev.touch_down, touch_handle_down);
    add(&t.up, &ev.touch_up, touch_handle_up);
    add(&t.motion, &ev.touch_motion, touch_handle_motion);
    add(&t.cancel, &ev.touch_cancel, touch_handle_cancel);
    add(&t.frame, &ev.touch_frame, touch_handle_frame);
    add(&t.layout_change, &server->output_layout->events.change, touch_handle_layout_change);
    t.identify_timer = wl_event_loop_add_timer(wl_display_get_event_loop(server->display), identify_timer_fired, server);
}

void touch_finish(BiomeServer *server) {
    BiomeTouch &t = server->touch;
    wl_list_remove(&t.down.link);
    wl_list_remove(&t.up.link);
    wl_list_remove(&t.motion.link);
    wl_list_remove(&t.cancel.link);
    wl_list_remove(&t.frame.link);
    wl_list_remove(&t.layout_change.link);
    wl_event_source_remove(t.identify_timer);
    t.identify_trees.clear(); // already freed with the scene
}

static void layout_candidates(BiomeServer *server, std::vector<TouchOutputCandidate> *candidates,
        std::vector<wlr_output *> *outputs) {
    BiomeOutput *output;
    wl_list_for_each(output, &server->outputs, link) {
        wlr_output *o = output->wlr;
        if (!output->disabled && wlr_output_layout_get(server->output_layout, o) != nullptr) {
            candidates->push_back({o->name, o->phys_width, o->phys_height, o->make ? o->make : "",
                o->model ? o->model : "", o->serial ? o->serial : "", o->transform});
            outputs->push_back(o);
        }
    }
}

static void identify_close(BiomeServer *server) {
    touch_identify_hide(server);
    server->touch.identifying = nullptr;
    server->touch.identify_outputs.clear();
}

// wlroots keeps a stale mapping when its output goes, so this reruns on every layout change.
static void touch_map_devices(BiomeServer *server) {
    std::vector<TouchOutputCandidate> candidates;
    std::vector<wlr_output *> outputs;
    layout_candidates(server, &candidates, &outputs);
    bool ask = false;
    BiomeTouchDevice *device;
    wl_list_for_each(device, &server->touch.devices, link) {
        int i = touch_pick_output(candidates, device->wlr->output_name, device->wlr->width_mm,
            device->wlr->height_mm, device->remembered);
        wlr_output *target = i >= 0 ? outputs[i] : nullptr;
        wlr_cursor_map_input_to_output(server->cursor, &device->wlr->base, target);
        device->mapped = target;
        device->unresolved = target == nullptr && candidates.size() >= 2;
        ask |= device->unresolved && !device->dismissed;
        const std::string name = target != nullptr ? target->name : "";
        if (name != device->mapped_to) {
            device->mapped_to = name;
            wlr_log(WLR_INFO, "Touch device '%s' (%.0fx%.0f mm) mapped to %s", device->wlr->base.name,
                device->wlr->width_mm, device->wlr->height_mm, target != nullptr ? name.c_str() : "the whole layout");
        }
    }
    // The outputs offered changed (or the answer came some other way): re-ask after the layout settles.
    identify_close(server);
    if (ask) {
        wl_event_source_timer_update(server->touch.identify_timer, 700);
    }
}

static int identify_timer_fired(void *data) {
    auto *server = static_cast<BiomeServer *>(data);
    BiomeTouchDevice *device, *pick = nullptr;
    wl_list_for_each(device, &server->touch.devices, link) {
        if (device->unresolved && !device->dismissed) {
            pick = device;
        }
    }
    if (pick == nullptr) {
        return 0;
    }
    std::vector<TouchOutputCandidate> candidates;
    layout_candidates(server, &candidates, &server->touch.identify_outputs);
    server->touch.identifying = pick;
    wlr_log(WLR_INFO, "Touch device '%s': output unclear, asking", pick->wlr->base.name);
    touch_identify_show(server, server->touch.identify_outputs, pick->wlr->base.name);
    return 0;
}

// The prompt's tap: true if consumed (always, for the device being asked about).
static bool identify_tap(BiomeServer *server, wlr_touch_down_event *event) {
    BiomeTouchDevice *device = server->touch.identifying;
    if (device == nullptr || event->touch != device->wlr || server->session_locked) {
        return false;
    }
    server->touch.swallowed.push_back(event->touch_id);
    std::vector<TouchOutputCandidate> candidates;
    std::vector<wlr_output *> outputs;
    layout_candidates(server, &candidates, &outputs);
    if (outputs != server->touch.identify_outputs) {
        return true; // a relayout is about to re-ask
    }
    int i = touch_identify_hit(candidates, event->x, event->y);
    if (i < 0) {
        return true;
    }
    const TouchOutputCandidate &c = candidates[i];
    device->remembered = {c.name, c.make, c.model, c.serial};
    touch_store_save(device->wlr->base.name, device->remembered);
    wlr_log(WLR_INFO, "Touch device '%s' identified as %s", device->wlr->base.name, c.name.c_str());
    touch_map_devices(server);
    return true;
}

bool touch_identify_dismiss(BiomeServer *server) {
    BiomeTouchDevice *device = server->touch.identifying;
    if (device == nullptr || server->session_locked) {
        return false;
    }
    device->dismissed = true;
    wlr_log(WLR_INFO, "Touch device '%s': identify skipped, using the whole layout", device->wlr->base.name);
    touch_map_devices(server); // asks about the next one, if any
    return true;
}

static void touch_handle_layout_change(wl_listener *listener, void *data) {
    (void)data;
    touch_map_devices(biome_server_from(listener, BiomeTouch, layout_change));
}

static void touch_device_destroy(wl_listener *listener, void *data) {
    (void)data;
    BiomeTouchDevice *device = wl_container_of(listener, device, destroy);
    BiomeServer *server = device->server;
    wl_list_remove(&device->destroy.link);
    wl_list_remove(&device->link);
    if (server->touch.identifying == device) {
        identify_close(server);
    }
    delete device;
    input_update_seat_capabilities(server);
}

void touch_new_device(BiomeServer *server, wlr_input_device *input) {
    auto *device = new BiomeTouchDevice();
    device->server = server;
    device->wlr = wlr_touch_from_input_device(input);
    device->wlr->data = device;
    device->remembered = touch_store_load(input->name);
    device->destroy.notify = touch_device_destroy;
    wl_signal_add(&input->events.destroy, &device->destroy);
    wl_list_insert(&server->touch.devices, &device->link);
    wlr_cursor_attach_input_device(server->cursor, input);
    touch_map_devices(server);
}

bool touch_has_devices(BiomeServer *server) {
    return !wl_list_empty(&server->touch.devices);
}

static BiomeTouchPoint *find_point(BiomeServer *server, int32_t id) {
    for (BiomeTouchPoint &point : server->touch.points) {
        if (point.id == id) {
            return &point;
        }
    }
    return nullptr;
}

static void forget_point(BiomeServer *server, int32_t id) {
    auto &points = server->touch.points;
    points.erase(std::remove_if(points.begin(), points.end(),
        [id](const BiomeTouchPoint &p) { return p.id == id; }), points.end());
}

int32_t touch_point_on(BiomeServer *server, wlr_surface *root) {
    if (root == nullptr) {
        return -1;
    }
    auto &points = server->touch.points;
    for (auto it = points.rbegin(); it != points.rend(); ++it) {
        wlr_touch_point *point = wlr_seat_touch_get_point(server->seat, it->id);
        if (point != nullptr && point->surface != nullptr && wlr_surface_get_root_surface(point->surface) == root) {
            return it->id;
        }
    }
    return -1;
}

void touch_begin_grab(BiomeServer *server, int32_t id) {
    BiomeTouchPoint *point = find_point(server, id);
    if (point == nullptr) {
        return;
    }
    server->touch.grab_id = id;
    wlr_cursor_warp_closest(server->cursor, nullptr, point->lx, point->ly);
}

bool touch_point_position(BiomeServer *server, int32_t id, double *lx, double *ly) {
    BiomeTouchPoint *point = find_point(server, id);
    if (point == nullptr) {
        return false;
    }
    *lx = point->lx;
    *ly = point->ly;
    return true;
}

// Touch coordinates are panel-native; wlr_cursor doesn't undo the output's rotation.
static void to_layout(BiomeServer *server, wlr_touch *touch, double x, double y, double *lx, double *ly) {
    auto *device = static_cast<BiomeTouchDevice *>(touch->data);
    if (device->mapped != nullptr && device->mapped->transform != WL_OUTPUT_TRANSFORM_NORMAL) {
        touch_native_to_output(device->mapped->transform, &x, &y);
    }
    wlr_cursor_absolute_to_layout_coords(server->cursor, &touch->base, x, y, lx, ly);
}

// Drops the point from swallowed; true if it was there.
static bool unswallow(BiomeServer *server, int32_t id, bool remove) {
    auto &ids = server->touch.swallowed;
    auto it = std::find(ids.begin(), ids.end(), id);
    if (it == ids.end()) {
        return false;
    }
    if (remove) {
        ids.erase(it);
    }
    return true;
}

static void end_grab(BiomeServer *server) {
    server->touch.grab_id = -1;
    if (server->grab.mode != BiomeCursorMode::Passthrough) {
        snap_drag_finish(server);
        reset_cursor_mode(server);
    }
}

static void touch_handle_down(wl_listener *listener, void *data) {
    BiomeServer *server = biome_server_from(listener, BiomeTouch, down);
    auto *event = static_cast<wlr_touch_down_event *>(data);
    idle_notify_activity(server);
    if (identify_tap(server, event)) {
        return;
    }
    double lx, ly;
    to_layout(server, event->touch, event->x, event->y, &lx, &ly);

    double sx, sy;
    wlr_surface *surface = nullptr;
    BiomeToplevel *toplevel = desktop_toplevel_at_node(scene_node_at(server, lx, ly, &sx, &sy), &surface);
    // Compositor UI and grabs work in pointer terms, so they get emulation.
    const bool compositor_owned = window_menu_active(server) || server->grab.from_menu ||
        server->grab.mode != BiomeCursorMode::Passthrough;
    if (surface != nullptr && !compositor_owned && wlr_surface_accepts_touch(surface, server->seat)) {
        if (wlr_seat_touch_notify_down(server->seat, surface, event->time_msec, event->touch_id, sx, sy) != 0) {
            server->touch.points.push_back({event->touch_id, lx, ly, lx - sx, ly - sy});
        }
        focus_for_press(server, toplevel, surface);
        return;
    }
    if (server->touch.pointer_id >= 0) {
        return; // one point drives the pointer at a time
    }
    server->touch.pointer_id = event->touch_id;
    server->touch.pointer_frame = true;
    cursor_warp(server, lx, ly, event->time_msec);
    cursor_emulate_button(server, event->time_msec, true);
}

static void touch_handle_motion(wl_listener *listener, void *data) {
    BiomeServer *server = biome_server_from(listener, BiomeTouch, motion);
    auto *event = static_cast<wlr_touch_motion_event *>(data);
    idle_notify_activity(server);
    if (unswallow(server, event->touch_id, false)) {
        return;
    }
    double lx, ly;
    to_layout(server, event->touch, event->x, event->y, &lx, &ly);

    if (event->touch_id == server->touch.pointer_id) {
        server->touch.pointer_frame = true;
        cursor_warp(server, lx, ly, event->time_msec);
        return;
    }
    BiomeTouchPoint *point = find_point(server, event->touch_id);
    if (point == nullptr) {
        return;
    }
    point->lx = lx;
    point->ly = ly;

    if (event->touch_id == server->touch.grab_id) {
        if (server->grab.mode != BiomeCursorMode::Passthrough) {
            cursor_warp(server, lx, ly, event->time_msec);
            return;
        }
        server->touch.grab_id = -1; // grab ended elsewhere (e.g. the window closed)
    }

    wlr_seat *seat = server->seat;
    wlr_drag *drag = seat->drag;
    if (drag != nullptr && drag->grab_type == WLR_DRAG_GRAB_KEYBOARD_TOUCH && drag->grab_touch_id == event->touch_id) {
        // wlr_drag's touch grab takes its drop target from the point's focus.
        if (server->pointer.drag_icon != nullptr) {
            wlr_scene_node_set_position(&server->pointer.drag_icon->node, lx, ly);
        }
        double sx, sy;
        wlr_surface *surface = nullptr;
        desktop_toplevel_at_node(scene_node_at(server, lx, ly, &sx, &sy), &surface);
        if (surface != nullptr) {
            wlr_seat_touch_point_focus(seat, surface, event->time_msec, event->touch_id, sx, sy);
            wlr_seat_touch_notify_motion(seat, event->time_msec, event->touch_id, sx, sy);
        } else {
            wlr_seat_touch_point_clear_focus(seat, event->time_msec, event->touch_id);
        }
        return;
    }
    wlr_seat_touch_notify_motion(seat, event->time_msec, event->touch_id, lx - point->origin_x, ly - point->origin_y);
}

static void touch_handle_up(wl_listener *listener, void *data) {
    BiomeServer *server = biome_server_from(listener, BiomeTouch, up);
    auto *event = static_cast<wlr_touch_up_event *>(data);
    idle_notify_activity(server);
    if (unswallow(server, event->touch_id, true)) {
        return;
    }
    if (event->touch_id == server->touch.pointer_id) {
        server->touch.pointer_id = -1;
        server->touch.pointer_frame = true;
        cursor_emulate_button(server, event->time_msec, false);
        return;
    }
    if (event->touch_id == server->touch.grab_id) {
        end_grab(server);
    }
    forget_point(server, event->touch_id);
    wlr_seat_touch_notify_up(server->seat, event->time_msec, event->touch_id);
}

static void touch_handle_cancel(wl_listener *listener, void *data) {
    BiomeServer *server = biome_server_from(listener, BiomeTouch, cancel);
    auto *event = static_cast<wlr_touch_cancel_event *>(data);
    if (unswallow(server, event->touch_id, true)) {
        return;
    }
    if (event->touch_id == server->touch.pointer_id) {
        server->touch.pointer_id = -1;
        server->touch.pointer_frame = true;
        cursor_emulate_button(server, event->time_msec, false);
        return;
    }
    if (event->touch_id == server->touch.grab_id) {
        end_grab(server);
    }
    wlr_touch_point *point = wlr_seat_touch_get_point(server->seat, event->touch_id);
    if (point != nullptr && point->client != nullptr) {
        // Cancels every point of that client.
        wlr_seat_touch_notify_cancel(server->seat, point->client);
    }
    auto &points = server->touch.points;
    points.erase(std::remove_if(points.begin(), points.end(), [server](const BiomeTouchPoint &p) {
        return wlr_seat_touch_get_point(server->seat, p.id) == nullptr;
    }), points.end());
}

static void touch_handle_frame(wl_listener *listener, void *data) {
    (void)data;
    BiomeServer *server = biome_server_from(listener, BiomeTouch, frame);
    if (server->touch.pointer_frame) {
        server->touch.pointer_frame = false;
        wlr_seat_pointer_notify_frame(server->seat);
    }
    wlr_seat_touch_notify_frame(server->seat);
}
