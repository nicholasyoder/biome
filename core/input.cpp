// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/input.h"

#include "core/cursor.h"
#include "core/keybindings.h"
#include "desktop/decoration_bridge.h"
#include "desktop/toplevel.h"

#include <xkbcommon/xkbcommon.h>

#include <algorithm>

static void server_new_input(wl_listener *listener, void *data);
static void seat_request_cursor(wl_listener *listener, void *data);
static void seat_request_set_selection(wl_listener *listener, void *data);
static void seat_request_set_primary_selection(wl_listener *listener, void *data);
static void seat_request_start_drag(wl_listener *listener, void *data);
static void seat_start_drag(wl_listener *listener, void *data);

void input_init(BiomeServer *server) {
    wl_list_init(&server->keyboards);
    server->new_input.notify = server_new_input;
    wl_signal_add(&server->backend->events.new_input, &server->new_input);
    server->seat = wlr_seat_create(server->display, "seat0");
    server->request_cursor.notify = seat_request_cursor;
    wl_signal_add(&server->seat->events.request_set_cursor, &server->request_cursor);
    server->request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&server->seat->events.request_set_selection, &server->request_set_selection);
    server->request_set_primary_selection.notify = seat_request_set_primary_selection;
    wl_signal_add(&server->seat->events.request_set_primary_selection, &server->request_set_primary_selection);
    server->request_start_drag.notify = seat_request_start_drag;
    wl_signal_add(&server->seat->events.request_start_drag, &server->request_start_drag);
    server->start_drag.notify = seat_start_drag;
    wl_signal_add(&server->seat->events.start_drag, &server->start_drag);
}

static void keyboard_handle_modifiers(wl_listener *listener, void *data) {
    (void)data;
    BiomeKeyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
    // A seat can only have one active keyboard - a Wayland protocol
    // limitation, not wlroots - so every connected keyboard is assigned to
    // the same seat, swapping out the underlying wlr_keyboard transparently.
    wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr);
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
        &keyboard->wlr->modifiers);

    // Alt released while the switcher was up (started by a Tab press in
    // handle_keybinding) - dismiss it.
    BiomeServer *server = keyboard->server;
    if (server->switcher_active && !(wlr_keyboard_get_modifiers(keyboard->wlr) & WLR_MODIFIER_ALT)) {
        // In switch-on-release mode the focus change was only ever
        // previewed - commit it now, the same target handle_keybinding's
        // Tab case would have focused immediately in live mode.
        if (kSwitcherSwitchOnRelease && !server->switcher_order.empty()) {
            BiomeToplevel *target = server->switcher_order[static_cast<size_t>(server->switcher_preview_index)];
            if (target->minimized) {
                set_toplevel_minimized(target, false);
            }
            focus_toplevel(target, toplevel_surface(target));
        }
        server->switcher_active = false;
        server->switcher_order.clear();
        update_switcher_overlay(server);
    }
}

static void keyboard_handle_key(wl_listener *listener, void *data) {
    BiomeKeyboard *keyboard = wl_container_of(listener, keyboard, key);
    BiomeServer *server = keyboard->server;
    auto *event = static_cast<wlr_keyboard_key_event *>(data);
    wlr_seat *seat = server->seat;

    uint32_t keycode = event->keycode + 8; // libinput keycode -> xkbcommon
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(
        keyboard->wlr->xkb_state, keycode, &syms);

    // Unlike the old Alt-only-gated handle_keybinding(), handle_key_press()
    // runs on every press regardless of which modifiers are held - portal-
    // registered shortcuts (core/keybindings.h) aren't restricted to
    // Alt-chords the way Biome's own built-in bindings are.
    bool handled = false;
    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr);
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms; i++) {
            handled = handle_key_press(server, syms[i], modifiers);
        }
    }

    if (!handled) {
        wlr_seat_set_keyboard(seat, keyboard->wlr);
        wlr_seat_keyboard_notify_key(seat, event->time_msec,
            event->keycode, event->state);
    }
}

static void keyboard_handle_destroy(wl_listener *listener, void *data) {
    (void)data;
    BiomeKeyboard *keyboard = wl_container_of(listener, keyboard, destroy);
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->link);
    free(keyboard);
}

static void server_new_keyboard(BiomeServer *server, wlr_input_device *device) {
    wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

    auto *keyboard = static_cast<BiomeKeyboard *>(calloc(1, sizeof(BiomeKeyboard)));
    keyboard->server = server;
    keyboard->wlr = wlr_keyboard;

    // Default XKB keymap (e.g. layout = "us").
    xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap *keymap = xkb_keymap_new_from_names(context, nullptr,
        XKB_KEYMAP_COMPILE_NO_FLAGS);

    wlr_keyboard_set_keymap(wlr_keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

    keyboard->modifiers.notify = keyboard_handle_modifiers;
    wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
    keyboard->key.notify = keyboard_handle_key;
    wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
    keyboard->destroy.notify = keyboard_handle_destroy;
    wl_signal_add(&device->events.destroy, &keyboard->destroy);

    wlr_seat_set_keyboard(server->seat, keyboard->wlr);
    wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(BiomeServer *server, wlr_input_device *device) {
    wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_input(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, new_input);
    auto *device = static_cast<wlr_input_device *>(data);
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        server_new_keyboard(server, device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        server_new_pointer(server, device);
        break;
    default:
        break;
    }
    // Biome always has a cursor, even with no pointer devices, so the
    // pointer capability is always advertised.
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(server->seat, caps);
}

static void seat_request_cursor(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, request_cursor);
    auto *event = static_cast<wlr_seat_pointer_request_set_cursor_event *>(data);
    wlr_seat_client *focused_client = server->seat->pointer_state.focused_client;
    // Any client can send this, so only honor it from the one that actually
    // has pointer focus.
    if (focused_client == event->seat_client) {
        wlr_cursor_set_surface(server->cursor, event->surface,
            event->hotspot_x, event->hotspot_y);
    }
}

static void seat_request_set_selection(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, request_set_selection);
    auto *event = static_cast<wlr_seat_request_set_selection_event *>(data);
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void seat_request_set_primary_selection(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, request_set_primary_selection);
    auto *event = static_cast<wlr_seat_request_set_primary_selection_event *>(data);
    wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

static void seat_request_start_drag(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, request_start_drag);
    auto *event = static_cast<wlr_seat_request_start_drag_event *>(data);
    if (wlr_seat_validate_pointer_grab_serial(server->seat, event->origin, event->serial)) {
        wlr_seat_start_pointer_drag(server->seat, event->drag, event->serial);
    }
    // An invalid serial (spoofed/stale client request) is simply dropped -
    // wlroots has no public wlr_drag_destroy() to release the rejected
    // wlr_drag, a known upstream API gap, not something to work around here.
}

static void seat_start_drag(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, start_drag);
    auto *drag = static_cast<wlr_drag *>(data);
    if (drag->icon != nullptr) {
        drag_icon_create(server, drag->icon);
    }
}

void remove_toplevel_from_switcher(BiomeServer *server, BiomeToplevel *toplevel) {
    auto it = std::find(server->switcher_order.begin(), server->switcher_order.end(), toplevel);
    if (it == server->switcher_order.end()) {
        return;
    }
    auto erased_index = it - server->switcher_order.begin();
    server->switcher_order.erase(it);

    if (server->switcher_order.empty()) {
        // Nothing left to cycle through or commit to on Alt-release.
        server->switcher_active = false;
        server->switcher_preview_index = 0;
    } else {
        if (erased_index < server->switcher_preview_index) {
            server->switcher_preview_index--;
        }
        int count = static_cast<int>(server->switcher_order.size());
        server->switcher_preview_index = ((server->switcher_preview_index % count) + count) % count;
    }
    update_switcher_overlay(server);
}
