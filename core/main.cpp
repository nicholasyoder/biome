// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Biome Phase 2: xfwm4 feature parity, still built on tinywl's bones (see
// git history for the Phase 0 port and Phase 1's XWayland/placement work).
// This phase adds: Alt-Tab/Alt-Shift-Tab MRU window cycling, 4 workspaces
// (matching Forest's xfwm4 config) with switch/move-window-to hotkeys,
// transient-dialog placement (centers on parent instead of the output),
// and a flat-colored SSD focus border. The border needed every toplevel's
// scene_tree to become a container (border rects + a content_tree holding
// the actual surface, offset inside it) rather than the surface's own
// scene tree directly - see docs/plan.md's Phase 2 writeup for the details.

#include "wlroots.hpp"

#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <getopt.h>
#include <unistd.h>
#include <xcb/xproto.h>
#include <xkbcommon/xkbcommon.h>

enum class BiomeCursorMode {
    Passthrough,
    Move,
    Resize,
};

enum class BiomeToplevelType {
    Xdg,
    Xwayland,
};

// Phase 2: flat-colored SSD border (visual only, no interactive
// border-drag-resize, no xdg-decoration negotiation - CSD clients like foot
// will show both their own CSD and this border. Matches sway/river's simple
// default look, not a full decoration protocol implementation).
constexpr int kBorderWidth = 2;
constexpr float kBorderColorFocused[4] = {0.20f, 0.52f, 0.89f, 1.0f};
constexpr float kBorderColorUnfocused[4] = {0.35f, 0.35f, 0.35f, 1.0f};

// Matches workspace_count in forest/usr/share/forest/xfwm4.xml.
constexpr int kWorkspaceCount = 4;

struct BiomeServer {
    wl_display *display = nullptr;
    wlr_backend *backend = nullptr;
    // Only non-null on a real KMS/DRM session (nested Wayland/X11 backends
    // have none). Needed to hand VT-switching back to the kernel below.
    wlr_session *session = nullptr;
    wlr_renderer *renderer = nullptr;
    wlr_allocator *allocator = nullptr;
    wlr_scene *scene = nullptr;
    wlr_scene_output_layout *scene_layout = nullptr;

    wlr_xdg_shell *xdg_shell = nullptr;
    wl_listener new_xdg_toplevel = {};
    wl_listener new_xdg_popup = {};
    wl_list toplevels = {};

    wlr_xwayland *xwayland = nullptr;
    wl_listener new_xwayland_surface = {};
    wl_listener xwayland_ready = {};

    wlr_cursor *cursor = nullptr;
    wlr_xcursor_manager *cursor_mgr = nullptr;
    wl_listener cursor_motion = {};
    wl_listener cursor_motion_absolute = {};
    wl_listener cursor_button = {};
    wl_listener cursor_axis = {};
    wl_listener cursor_frame = {};

    wlr_seat *seat = nullptr;
    wl_listener new_input = {};
    wl_listener request_cursor = {};
    wl_listener request_set_selection = {};
    wl_list keyboards = {};
    BiomeCursorMode cursor_mode = BiomeCursorMode::Passthrough;
    struct BiomeToplevel *grabbed_toplevel = nullptr;
    double grab_x = 0, grab_y = 0;
    wlr_box grab_geobox = {};
    uint32_t resize_edges = 0;

    int active_workspace = 0;

    wlr_output_layout *output_layout = nullptr;
    wl_list outputs = {};
    wl_listener new_output = {};
};

struct BiomeOutput {
    wl_list link = {};
    BiomeServer *server = nullptr;
    wlr_output *wlr = nullptr;
    wl_listener frame = {};
    wl_listener request_state = {};
    wl_listener destroy = {};
};

struct BiomeToplevel {
    wl_list link = {};
    BiomeServer *server = nullptr;
    BiomeToplevelType type = BiomeToplevelType::Xdg;
    int workspace = 0;

    // scene_tree is the container: its position is the window's on-screen
    // position (what move/resize/focus-raise all act on). content_tree is
    // the actual surface tree, a child of scene_tree offset by kBorderWidth
    // so the border rects (also children of scene_tree) can frame it.
    wlr_scene_tree *scene_tree = nullptr;
    wlr_scene_tree *content_tree = nullptr;
    wlr_scene_rect *border[4] = {}; // top, bottom, left, right

    wlr_xdg_toplevel *xdg_toplevel = nullptr;         // type == Xdg
    wlr_xwayland_surface *xwayland_surface = nullptr; // type == Xwayland

    wl_listener map = {};
    wl_listener unmap = {};
    wl_listener commit = {}; // xdg only
    wl_listener destroy = {};
    wl_listener request_move = {};
    wl_listener request_resize = {};
    wl_listener request_maximize = {};
    wl_listener request_fullscreen = {};

    // Xwayland only: the underlying wlr_surface only exists between
    // associate/dissociate, so map/unmap are (dis)connected there instead
    // of at creation/destroy time like xdg-shell's are.
    wl_listener associate = {};
    wl_listener dissociate = {};
    wl_listener request_configure = {};
};

// An override-redirect Xwayland surface (menus, tooltips, dnd icons, ...).
// These position themselves and are never part of server->toplevels - no
// compositor-driven focus, move, or resize.
struct BiomeUnmanaged {
    BiomeServer *server = nullptr;
    wlr_xwayland_surface *xwayland_surface = nullptr;
    wlr_scene_tree *scene_tree = nullptr;

    wl_listener associate = {};
    wl_listener dissociate = {};
    wl_listener destroy = {};
    wl_listener map = {};
    wl_listener unmap = {};
    wl_listener request_configure = {};
};

struct BiomePopup {
    wlr_xdg_popup *xdg_popup = nullptr;
    wl_listener commit = {};
    wl_listener destroy = {};
};

struct BiomeKeyboard {
    wl_list link = {};
    BiomeServer *server = nullptr;
    wlr_keyboard *wlr = nullptr;

    wl_listener modifiers = {};
    wl_listener key = {};
    wl_listener destroy = {};
};

static wlr_surface *toplevel_surface(BiomeToplevel *toplevel) {
    return toplevel->type == BiomeToplevelType::Xdg
        ? toplevel->xdg_toplevel->base->surface
        : toplevel->xwayland_surface->surface;
}

static void toplevel_get_geometry(BiomeToplevel *toplevel, wlr_box *box) {
    if (toplevel->type == BiomeToplevelType::Xdg) {
        wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, box);
        return;
    }
    box->x = 0;
    box->y = 0;
    box->width = toplevel->xwayland_surface->width;
    box->height = toplevel->xwayland_surface->height;
}

// content_tree->node.data is set to the owning BiomeToplevel for both xdg
// and Xwayland (mirroring base->data / xsurface->data below), so these can
// recover a BiomeToplevel from a bare protocol object - used for looking up
// a parent (transient placement) or the previously-focused surface (border
// color) without needing a wlr_surface in hand.
static BiomeToplevel *toplevel_from_xdg(wlr_xdg_toplevel *xdg_toplevel) {
    if (xdg_toplevel == nullptr) {
        return nullptr;
    }
    auto *tree = static_cast<wlr_scene_tree *>(xdg_toplevel->base->data);
    return tree ? static_cast<BiomeToplevel *>(tree->node.data) : nullptr;
}

static BiomeToplevel *toplevel_from_xwayland(wlr_xwayland_surface *xsurface) {
    if (xsurface == nullptr) {
        return nullptr;
    }
    auto *tree = static_cast<wlr_scene_tree *>(xsurface->data);
    return tree ? static_cast<BiomeToplevel *>(tree->node.data) : nullptr;
}

// Resizes/repositions the 4 border rects to frame content_tree (which sits
// at a fixed (kBorderWidth, kBorderWidth) offset inside scene_tree). Called
// whenever a toplevel's content geometry changes.
static void update_toplevel_decoration(BiomeToplevel *toplevel) {
    wlr_box geo;
    toplevel_get_geometry(toplevel, &geo);
    int width = geo.width > 0 ? geo.width : 0;
    int height = geo.height > 0 ? geo.height : 0;

    wlr_scene_rect_set_size(toplevel->border[0], width + 2 * kBorderWidth, kBorderWidth);
    wlr_scene_node_set_position(&toplevel->border[0]->node, 0, 0);

    wlr_scene_rect_set_size(toplevel->border[1], width + 2 * kBorderWidth, kBorderWidth);
    wlr_scene_node_set_position(&toplevel->border[1]->node, 0, kBorderWidth + height);

    wlr_scene_rect_set_size(toplevel->border[2], kBorderWidth, height);
    wlr_scene_node_set_position(&toplevel->border[2]->node, 0, kBorderWidth);

    wlr_scene_rect_set_size(toplevel->border[3], kBorderWidth, height);
    wlr_scene_node_set_position(&toplevel->border[3]->node, kBorderWidth + width, kBorderWidth);
}

static void update_toplevel_border_color(BiomeToplevel *toplevel, bool focused) {
    if (toplevel == nullptr) {
        return;
    }
    const float *color = focused ? kBorderColorFocused : kBorderColorUnfocused;
    for (wlr_scene_rect *rect : toplevel->border) {
        wlr_scene_rect_set_color(rect, color);
    }
}

// Creates the 4 border rects as children of scene_tree (the container).
// Sized/positioned later by update_toplevel_decoration once geometry is
// known.
static void create_toplevel_border(BiomeToplevel *toplevel) {
    for (wlr_scene_rect *&rect : toplevel->border) {
        rect = wlr_scene_rect_create(toplevel->scene_tree, 0, 0, kBorderColorUnfocused);
    }
}

// Used during interactive resize: xdg-shell only needs the new size (the
// client acks asynchronously and the compositor owns position via the scene
// graph); Xwayland surfaces track their own absolute geometry, so x/y/width/
// height all have to be sent together.
static void toplevel_set_size(BiomeToplevel *toplevel, int x, int y, int width, int height) {
    if (toplevel->type == BiomeToplevelType::Xdg) {
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, width, height);
    } else {
        wlr_xwayland_surface_configure(toplevel->xwayland_surface,
            static_cast<int16_t>(x), static_cast<int16_t>(y),
            static_cast<uint16_t>(width), static_cast<uint16_t>(height));
    }
}

// Xwayland surfaces need to be told about every position change (X11 popups
// and menus position themselves relative to their parent's known x/y), so
// every move has to be mirrored into the X server. xdg-shell toplevels have
// no equivalent state - positioning them is purely a scene graph concern.
static void toplevel_sync_position(BiomeToplevel *toplevel, int x, int y) {
    if (toplevel->type == BiomeToplevelType::Xwayland) {
        wlr_xwayland_surface_configure(toplevel->xwayland_surface,
            static_cast<int16_t>(x), static_cast<int16_t>(y),
            toplevel->xwayland_surface->width, toplevel->xwayland_surface->height);
    }
}

static void focus_toplevel(BiomeToplevel *toplevel, wlr_surface *surface) {
    // Note: this function only deals with keyboard focus (and, for
    // Xwayland, the X11 stacking order that goes along with it).
    if (toplevel == nullptr) {
        return;
    }
    BiomeServer *server = toplevel->server;
    wlr_seat *seat = server->seat;
    wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    if (prev_surface == surface) {
        // Don't re-focus an already focused surface.
        return;
    }
    if (prev_surface) {
        // Deactivate the previously focused surface. This lets the client
        // know it no longer has focus and the client will repaint
        // accordingly, e.g. stop displaying a caret.
        wlr_xdg_toplevel *prev_xdg_toplevel =
            wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
        if (prev_xdg_toplevel != nullptr) {
            wlr_xdg_toplevel_set_activated(prev_xdg_toplevel, false);
            update_toplevel_border_color(toplevel_from_xdg(prev_xdg_toplevel), false);
        } else {
            wlr_xwayland_surface *prev_xwayland_surface =
                wlr_xwayland_surface_try_from_wlr_surface(prev_surface);
            if (prev_xwayland_surface != nullptr) {
                wlr_xwayland_surface_activate(prev_xwayland_surface, false);
                update_toplevel_border_color(toplevel_from_xwayland(prev_xwayland_surface), false);
            }
        }
    }
    wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    // Move the toplevel to the front
    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    wl_list_remove(&toplevel->link);
    wl_list_insert(&server->toplevels, &toplevel->link);
    // Activate the new surface
    if (toplevel->type == BiomeToplevelType::Xdg) {
        wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
    } else {
        wlr_xwayland_surface_activate(toplevel->xwayland_surface, true);
        // wlr_scene_node_raise_to_top only reorders our own render tree;
        // Xwayland windows also need their X11 stacking order raised, since
        // X11 clients (e.g. submenus) may position themselves relative to
        // sibling stacking order.
        wlr_xwayland_surface_restack(toplevel->xwayland_surface, nullptr, XCB_STACK_MODE_ABOVE);
    }
    update_toplevel_border_color(toplevel, true);
    // Tell the seat to have the keyboard enter this surface. wlroots will
    // keep track of this and automatically send key events to the
    // appropriate clients without additional work on your part.
    if (keyboard != nullptr) {
        wlr_seat_keyboard_notify_enter(seat, surface,
            keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    }
}

static void keyboard_handle_modifiers(wl_listener *listener, void *data) {
    (void)data;
    // This event is raised when a modifier key, such as shift or alt, is
    // pressed. We simply communicate this to the client.
    BiomeKeyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
    // A seat can only have one keyboard, but this is a limitation of the
    // Wayland protocol - not wlroots. We assign all connected keyboards to
    // the same seat. You can swap out the underlying wlr_keyboard like this
    // and wlr_seat handles this transparently.
    wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr);
    // Send modifiers to the client.
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
        &keyboard->wlr->modifiers);
}

// Hands focus to the topmost (most-recently-focused) toplevel that's on the
// active workspace, or clears keyboard focus if there isn't one.
static void focus_topmost_on_active_workspace(BiomeServer *server) {
    BiomeToplevel *pos;
    wl_list_for_each(pos, &server->toplevels, link) {
        if (pos->workspace == server->active_workspace) {
            focus_toplevel(pos, toplevel_surface(pos));
            return;
        }
    }
    wlr_seat_keyboard_notify_clear_focus(server->seat);
}

static int wrap_workspace(int index) {
    if (index < 0) {
        return kWorkspaceCount - 1;
    }
    if (index >= kWorkspaceCount) {
        return 0;
    }
    return index;
}

static void switch_workspace(BiomeServer *server, int index) {
    index = wrap_workspace(index);
    if (index == server->active_workspace) {
        return;
    }
    server->active_workspace = index;

    BiomeToplevel *pos;
    wl_list_for_each(pos, &server->toplevels, link) {
        wlr_scene_node_set_enabled(&pos->scene_tree->node, pos->workspace == index);
    }
    // The pointer may be sitting over a surface that just got hidden;
    // clear its focus so stale events don't reach it. It'll be re-resolved
    // on the next motion event.
    wlr_seat_pointer_clear_focus(server->seat);
    focus_topmost_on_active_workspace(server);
}

static void move_toplevel_to_workspace(BiomeToplevel *toplevel, int index) {
    BiomeServer *server = toplevel->server;
    index = wrap_workspace(index);
    if (index == toplevel->workspace) {
        return;
    }
    toplevel->workspace = index;
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, index == server->active_workspace);
    if (server->seat->keyboard_state.focused_surface == toplevel_surface(toplevel) &&
            index != server->active_workspace) {
        wlr_seat_pointer_clear_focus(server->seat);
        focus_topmost_on_active_workspace(server);
    }
}

static bool handle_keybinding(BiomeServer *server, xkb_keysym_t sym, uint32_t modifiers) {
    // Here we handle compositor keybindings. This is when the compositor is
    // processing keys, rather than passing them on to the client for its
    // own processing.
    //
    // This function assumes Alt is held down.
    bool ctrl = modifiers & WLR_MODIFIER_CTRL;
    bool shift = modifiers & WLR_MODIFIER_SHIFT;

    // Ctrl-Alt-F1..F12: hand VT switching back to the session/kernel. Taking
    // over a KMS/DRM session puts the console in graphics mode, which stops
    // the kernel from handling these itself - the compositor has to notice
    // them and call wlr_session_change_vt(), same as every other wlroots
    // compositor. No-op (but still swallowed) on nested backends, which have
    // no session and nothing to switch away from.
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
        BiomeToplevel *target;
        if (reverse) {
            // "Previously focused window" - the second entry in MRU order.
            target = wl_container_of(server->toplevels.next->next, target, link);
        } else {
            // Rotate the least-recently-used window to the front.
            target = wl_container_of(server->toplevels.prev, target, link);
        }
        focus_toplevel(target, toplevel_surface(target));
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
    // This event is raised when a key is pressed or released.
    BiomeKeyboard *keyboard = wl_container_of(listener, keyboard, key);
    BiomeServer *server = keyboard->server;
    auto *event = static_cast<wlr_keyboard_key_event *>(data);
    wlr_seat *seat = server->seat;

    // Translate libinput keycode -> xkbcommon
    uint32_t keycode = event->keycode + 8;
    // Get a list of keysyms based on the keymap for this keyboard
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(
        keyboard->wlr->xkb_state, keycode, &syms);

    bool handled = false;
    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr);
    if ((modifiers & WLR_MODIFIER_ALT) &&
            event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        // If alt is held down and this button was _pressed_, we attempt to
        // process it as a compositor keybinding.
        for (int i = 0; i < nsyms; i++) {
            handled = handle_keybinding(server, syms[i], modifiers);
        }
    }

    if (!handled) {
        // Otherwise, we pass it along to the client.
        wlr_seat_set_keyboard(seat, keyboard->wlr);
        wlr_seat_keyboard_notify_key(seat, event->time_msec,
            event->keycode, event->state);
    }
}

static void keyboard_handle_destroy(wl_listener *listener, void *data) {
    (void)data;
    // This event is raised by the keyboard base wlr_input_device to signal
    // the destruction of the wlr_keyboard. It will no longer receive events
    // and should be destroyed.
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

    // We need to prepare an XKB keymap and assign it to the keyboard. This
    // assumes the defaults (e.g. layout = "us").
    xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap *keymap = xkb_keymap_new_from_names(context, nullptr,
        XKB_KEYMAP_COMPILE_NO_FLAGS);

    wlr_keyboard_set_keymap(wlr_keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

    // Here we set up listeners for keyboard events.
    keyboard->modifiers.notify = keyboard_handle_modifiers;
    wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
    keyboard->key.notify = keyboard_handle_key;
    wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
    keyboard->destroy.notify = keyboard_handle_destroy;
    wl_signal_add(&device->events.destroy, &keyboard->destroy);

    wlr_seat_set_keyboard(server->seat, keyboard->wlr);

    // And add the keyboard to our list of keyboards
    wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(BiomeServer *server, wlr_input_device *device) {
    // We don't do anything special with pointers. All of our pointer
    // handling is proxied through wlr_cursor. On another compositor, you
    // might take this opportunity to do libinput configuration on the
    // device to set acceleration, etc.
    wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_input(wl_listener *listener, void *data) {
    // This event is raised by the backend when a new input device becomes
    // available.
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
    // We need to let the wlr_seat know what our capabilities are, which is
    // communicated to the client. In Biome we always have a cursor, even if
    // there are no pointer devices, so we always include that capability.
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(server->seat, caps);
}

static void seat_request_cursor(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, request_cursor);
    // This event is raised by the seat when a client provides a cursor image
    auto *event = static_cast<wlr_seat_pointer_request_set_cursor_event *>(data);
    wlr_seat_client *focused_client = server->seat->pointer_state.focused_client;
    // This can be sent by any client, so we check to make sure this one
    // actually has pointer focus first.
    if (focused_client == event->seat_client) {
        // Once we've vetted the client, we can tell the cursor to use the
        // provided surface as the cursor image. It will set the hardware
        // cursor on the output that it's currently on and continue to do so
        // as the cursor moves between outputs.
        wlr_cursor_set_surface(server->cursor, event->surface,
            event->hotspot_x, event->hotspot_y);
    }
}

static void seat_request_set_selection(wl_listener *listener, void *data) {
    // This event is raised by the seat when a client wants to set the
    // selection, usually when the user copies something. wlroots allows
    // compositors to ignore such requests if they so choose, but in Biome
    // we always honor them.
    BiomeServer *server = wl_container_of(listener, server, request_set_selection);
    auto *event = static_cast<wlr_seat_request_set_selection_event *>(data);
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static BiomeToplevel *desktop_toplevel_at(
        BiomeServer *server, double lx, double ly,
        wlr_surface **surface, double *sx, double *sy) {
    // This returns the topmost node in the scene at the given layout
    // coords. We only care about surface nodes as we are specifically
    // looking for a surface in the surface tree of a BiomeToplevel.
    wlr_scene_node *node = wlr_scene_node_at(
        &server->scene->tree.node, lx, ly, sx, sy);
    if (node == nullptr || node->type != WLR_SCENE_NODE_BUFFER) {
        return nullptr;
    }
    wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    if (!scene_surface) {
        return nullptr;
    }

    *surface = scene_surface->surface;
    // Find the node corresponding to the BiomeToplevel at the root of this
    // surface tree, it is the only one for which we set the data field.
    // (Override-redirect Xwayland surfaces never set this, so clicking one
    // yields toplevel == nullptr - pointer events still reach it via
    // *surface above, it just isn't managed by us.)
    wlr_scene_tree *tree = node->parent;
    while (tree != nullptr && tree->node.data == nullptr) {
        tree = tree->node.parent;
    }
    return tree ? static_cast<BiomeToplevel *>(tree->node.data) : nullptr;
}

static void reset_cursor_mode(BiomeServer *server) {
    // Reset the cursor mode to passthrough.
    server->cursor_mode = BiomeCursorMode::Passthrough;
    server->grabbed_toplevel = nullptr;
}

static void process_cursor_move(BiomeServer *server, uint32_t time) {
    (void)time;
    // Move the grabbed toplevel to the new position.
    BiomeToplevel *toplevel = server->grabbed_toplevel;
    int x = static_cast<int>(server->cursor->x - server->grab_x);
    int y = static_cast<int>(server->cursor->y - server->grab_y);
    wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
    // The X server has no notion of our border - tell it about the visible
    // content position, not the container's.
    toplevel_sync_position(toplevel, x + kBorderWidth, y + kBorderWidth);
}

static void process_cursor_resize(BiomeServer *server, uint32_t time) {
    (void)time;
    // Resizing the grabbed toplevel can be a little bit complicated, because
    // we could be resizing from any corner or edge. This not only resizes
    // the toplevel on one or two axes, but can also move the toplevel if you
    // resize from the top or left edges (or top-left corner).
    //
    // Note that some shortcuts are taken here. In a more fleshed-out
    // compositor, you'd wait for the client to prepare a buffer at the new
    // size, then commit any movement that was prepared.
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
    wlr_scene_node_set_position(&toplevel->scene_tree->node,
        new_left - geo_box.x - kBorderWidth, new_top - geo_box.y - kBorderWidth);

    int new_width = new_right - new_left;
    int new_height = new_bottom - new_top;
    toplevel_set_size(toplevel, new_left - geo_box.x, new_top - geo_box.y, new_width, new_height);
    update_toplevel_decoration(toplevel);
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
        // If there's no toplevel under the cursor, set the cursor image to
        // a default. This is what makes the cursor image appear when you
        // move it around the screen, not over any toplevels.
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
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

static void server_cursor_motion(wl_listener *listener, void *data) {
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

static void server_cursor_motion_absolute(wl_listener *listener, void *data) {
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

static void server_cursor_button(wl_listener *listener, void *data) {
    // This event is forwarded by the cursor when a pointer emits a button event.
    BiomeServer *server = wl_container_of(listener, server, cursor_button);
    auto *event = static_cast<wlr_pointer_button_event *>(data);
    // Notify the client with pointer focus that a button press has occurred
    wlr_seat_pointer_notify_button(server->seat,
        event->time_msec, event->button, event->state);
    double sx, sy;
    wlr_surface *surface = nullptr;
    BiomeToplevel *toplevel = desktop_toplevel_at(server,
        server->cursor->x, server->cursor->y, &surface, &sx, &sy);
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        // If you released any buttons, we exit interactive move/resize mode.
        reset_cursor_mode(server);
    } else {
        // Focus that client if the button was _pressed_
        focus_toplevel(toplevel, surface);
    }
}

static void server_cursor_axis(wl_listener *listener, void *data) {
    // This event is forwarded by the cursor when a pointer emits an axis
    // event, for example when you move the scroll wheel.
    BiomeServer *server = wl_container_of(listener, server, cursor_axis);
    auto *event = static_cast<wlr_pointer_axis_event *>(data);
    // Notify the client with pointer focus of the axis event.
    wlr_seat_pointer_notify_axis(server->seat,
        event->time_msec, event->orientation, event->delta,
        event->delta_discrete, event->source, event->relative_direction);
}

static void server_cursor_frame(wl_listener *listener, void *data) {
    (void)data;
    // This event is forwarded by the cursor when a pointer emits an frame
    // event. Frame events are sent after regular pointer events to group
    // multiple events together. For instance, two axis events may happen at
    // the same time, in which case a frame event won't be sent in between.
    BiomeServer *server = wl_container_of(listener, server, cursor_frame);
    // Notify the client with pointer focus of the frame event.
    wlr_seat_pointer_notify_frame(server->seat);
}

static void output_frame(wl_listener *listener, void *data) {
    (void)data;
    // This function is called every time an output is ready to display a
    // frame, generally at the output's refresh rate (e.g. 60Hz).
    BiomeOutput *output = wl_container_of(listener, output, frame);
    wlr_scene *scene = output->server->scene;

    wlr_scene_output *scene_output = wlr_scene_get_scene_output(scene, output->wlr);

    // Render the scene if needed and commit the output
    wlr_scene_output_commit(scene_output, nullptr);

    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(wl_listener *listener, void *data) {
    // This function is called when the backend requests a new state for the
    // output. For example, Wayland and X11 backends request a new mode when
    // the output window is resized.
    BiomeOutput *output = wl_container_of(listener, output, request_state);
    auto *event = static_cast<const wlr_output_event_request_state *>(data);
    wlr_output_commit_state(output->wlr, event->state);
}

static void output_destroy(wl_listener *listener, void *data) {
    (void)data;
    BiomeOutput *output = wl_container_of(listener, output, destroy);

    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    free(output);
}

static void server_new_output(wl_listener *listener, void *data) {
    // This event is raised by the backend when a new output (aka a display
    // or monitor) becomes available.
    BiomeServer *server = wl_container_of(listener, server, new_output);
    auto *wlr_output = static_cast<struct wlr_output *>(data);

    // Configures the output created by the backend to use our allocator and
    // our renderer. Must be done once, before committing the output
    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    // The output may be disabled, switch it on.
    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    // Some backends don't have modes. DRM+KMS does, and we need to set a
    // mode before we can use the output. The mode is a tuple of (width,
    // height, refresh rate), and each monitor supports only a specific set
    // of modes. We just pick the monitor's preferred mode, a more
    // sophisticated compositor would let the user configure it.
    wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode != nullptr) {
        wlr_output_state_set_mode(&state, mode);
    }

    // Atomically applies the new output state.
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    // Allocates and configures our state for this output
    auto *output = static_cast<BiomeOutput *>(calloc(1, sizeof(BiomeOutput)));
    output->wlr = wlr_output;
    output->server = server;

    // Sets up a listener for the frame event.
    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);

    // Sets up a listener for the state request event.
    output->request_state.notify = output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);

    // Sets up a listener for the destroy event.
    output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    wl_list_insert(&server->outputs, &output->link);

    // Adds this to the output layout. The add_auto function arranges
    // outputs from left-to-right in the order they appear. A more
    // sophisticated compositor would let the user configure the arrangement
    // of outputs in the layout.
    //
    // The output layout utility automatically adds a wl_output global to
    // the display, which Wayland clients can see to find out information
    // about the output (such as DPI, scale factor, manufacturer, etc).
    wlr_output_layout_output *l_output =
        wlr_output_layout_add_auto(server->output_layout, wlr_output);
    wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);
}

// Places a newly-mapped floating toplevel. Window rule: a transient window
// (one with a parent, e.g. a dialog) centers on its parent, matching
// xfwm4's default dialog placement. Otherwise it's centered on the output
// layout, with a small cascading offset per concurrently-open window so
// repeated launches don't stack exactly on top of each other - xfwm4's
// default (non-tiling) placement, not anything protocol-driven.
static void place_new_toplevel(BiomeToplevel *toplevel) {
    BiomeServer *server = toplevel->server;

    wlr_box geo;
    toplevel_get_geometry(toplevel, &geo);
    int width = geo.width > 0 ? geo.width : 0;
    int height = geo.height > 0 ? geo.height : 0;

    BiomeToplevel *parent = toplevel->type == BiomeToplevelType::Xdg
        ? toplevel_from_xdg(toplevel->xdg_toplevel->parent)
        : toplevel_from_xwayland(toplevel->xwayland_surface->parent);

    // (vis_x, vis_y): desired top-left of the *visible content*, i.e.
    // ignoring our border - toplevel_sync_position and the scene node
    // position (which also needs the border subtracted) are derived from
    // this below.
    int vis_x, vis_y;

    if (parent != nullptr) {
        wlr_box parent_geo;
        toplevel_get_geometry(parent, &parent_geo);
        int parent_vis_x = static_cast<int>(parent->scene_tree->node.x) + kBorderWidth + parent_geo.x;
        int parent_vis_y = static_cast<int>(parent->scene_tree->node.y) + kBorderWidth + parent_geo.y;
        vis_x = parent_vis_x + (parent_geo.width - width) / 2;
        vis_y = parent_vis_y + (parent_geo.height - height) / 2;
        toplevel->workspace = parent->workspace;
    } else {
        wlr_box layout_box;
        wlr_output_layout_get_box(server->output_layout, nullptr, &layout_box);
        if (wlr_box_empty(&layout_box)) {
            return;
        }
        int index = static_cast<int>(wl_list_length(&server->toplevels)) % 8;
        int cascade = index * 24;
        vis_x = layout_box.x + (layout_box.width - width) / 2 + cascade;
        vis_y = layout_box.y + (layout_box.height - height) / 2 + cascade;
        toplevel->workspace = server->active_workspace;
    }

    wlr_scene_node_set_position(&toplevel->scene_tree->node,
        vis_x - kBorderWidth, vis_y - kBorderWidth);
    toplevel_sync_position(toplevel, vis_x, vis_y);
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, toplevel->workspace == server->active_workspace);
}

// Shared between xdg-shell and Xwayland: called once the underlying
// wlr_surface is ready to be shown on screen.
static void toplevel_map(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, map);

    place_new_toplevel(toplevel);
    update_toplevel_decoration(toplevel);
    wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
    focus_toplevel(toplevel, toplevel_surface(toplevel));
}

static void toplevel_unmap(wl_listener *listener, void *data) {
    (void)data;
    // Called when the surface is unmapped, and should no longer be shown.
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, unmap);

    // Reset the cursor mode if the grabbed toplevel was unmapped.
    if (toplevel == toplevel->server->grabbed_toplevel) {
        reset_cursor_mode(toplevel->server);
    }

    wl_list_remove(&toplevel->link);
}

static void xdg_toplevel_commit(wl_listener *listener, void *data) {
    (void)data;
    // Called when a new surface state is committed.
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, commit);

    if (toplevel->xdg_toplevel->base->initial_commit) {
        // When an xdg_surface performs an initial commit, the compositor
        // must reply with a configure so the client can map the surface.
        // Biome configures the xdg_toplevel with 0,0 size to let the client
        // pick the dimensions itself.
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
        return;
    }
    // The client may have resized itself (e.g. due to a content change)
    // outside of an interactive grab - keep the border in sync.
    update_toplevel_decoration(toplevel);
}

static void xdg_toplevel_destroy(wl_listener *listener, void *data) {
    (void)data;
    // Called when the xdg_toplevel is destroyed.
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, destroy);

    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->commit.link);
    wl_list_remove(&toplevel->destroy.link);
    wl_list_remove(&toplevel->request_move.link);
    wl_list_remove(&toplevel->request_resize.link);
    wl_list_remove(&toplevel->request_maximize.link);
    wl_list_remove(&toplevel->request_fullscreen.link);

    // Unlike Phase 1, scene_tree is a plain wlr_scene_tree_create() we
    // allocated ourselves (to hold the border rects), not one tied to the
    // xdg_surface's own lifecycle - we have to destroy it explicitly.
    // This recursively destroys content_tree and the border rects too.
    wlr_scene_node_destroy(&toplevel->scene_tree->node);

    free(toplevel);
}

static void begin_interactive(BiomeToplevel *toplevel, BiomeCursorMode mode, uint32_t edges) {
    // This function sets up an interactive move or resize operation, where
    // the compositor stops propagating pointer events to clients and
    // instead consumes them itself, to move or resize windows.
    BiomeServer *server = toplevel->server;
    wlr_surface *focused_surface = server->seat->pointer_state.focused_surface;
    if (toplevel_surface(toplevel) != wlr_surface_get_root_surface(focused_surface)) {
        // Deny move/resize requests from unfocused clients.
        return;
    }
    server->grabbed_toplevel = toplevel;
    server->cursor_mode = mode;

    if (mode == BiomeCursorMode::Move) {
        server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
        server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
    } else {
        wlr_box geo_box;
        toplevel_get_geometry(toplevel, &geo_box);

        double border_x = (toplevel->scene_tree->node.x + kBorderWidth + geo_box.x) +
            ((edges & WLR_EDGE_RIGHT) ? geo_box.width : 0);
        double border_y = (toplevel->scene_tree->node.y + kBorderWidth + geo_box.y) +
            ((edges & WLR_EDGE_BOTTOM) ? geo_box.height : 0);
        server->grab_x = server->cursor->x - border_x;
        server->grab_y = server->cursor->y - border_y;

        server->grab_geobox = geo_box;
        server->grab_geobox.x += static_cast<int>(toplevel->scene_tree->node.x) + kBorderWidth;
        server->grab_geobox.y += static_cast<int>(toplevel->scene_tree->node.y) + kBorderWidth;

        server->resize_edges = edges;
    }
}

// Shared between xdg-shell and Xwayland: both signal this with irrelevant
// (or no) event data, so the handler is identical either way.
static void toplevel_request_move(wl_listener *listener, void *data) {
    (void)data;
    // This event is raised when a client would like to begin an interactive
    // move, typically because the user clicked on their client-side
    // decorations. Note that a more sophisticated compositor should check
    // the provided serial against a list of button press serials sent to
    // this client, to prevent the client from requesting this whenever they
    // want.
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_move);
    begin_interactive(toplevel, BiomeCursorMode::Move, 0);
}

static void xdg_toplevel_request_resize(wl_listener *listener, void *data) {
    // This event is raised when a client would like to begin an interactive
    // resize, typically because the user clicked on their client-side
    // decorations. Note that a more sophisticated compositor should check
    // the provided serial against a list of button press serials sent to
    // this client, to prevent the client from requesting this whenever they
    // want.
    auto *event = static_cast<wlr_xdg_toplevel_resize_event *>(data);
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
    begin_interactive(toplevel, BiomeCursorMode::Resize, event->edges);
}

static void xdg_toplevel_request_maximize(wl_listener *listener, void *data) {
    (void)data;
    // This event is raised when a client would like to maximize itself,
    // typically because the user clicked on the maximize button on
    // client-side decorations. Biome doesn't support maximization, but to
    // conform to xdg-shell protocol we still must send a configure.
    // wlr_xdg_surface_schedule_configure() is used to send an empty reply.
    // However, if the request was sent before an initial commit, we don't
    // do anything and let the client finish the initial surface setup.
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_maximize);
    if (toplevel->xdg_toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
    }
}

static void xdg_toplevel_request_fullscreen(wl_listener *listener, void *data) {
    (void)data;
    // Just as with request_maximize, we must send a configure here.
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);
    if (toplevel->xdg_toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
    }
}

static void server_new_xdg_toplevel(wl_listener *listener, void *data) {
    // This event is raised when a client creates a new toplevel (application window).
    BiomeServer *server = wl_container_of(listener, server, new_xdg_toplevel);
    auto *xdg_toplevel = static_cast<wlr_xdg_toplevel *>(data);

    // Allocate a BiomeToplevel for this surface
    auto *toplevel = static_cast<BiomeToplevel *>(calloc(1, sizeof(BiomeToplevel)));
    toplevel->server = server;
    toplevel->type = BiomeToplevelType::Xdg;
    toplevel->xdg_toplevel = xdg_toplevel;

    toplevel->scene_tree = wlr_scene_tree_create(&toplevel->server->scene->tree);
    toplevel->scene_tree->node.data = toplevel;
    create_toplevel_border(toplevel);

    toplevel->content_tree =
        wlr_scene_xdg_surface_create(toplevel->scene_tree, xdg_toplevel->base);
    toplevel->content_tree->node.data = toplevel;
    wlr_scene_node_set_position(&toplevel->content_tree->node, kBorderWidth, kBorderWidth);
    xdg_toplevel->base->data = toplevel->content_tree;

    // Listen to the various events it can emit
    toplevel->map.notify = toplevel_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
    toplevel->unmap.notify = toplevel_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
    toplevel->commit.notify = xdg_toplevel_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);

    toplevel->destroy.notify = xdg_toplevel_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

    toplevel->request_move.notify = toplevel_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
    toplevel->request_resize.notify = xdg_toplevel_request_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
    toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
    wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
    toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
}

static void xdg_popup_commit(wl_listener *listener, void *data) {
    (void)data;
    // Called when a new surface state is committed.
    BiomePopup *popup = wl_container_of(listener, popup, commit);

    if (popup->xdg_popup->base->initial_commit) {
        // When an xdg_surface performs an initial commit, the compositor
        // must reply with a configure so the client can map the surface.
        // Biome sends an empty configure. A more sophisticated compositor
        // might change an xdg_popup's geometry to ensure it's not
        // positioned off-screen, for example.
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
    }
}

static void xdg_popup_destroy(wl_listener *listener, void *data) {
    (void)data;
    // Called when the xdg_popup is destroyed.
    BiomePopup *popup = wl_container_of(listener, popup, destroy);

    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);

    free(popup);
}

static void server_new_xdg_popup(wl_listener *listener, void *data) {
    (void)listener;
    // This event is raised when a client creates a new popup.
    auto *xdg_popup = static_cast<wlr_xdg_popup *>(data);

    auto *popup = static_cast<BiomePopup *>(calloc(1, sizeof(BiomePopup)));
    popup->xdg_popup = xdg_popup;

    // We must add xdg popups to the scene graph so they get rendered. The
    // wlroots scene graph provides a helper for this, but to use it we must
    // provide the proper parent scene node of the xdg popup. To enable
    // this, we always set the user data field of xdg_surfaces to the
    // corresponding scene node.
    wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
    assert(parent != nullptr);
    auto *parent_tree = static_cast<wlr_scene_tree *>(parent->data);
    xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

    popup->commit.notify = xdg_popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

    popup->destroy.notify = xdg_popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

// --- Xwayland: managed toplevels -------------------------------------

static void xwayland_toplevel_associate(wl_listener *listener, void *data) {
    (void)data;
    // The wlr_surface backing this X11 window now exists - hook up map/
    // unmap (shared with xdg-shell) and create its scene node.
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, associate);
    wlr_xwayland_surface *xsurface = toplevel->xwayland_surface;

    toplevel->content_tree =
        wlr_scene_subsurface_tree_create(toplevel->scene_tree, xsurface->surface);
    toplevel->content_tree->node.data = toplevel;
    wlr_scene_node_set_position(&toplevel->content_tree->node, kBorderWidth, kBorderWidth);
    xsurface->data = toplevel->content_tree;

    toplevel->map.notify = toplevel_map;
    wl_signal_add(&xsurface->surface->events.map, &toplevel->map);
    toplevel->unmap.notify = toplevel_unmap;
    wl_signal_add(&xsurface->surface->events.unmap, &toplevel->unmap);
}

static void xwayland_toplevel_dissociate(wl_listener *listener, void *data) {
    (void)data;
    // The wlr_surface is going away (but the X11 window wrapper itself may
    // persist, e.g. it could be re-associated later). wlroots destroys the
    // scene node tied to the surface itself, we just drop our listeners.
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, dissociate);
    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    toplevel->xwayland_surface->data = nullptr;
    // content_tree's node is destroyed by wlroots along with the surface
    // it wraps; scene_tree (the container, and its border rects) persists
    // in case this surface re-associates later.
    toplevel->content_tree = nullptr;
}

static void xwayland_toplevel_destroy(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, destroy);

    wl_list_remove(&toplevel->associate.link);
    wl_list_remove(&toplevel->dissociate.link);
    wl_list_remove(&toplevel->destroy.link);
    wl_list_remove(&toplevel->request_move.link);
    wl_list_remove(&toplevel->request_resize.link);
    wl_list_remove(&toplevel->request_maximize.link);
    wl_list_remove(&toplevel->request_fullscreen.link);
    wl_list_remove(&toplevel->request_configure.link);

    // scene_tree (the container + border rects) was created up front in
    // server_new_xwayland_surface and outlives any single associate/
    // dissociate cycle, so we own destroying it.
    wlr_scene_node_destroy(&toplevel->scene_tree->node);

    free(toplevel);
}

static void xwayland_toplevel_request_resize(wl_listener *listener, void *data) {
    auto *event = static_cast<wlr_xwayland_resize_event *>(data);
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
    begin_interactive(toplevel, BiomeCursorMode::Resize, event->edges);
}

static void xwayland_toplevel_request_maximize(wl_listener *listener, void *data) {
    (void)data;
    // Biome doesn't support maximization yet (matches the xdg-shell path);
    // explicitly reject so clients don't sit in a "waiting for the WM"
    // state.
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_maximize);
    wlr_xwayland_surface_set_maximized(toplevel->xwayland_surface, false);
}

static void xwayland_toplevel_request_fullscreen(wl_listener *listener, void *data) {
    (void)data;
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);
    wlr_xwayland_surface_set_fullscreen(toplevel->xwayland_surface, false);
}

static void xwayland_toplevel_request_configure(wl_listener *listener, void *data) {
    // X11 clients can ask to move/resize themselves outside of an
    // interactive grab (e.g. a terminal resizing to fit its font). Biome
    // doesn't second-guess these, it just honors them and keeps the scene
    // node in sync.
    BiomeToplevel *toplevel = wl_container_of(listener, toplevel, request_configure);
    auto *event = static_cast<wlr_xwayland_surface_configure_event *>(data);
    wlr_xwayland_surface_configure(toplevel->xwayland_surface,
        event->x, event->y, event->width, event->height);
    if (toplevel->content_tree) {
        wlr_scene_node_set_position(&toplevel->scene_tree->node,
            event->x - kBorderWidth, event->y - kBorderWidth);
        update_toplevel_decoration(toplevel);
    }
}

// --- Xwayland: override-redirect (unmanaged) surfaces -----------------

static void unmanaged_associate(wl_listener *listener, void *data) {
    (void)data;
    BiomeUnmanaged *surface = wl_container_of(listener, surface, associate);
    wlr_xwayland_surface *xsurface = surface->xwayland_surface;

    surface->scene_tree =
        wlr_scene_subsurface_tree_create(&surface->server->scene->tree, xsurface->surface);
    wlr_scene_node_set_position(&surface->scene_tree->node, xsurface->x, xsurface->y);

    surface->map.notify = [](wl_listener *l, void *d) {
        (void)d;
        BiomeUnmanaged *s = wl_container_of(l, s, map);
        wlr_scene_node_raise_to_top(&s->scene_tree->node);
        if (wlr_xwayland_or_surface_wants_focus(s->xwayland_surface)) {
            wlr_seat *seat = s->server->seat;
            wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
            wlr_seat_keyboard_notify_enter(seat, s->xwayland_surface->surface,
                keyboard ? keyboard->keycodes : nullptr,
                keyboard ? keyboard->num_keycodes : 0,
                keyboard ? &keyboard->modifiers : nullptr);
        }
    };
    wl_signal_add(&xsurface->surface->events.map, &surface->map);

    surface->unmap.notify = [](wl_listener *l, void *d) {
        (void)d;
        BiomeUnmanaged *s = wl_container_of(l, s, unmap);
        wlr_seat *seat = s->server->seat;
        if (seat->keyboard_state.focused_surface != s->xwayland_surface->surface) {
            return;
        }
        // Hand focus back to the topmost managed toplevel, if any.
        if (!wl_list_empty(&s->server->toplevels)) {
            BiomeToplevel *top = wl_container_of(s->server->toplevels.next, top, link);
            focus_toplevel(top, toplevel_surface(top));
        } else {
            wlr_seat_keyboard_notify_clear_focus(seat);
        }
    };
    wl_signal_add(&xsurface->surface->events.unmap, &surface->unmap);
}

static void unmanaged_dissociate(wl_listener *listener, void *data) {
    (void)data;
    BiomeUnmanaged *surface = wl_container_of(listener, surface, dissociate);
    wl_list_remove(&surface->map.link);
    wl_list_remove(&surface->unmap.link);
    surface->scene_tree = nullptr;
}

static void unmanaged_destroy(wl_listener *listener, void *data) {
    (void)data;
    BiomeUnmanaged *surface = wl_container_of(listener, surface, destroy);
    wl_list_remove(&surface->associate.link);
    wl_list_remove(&surface->dissociate.link);
    wl_list_remove(&surface->destroy.link);
    wl_list_remove(&surface->request_configure.link);
    free(surface);
}

static void unmanaged_request_configure(wl_listener *listener, void *data) {
    BiomeUnmanaged *surface = wl_container_of(listener, surface, request_configure);
    auto *event = static_cast<wlr_xwayland_surface_configure_event *>(data);
    wlr_xwayland_surface_configure(surface->xwayland_surface,
        event->x, event->y, event->width, event->height);
    if (surface->scene_tree) {
        wlr_scene_node_set_position(&surface->scene_tree->node, event->x, event->y);
    }
}

static void server_new_xwayland_surface(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, new_xwayland_surface);
    auto *xsurface = static_cast<wlr_xwayland_surface *>(data);

    if (xsurface->override_redirect) {
        auto *surface = static_cast<BiomeUnmanaged *>(calloc(1, sizeof(BiomeUnmanaged)));
        surface->server = server;
        surface->xwayland_surface = xsurface;

        surface->associate.notify = unmanaged_associate;
        wl_signal_add(&xsurface->events.associate, &surface->associate);
        surface->dissociate.notify = unmanaged_dissociate;
        wl_signal_add(&xsurface->events.dissociate, &surface->dissociate);
        surface->destroy.notify = unmanaged_destroy;
        wl_signal_add(&xsurface->events.destroy, &surface->destroy);
        surface->request_configure.notify = unmanaged_request_configure;
        wl_signal_add(&xsurface->events.request_configure, &surface->request_configure);
        return;
    }

    auto *toplevel = static_cast<BiomeToplevel *>(calloc(1, sizeof(BiomeToplevel)));
    toplevel->server = server;
    toplevel->type = BiomeToplevelType::Xwayland;
    toplevel->xwayland_surface = xsurface;

    // Created up front (unlike content_tree, which comes and goes with
    // associate/dissociate) so the border rects survive re-association.
    toplevel->scene_tree = wlr_scene_tree_create(&server->scene->tree);
    toplevel->scene_tree->node.data = toplevel;
    create_toplevel_border(toplevel);

    toplevel->associate.notify = xwayland_toplevel_associate;
    wl_signal_add(&xsurface->events.associate, &toplevel->associate);
    toplevel->dissociate.notify = xwayland_toplevel_dissociate;
    wl_signal_add(&xsurface->events.dissociate, &toplevel->dissociate);
    toplevel->destroy.notify = xwayland_toplevel_destroy;
    wl_signal_add(&xsurface->events.destroy, &toplevel->destroy);
    toplevel->request_move.notify = toplevel_request_move;
    wl_signal_add(&xsurface->events.request_move, &toplevel->request_move);
    toplevel->request_resize.notify = xwayland_toplevel_request_resize;
    wl_signal_add(&xsurface->events.request_resize, &toplevel->request_resize);
    toplevel->request_maximize.notify = xwayland_toplevel_request_maximize;
    wl_signal_add(&xsurface->events.request_maximize, &toplevel->request_maximize);
    toplevel->request_fullscreen.notify = xwayland_toplevel_request_fullscreen;
    wl_signal_add(&xsurface->events.request_fullscreen, &toplevel->request_fullscreen);
    toplevel->request_configure.notify = xwayland_toplevel_request_configure;
    wl_signal_add(&xsurface->events.request_configure, &toplevel->request_configure);
}

static void server_xwayland_ready(wl_listener *listener, void *data) {
    (void)data;
    // Give Xwayland a cursor image as soon as it's up - without this, X11
    // clients show no cursor at all until they set their own.
    BiomeServer *server = wl_container_of(listener, server, xwayland_ready);
    wlr_xcursor *xcursor = wlr_xcursor_manager_get_xcursor(server->cursor_mgr, "default", 1.0f);
    if (xcursor != nullptr && xcursor->image_count > 0) {
        wlr_xcursor_image *image = xcursor->images[0];
        wlr_xwayland_set_cursor(server->xwayland, image->buffer,
            image->width * 4, image->width, image->height,
            static_cast<int32_t>(image->hotspot_x), static_cast<int32_t>(image->hotspot_y));
    }
}

int main(int argc, char *argv[]) {
    wlr_log_init(WLR_DEBUG, nullptr);
    char *startup_cmd = nullptr;

    int c;
    while ((c = getopt(argc, argv, "s:h")) != -1) {
        switch (c) {
        case 's':
            startup_cmd = optarg;
            break;
        default:
            printf("Usage: %s [-s startup command]\n", argv[0]);
            return 0;
        }
    }
    if (optind < argc) {
        printf("Usage: %s [-s startup command]\n", argv[0]);
        return 0;
    }

    BiomeServer server;
    // The Wayland display is managed by libwayland. It handles accepting
    // clients from the Unix socket, managing Wayland globals, and so on.
    server.display = wl_display_create();
    // The backend is a wlroots feature which abstracts the underlying input
    // and output hardware. The autocreate option will choose the most
    // suitable backend based on the current environment, such as opening an
    // X11 window if an X11 server is running.
    server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.display), &server.session);
    if (server.backend == nullptr) {
        wlr_log(WLR_ERROR, "failed to create wlr_backend");
        return 1;
    }

    // Autocreates a renderer, either Pixman, GLES2 or Vulkan for us. The
    // user can also specify a renderer using the WLR_RENDERER env var. The
    // renderer is responsible for defining the various pixel formats it
    // supports for shared memory, this configures that for clients.
    server.renderer = wlr_renderer_autocreate(server.backend);
    if (server.renderer == nullptr) {
        wlr_log(WLR_ERROR, "failed to create wlr_renderer");
        return 1;
    }

    wlr_renderer_init_wl_display(server.renderer, server.display);

    // Autocreates an allocator for us. The allocator is the bridge between
    // the renderer and the backend. It handles the buffer creation,
    // allowing wlroots to render onto the screen.
    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    if (server.allocator == nullptr) {
        wlr_log(WLR_ERROR, "failed to create wlr_allocator");
        return 1;
    }

    // This creates some hands-off wlroots interfaces. The compositor is
    // necessary for clients to allocate surfaces, the subcompositor allows
    // to assign the role of subsurfaces to surfaces and the data device
    // manager handles the clipboard. Each of these wlroots interfaces has
    // room for you to dig your fingers in and play with their behavior if
    // you want. Note that the clients cannot set the selection directly
    // without compositor approval, see the handling of the
    // request_set_selection event below.
    wlr_compositor *compositor = wlr_compositor_create(server.display, 5, server.renderer);
    wlr_subcompositor_create(server.display);
    wlr_data_device_manager_create(server.display);

    // Creates an output layout, which a wlroots utility for working with an
    // arrangement of screens in a physical layout.
    server.output_layout = wlr_output_layout_create(server.display);

    // Configure a listener to be notified when new outputs are available on
    // the backend.
    wl_list_init(&server.outputs);
    server.new_output.notify = server_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    // Create a scene graph. This is a wlroots abstraction that handles all
    // rendering and damage tracking. All the compositor author needs to do
    // is add things that should be rendered to the scene graph at the
    // proper positions and then call wlr_scene_output_commit() to render a
    // frame if necessary.
    server.scene = wlr_scene_create();
    server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);

    // Set up xdg-shell version 3. The xdg-shell is a Wayland protocol which
    // is used for application windows.
    wl_list_init(&server.toplevels);
    server.xdg_shell = wlr_xdg_shell_create(server.display, 3);
    server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
    wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
    server.new_xdg_popup.notify = server_new_xdg_popup;
    wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

    // Creates a cursor, which is a wlroots utility for tracking the cursor
    // image shown on screen.
    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

    // Creates an xcursor manager, another wlroots utility which loads up
    // Xcursor themes to source cursor images from and makes sure that
    // cursor images are available at all scale factors on the screen
    // (necessary for HiDPI support).
    server.cursor_mgr = wlr_xcursor_manager_create(nullptr, 24);

    // wlr_cursor *only* displays an image on screen. It does not move
    // around when the pointer moves. However, we can attach input devices
    // to it, and it will generate aggregate events for all of them. In
    // these events, we can choose how we want to process them, forwarding
    // them to clients and moving the cursor around.
    server.cursor_mode = BiomeCursorMode::Passthrough;
    server.cursor_motion.notify = server_cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
    server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);
    server.cursor_button.notify = server_cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_button);
    server.cursor_axis.notify = server_cursor_axis;
    wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
    server.cursor_frame.notify = server_cursor_frame;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

    // Configures a seat, which is a single "seat" at which a user sits and
    // operates the computer. This conceptually includes up to one keyboard,
    // pointer, touch, and drawing tablet device. We also rig up a listener
    // to let us know when new input devices are available on the backend.
    wl_list_init(&server.keyboards);
    server.new_input.notify = server_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);
    server.seat = wlr_seat_create(server.display, "seat0");
    server.request_cursor.notify = seat_request_cursor;
    wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);
    server.request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&server.seat->events.request_set_selection, &server.request_set_selection);

    // Starts Xwayland (lazily - the actual Xwayland process only spawns
    // once a client tries to connect) so unmodified X11 apps can run
    // alongside native Wayland ones while the rest of Forest migrates. This
    // is optional: if it fails to start (e.g. Xwayland isn't installed),
    // Biome keeps running as a Wayland-only compositor.
    server.xwayland = wlr_xwayland_create(server.display, compositor, true);
    if (server.xwayland == nullptr) {
        wlr_log(WLR_ERROR, "failed to start Xwayland; X11 apps will not work");
    } else {
        server.new_xwayland_surface.notify = server_new_xwayland_surface;
        wl_signal_add(&server.xwayland->events.new_surface, &server.new_xwayland_surface);
        server.xwayland_ready.notify = server_xwayland_ready;
        wl_signal_add(&server.xwayland->events.ready, &server.xwayland_ready);
        wlr_xwayland_set_seat(server.xwayland, server.seat);
        setenv("DISPLAY", server.xwayland->display_name, true);
    }

    // Add a Unix socket to the Wayland display.
    const char *socket = wl_display_add_socket_auto(server.display);
    if (!socket) {
        wlr_backend_destroy(server.backend);
        return 1;
    }

    // Start the backend. This will enumerate outputs and inputs, become the
    // DRM master, etc.
    if (!wlr_backend_start(server.backend)) {
        wlr_backend_destroy(server.backend);
        wl_display_destroy(server.display);
        return 1;
    }

    // Set the WAYLAND_DISPLAY environment variable to our socket and run
    // the startup command if requested.
    setenv("WAYLAND_DISPLAY", socket, true);
    if (startup_cmd) {
        if (fork() == 0) {
            execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)nullptr);
        }
    }
    // Run the Wayland event loop. This does not return until you exit the
    // compositor. Starting the backend rigged up all of the necessary event
    // loop configuration to listen to libinput events, DRM events, generate
    // frame events at the refresh rate, and so on.
    wlr_log(WLR_INFO, "Running Biome on WAYLAND_DISPLAY=%s", socket);
    wl_display_run(server.display);

    // Once wl_display_run returns, we destroy all clients then shut down
    // the server.
    wl_display_destroy_clients(server.display);
    if (server.xwayland) {
        wlr_xwayland_destroy(server.xwayland);
    }
    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_xcursor_manager_destroy(server.cursor_mgr);
    wlr_cursor_destroy(server.cursor);
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.display);
    return 0;
}
