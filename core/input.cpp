// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/input.h"

#include "desktop/decoration_bridge.h"
#include "desktop/toplevel.h"
#include "desktop/workspace.h"

#include <xkbcommon/xkbcommon.h>

static void server_new_input(wl_listener *listener, void *data);
static void seat_request_cursor(wl_listener *listener, void *data);
static void seat_request_set_selection(wl_listener *listener, void *data);

void input_init(BiomeServer *server) {
    wl_list_init(&server->keyboards);
    server->new_input.notify = server_new_input;
    wl_signal_add(&server->backend->events.new_input, &server->new_input);
    server->seat = wlr_seat_create(server->display, "seat0");
    server->request_cursor.notify = seat_request_cursor;
    wl_signal_add(&server->seat->events.request_set_cursor, &server->request_cursor);
    server->request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&server->seat->events.request_set_selection, &server->request_set_selection);
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

// Compositor keybindings, handled here instead of passed to the client.
// Assumes Alt is held down.
static bool handle_keybinding(BiomeServer *server, xkb_keysym_t sym, uint32_t modifiers) {
    bool ctrl = modifiers & WLR_MODIFIER_CTRL;
    bool shift = modifiers & WLR_MODIFIER_SHIFT;

    // Ctrl-Alt-F1..F12: hand VT switching back to the session/kernel. Taking
    // over a KMS/DRM session puts the console in graphics mode, so the
    // compositor has to notice these and call wlr_session_change_vt() itself.
    // No-op (but still swallowed) on nested backends, which have no session.
    if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
        if (server->session != nullptr) {
            unsigned vt = static_cast<unsigned>(sym - XKB_KEY_XF86Switch_VT_1 + 1);
            wlr_session_change_vt(server->session, vt);
        }
        return true;
    }

    switch (sym) {
    case XKB_KEY_Escape:
        wl_display_terminate(server->display);
        return true;
    case XKB_KEY_Tab:
    case XKB_KEY_ISO_Left_Tab: {
        // Alt-Tab / Alt-Shift-Tab: cycle through windows in MRU order, no
        // live preview (matches xfwm4's cycle_preview=false default).
        if (wl_list_length(&server->toplevels) < 2) {
            return true;
        }
        bool reverse = shift || sym == XKB_KEY_ISO_Left_Tab;

        // The switcher always shows a static snapshot of MRU order taken at
        // the start of this Alt-hold, in both modes - server->toplevels
        // itself isn't re-read again until the hold ends, so the on-screen
        // list doesn't reshuffle underfoot as you cycle (in live mode,
        // committing a target below does reorder the live list, but nothing
        // reads it again until the next fresh hold). Only
        // switcher_preview_index moves, wrapping over the snapshot.
        if (!server->switcher_active) {
            server->switcher_order.clear();
            BiomeToplevel *pos;
            wl_list_for_each(pos, &server->toplevels, link) {
                server->switcher_order.push_back(pos);
            }
            server->switcher_preview_index = 0;
        }
        int count = static_cast<int>(server->switcher_order.size());
        server->switcher_preview_index =
            (server->switcher_preview_index + (reverse ? -1 : 1) + count) % count;

        if constexpr (!kSwitcherSwitchOnRelease) {
            BiomeToplevel *target =
                server->switcher_order[static_cast<size_t>(server->switcher_preview_index)];
            if (target->minimized) {
                // Cycling to a minimized window should bring it back, not just
                // hand it invisible keyboard focus.
                set_toplevel_minimized(target, false);
            }
            focus_toplevel(target, toplevel_surface(target));
        }
        server->switcher_active = true;
        update_switcher_overlay(server);
        return true;
    }
    case XKB_KEY_Left:
    case XKB_KEY_Right: {
        // Ctrl-Alt-Left/Right: switch workspace. Add Shift to bring the
        // focused window along.
        if (!ctrl) {
            return false;
        }
        int dir = (sym == XKB_KEY_Right) ? 1 : -1;
        int target = server->active_workspace + dir;
        if (shift && !wl_list_empty(&server->toplevels)) {
            BiomeToplevel *focused = wl_container_of(server->toplevels.next, focused, link);
            move_toplevel_to_workspace(focused, target);
        }
        switch_workspace(server, target);
        return true;
    }
    case XKB_KEY_1:
    case XKB_KEY_2:
    case XKB_KEY_3:
    case XKB_KEY_4: {
        // Ctrl-Alt-1..4: jump directly to a workspace.
        if (!ctrl) {
            return false;
        }
        switch_workspace(server, static_cast<int>(sym - XKB_KEY_1));
        return true;
    }
    default:
        return false;
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

    bool handled = false;
    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr);
    if ((modifiers & WLR_MODIFIER_ALT) &&
            event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms; i++) {
            handled = handle_keybinding(server, syms[i], modifiers);
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
