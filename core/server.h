// SPDX-License-Identifier: LGPL-3.0-or-later
//
// The compositor "world" state: BiomeServer (backend/output/input/seat, plus
// the handful of cross-cutting fields window/decoration handling needs to
// read - grabbed/hovered/pressed toplevel pointers, cursor mode, active
// workspace), BiomeOutput, and BiomeKeyboard. Window/toplevel state itself
// (BiomeToplevel and friends) lives in desktop/toplevel.h - this header only
// forward-declares it, since BiomeServer never needs more than a pointer to
// one.

#pragma once

#include "wlroots.hpp"

struct BiomeToplevel;

enum class BiomeCursorMode {
    Passthrough,
    Move,
    Resize,
};

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

    // Biome always draws its own decoration (see decoration/), so this just
    // forces server-side mode on every client that asks - no negotiation.
    wlr_xdg_decoration_manager_v1 *xdg_decoration_manager = nullptr;
    wl_listener new_xdg_toplevel_decoration = {};

    // GTK3 never implemented xdg-decoration above (only GTK4 did) - its only
    // way to learn a compositor wants server-side decorations is this older
    // KDE protocol. default_mode is set to Server right after creation (see
    // xdg_shell_init) and needs no per-client negotiation, same "always
    // server-side" stance as xdg_decoration_manager above. Without this,
    // GTK3 clients fall back to drawing their own CSD titlebar on top of
    // Biome's frame - the double-border bug this exists to fix.
    wlr_server_decoration_manager *kde_decoration_manager = nullptr;

    wlr_xwayland *xwayland = nullptr;
    wl_listener new_xwayland_surface = {};
    wl_listener xwayland_ready = {};

    // Atom-initialized once xwayland_ready fires (desktop/xwayland_shell.cpp)
    // - needed for _NET_WM_ICON lookup (desktop/app_icon.h). ewmh_ready
    // guards against using this before that init has actually run (or after
    // it failed), since xcb_ewmh_connection_t has no other "valid" sentinel.
    xcb_ewmh_connection_t ewmh = {};
    bool ewmh_ready = false;

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
    BiomeToplevel *grabbed_toplevel = nullptr;
    double grab_x = 0, grab_y = 0;
    wlr_box grab_geobox = {};
    uint32_t resize_edges = 0;

    // For double-click-titlebar-to-maximize detection in server_cursor_button.
    uint32_t last_left_click_time = 0;
    BiomeToplevel *last_left_click_toplevel = nullptr;

    // Which toplevel (if any) currently has a hovered/pressed decoration
    // button, so process_cursor_motion/server_cursor_button know when to
    // clear the old one's QSS :hover/:pressed state. Like
    // last_left_click_toplevel above, not cleared on toplevel destroy -
    // only ever compared, never dereferenced, so a stale pointer here is
    // harmless.
    BiomeToplevel *hovered_decoration_toplevel = nullptr;
    BiomeToplevel *pressed_decoration_toplevel = nullptr;

    int active_workspace = 0;

    // Graphical Alt-Tab switcher overlay. switcher_active tracks whether
    // Alt is currently held with the switcher shown (set on the first
    // Tab press, cleared on Alt release - see keyboard_handle_modifiers);
    // switcher_buffer is created once at startup and just hidden/shown.
    bool switcher_active = false;
    wlr_scene_buffer *switcher_buffer = nullptr;

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

struct BiomeKeyboard {
    wl_list link = {};
    BiomeServer *server = nullptr;
    wlr_keyboard *wlr = nullptr;

    wl_listener modifiers = {};
    wl_listener key = {};
    wl_listener destroy = {};
};
