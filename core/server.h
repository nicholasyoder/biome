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

#include "core/output_config.h"
#include "wlroots.hpp"

#include <unordered_map>
#include <vector>

struct BiomeToplevel;

// Whether Alt-Tab commits the focus change on every Tab press (matching
// Phase 2/3's original behavior) or only previews the selection and commits
// once on Alt release. Hardcoded until Biome has a real config file to read
// this from - see BiomeServer::switcher_order/switcher_preview_index below
// for the state that mode requires.
inline constexpr bool kSwitcherSwitchOnRelease = true;

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

    // Biome draws its own decoration (see decoration/) by default, but honors
    // a client's own request for client-side decoration instead - see
    // server_new_xdg_toplevel_decoration.
    wlr_xdg_decoration_manager_v1 *xdg_decoration_manager = nullptr;
    wl_listener new_xdg_toplevel_decoration = {};

    // GTK3 never implemented xdg-decoration above (only GTK4 did) - its only
    // way to negotiate decoration mode is this older KDE protocol.
    // default_mode (set right after creation in xdg_shell_init) is only the
    // initial value handed to a client before it asks for anything itself -
    // server_new_kde_decoration listens for a client's own request and
    // honors it the same way the xdg-decoration path above does.
    wlr_server_decoration_manager *kde_decoration_manager = nullptr;
    wl_listener new_kde_decoration = {};

    // KDE decoration objects created before their owning wl_surface has an
    // xdg_toplevel role yet - GTK routinely creates the decoration object
    // right after the wl_surface itself, before calling get_xdg_surface/
    // get_toplevel. Holds BiomePendingKdeDecoration nodes (desktop/
    // xdg_shell.cpp) until server_new_xdg_toplevel claims a match by
    // surface - see claim_pending_kde_decoration.
    wl_list pending_kde_decorations = {};

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
    wl_listener request_set_primary_selection = {};
    wl_listener request_start_drag = {};
    wl_listener start_drag = {};
    // Non-null only while a client-initiated wl_data_device drag with an
    // icon is in progress - see drag_icon_create/process_cursor_motion in
    // core/cursor.cpp. A drag with no icon (drag->icon == nullptr) leaves
    // this null for the drag's whole duration; nothing to position or clean
    // up in that case.
    wlr_scene_tree *drag_icon_tree = nullptr;
    wl_listener drag_icon_tree_destroy = {};
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
    // clear the old one's QSS :hover/:pressed state. Unlike
    // last_left_click_toplevel above (only ever compared, never
    // dereferenced), these ARE dereferenced when clearing hover/press state -
    // a stale pointer here is a real use-after-free, so both toplevel destroy
    // handlers clear these via desktop/decoration_bridge.h's
    // clear_decoration_tracking() before free(). See that function's doc
    // comment for the full story.
    BiomeToplevel *hovered_decoration_toplevel = nullptr;
    BiomeToplevel *pressed_decoration_toplevel = nullptr;

    int active_workspace = 0;

    // Graphical Alt-Tab switcher overlay. switcher_active tracks whether
    // Alt is currently held with the switcher shown (set on the first
    // Tab press, cleared on Alt release - see keyboard_handle_modifiers);
    // switcher_buffer is created once at startup and just hidden/shown.
    bool switcher_active = false;
    wlr_scene_buffer *switcher_buffer = nullptr;

    // Only used when kSwitcherSwitchOnRelease is true: switcher_order is a
    // snapshot of toplevels' MRU order taken on the first Tab press of a
    // hold (server->toplevels itself isn't touched again until Alt release,
    // since nothing commits mid-cycle), and switcher_preview_index is the
    // currently-highlighted offset into it, advanced by later Tab presses.
    std::vector<BiomeToplevel *> switcher_order;
    int switcher_preview_index = 0;

    wlr_output_layout *output_layout = nullptr;
    wl_list outputs = {};
    wl_listener new_output = {};
    // Loaded once at startup by output_manager_init() - see output_config.h.
    std::unordered_map<std::string, OutputConfig> output_configs;

    // ext-session-lock-v1 (desktop/session_lock.cpp). lock_tree is created
    // once at startup and just enabled/raised on lock, disabled on unlock -
    // same create-once-toggle-visibility pattern as switcher_buffer below.
    // See that file's header comment for the security invariant this relies
    // on: lock_tree must stay the topmost sibling of scene->tree for as long
    // as session_locked is true.
    wlr_session_lock_manager_v1 *session_lock_manager = nullptr;
    wl_listener new_session_lock = {};
    // Non-null only for the lifetime of the current lock's wl_resource -
    // nulled by lock_destroy (fires for both a clean unlock_and_destroy and
    // an abnormal client death). Deliberately a separate field from
    // session_locked below: the reject-a-second-lock check in
    // session_lock.cpp reads this one, not session_locked, so that a
    // replacement client can take over locking after the original lock
    // client crashes (session_locked stays true across that handoff).
    wlr_session_lock_v1 *active_lock = nullptr;
    wl_listener lock_new_surface = {};
    wl_listener lock_unlock = {};
    wl_listener lock_destroy = {};
    // True from the instant a lock is accepted until a real `unlock` event
    // fires (i.e. the client called unlock_and_destroy). Must NOT be reset
    // by active_lock going null on an abnormal client death - per the
    // ext-session-lock-v1 spec the compositor must not unlock the session in
    // that case, so this is the one field that has to survive a crash.
    bool session_locked = false;
    wlr_scene_tree *lock_tree = nullptr;
};

struct BiomeOutput {
    wl_list link = {};
    BiomeServer *server = nullptr;
    wlr_output *wlr = nullptr;
    wl_listener frame = {};
    wl_listener request_state = {};
    wl_listener destroy = {};

    // ext-session-lock-v1 (desktop/session_lock.cpp). Created unconditionally
    // for every output, locked or not, so a monitor that appears while
    // already locked is blanked from its very first frame with no special
    // hotplug-during-lock handling. lock_tree is a child of
    // BiomeServer::lock_tree, positioned at this output's layout coords;
    // lock_rect is the opaque fallback/backstop layer, always the first
    // (bottommost) child of lock_tree so a client's own lock surface (added
    // later, as a sibling) renders on top of it.
    wlr_scene_tree *lock_tree = nullptr;
    wlr_scene_rect *lock_rect = nullptr;
    // Non-null only while this output has a live client-provided lock
    // surface - needed by output_request_state() to re-configure it if the
    // output's resolution changes mid-lock (e.g. a nested dev backend's host
    // window resizing). Owned by wlroots; the owning BiomeLockSurface
    // wrapper (session_lock.cpp) clears this on the surface's own destroy.
    wlr_session_lock_surface_v1 *lock_surface = nullptr;
    // True from the start of a lock until this output has committed one
    // frame since - see session_lock.cpp's locked-event timing.
    bool pending_lock_frame = false;
};

struct BiomeKeyboard {
    wl_list link = {};
    BiomeServer *server = nullptr;
    wlr_keyboard *wlr = nullptr;

    wl_listener modifiers = {};
    wl_listener key = {};
    wl_listener destroy = {};
};
