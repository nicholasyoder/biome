// SPDX-License-Identifier: LGPL-3.0-or-later
//
// BiomeToplevel and the window management shared by xdg-shell and Xwayland:
// geometry, focus, placement, maximize/snap/fullscreen/minimize, always-on-
// top, and relocation across output changes. Backend-specific handling is in
// desktop/xdg_shell.h and desktop/xwayland_shell.h.

#pragma once

#include "core/server.h"
#include "decoration/layout.h" // biome_decoration::Region
#include "decoration/renderer.h" // biome_decoration::IconImage

namespace biome_decoration {
class DecorationFrame;
}

struct BiomeForeignToplevel;

enum class BiomeToplevelType {
    Xdg,
    Xwayland,
};

struct BiomeToplevel {
    wl_list link = {};
    BiomeServer *server = nullptr;
    BiomeToplevelType type = BiomeToplevelType::Xdg;
    int workspace = 0;

    // Has its first real position (place_new_toplevel); no decoration before.
    bool placed = false;

    // Set when the window's output was unplugged (e.g. a DPMS hotplug bounce) and it was
    // pulled elsewhere: lets the output's return put it back. Offset is from the output's origin.
    bool home_pending = false;
    char home_output[32] = {};
    int home_dx = 0, home_dy = 0;
    int home_rescued_x = 0, home_rescued_y = 0; // node position after the pull; moved since => user's

    // restore_box: pre-maximize/snap content box, layout coordinates.
    bool maximized = false;
    wlr_box restore_box = {};

    // Snapped to a half/quarter of the output (set_toplevel_snapped). Shares
    // restore_box with maximize; never set together with maximized.
    SnapZone snap_zone = SnapZone::None;

    // Independent of maximized, which survives a fullscreen round trip. While
    // set, scene_tree is in layers.fullscreen until another window on its
    // output takes focus.
    bool fullscreen = false;
    wlr_box fullscreen_restore_box = {};

    // xdg only: the new position waits for the client to ack this configure
    // serial (not a size change, which may not happen), so the old buffer is
    // never shown at the new position.
    bool reposition_pending = false;
    int reposition_pending_x = 0, reposition_pending_y = 0;
    uint32_t reposition_pending_serial = 0;
    // Center the client's new size on its output instead of using
    // reposition_pending_x/y (restoring to an unknown size).
    bool reposition_center = false;

    // xdg only: maximized/fullscreen configure sent before map (see
    // toplevel_configure_premap); premap_target is its content box.
    bool premap_pending = false;
    wlr_box premap_target = {};
    uint32_t premap_serial = 0;

    // Lives in layers.above instead of layers.toplevels (toplevel_layer()).
    bool always_on_top = false;

    bool minimized = false;

    // Demands attention (unhonored activation, X11 urgency hint). Never set
    // while focused; focusing clears it. See set_toplevel_urgent.
    bool urgent = false;

    // scene_tree's position is the window's; content_tree (the surfaces) and
    // decoration_buffer are its children, content offset by the border/titlebar.
    wlr_scene_tree *scene_tree = nullptr;
    wlr_scene_tree *content_tree = nullptr;
    wlr_scene_buffer *decoration_buffer = nullptr;
    bool focused = false; // drives which QSS [focused=...] state gets applied

    // This window's own QSS-styled frame widget (desktop/decoration_bridge.h).
    biome_decoration::DecorationFrame *decoration_frame = nullptr;

    // Hovered/pressed decoration button, if any.
    biome_decoration::Region hovered_region = biome_decoration::Region::None;
    biome_decoration::Region pressed_region = biome_decoration::Region::None;

    // Resolved once at map (desktop/app_icon.h); size 0 = none found.
    biome_decoration::IconImage icon;
    bool icon_resolved = false;

    // Last rendered decoration state, to skip unchanged re-renders. Title/icon
    // are keyed on serials, not pointers (ABA).
    bool decoration_rendered_once = false;
    int last_decoration_width = -1;
    int last_decoration_height = -1;
    bool last_decoration_focused = false;
    bool last_decoration_urgent = false;
    bool last_decoration_maximized = false;
    uint32_t title_serial = 0;
    uint32_t icon_serial = 0;
    uint32_t last_decoration_title_serial = 0;
    uint32_t last_decoration_icon_serial = 0;
    biome_decoration::Region last_decoration_hovered = biome_decoration::Region::None;
    biome_decoration::Region last_decoration_pressed = biome_decoration::Region::None;
    float last_decoration_scale = 0.0f;

    // Output scale the decoration renders at, tracked via decoration_buffer's
    // outputs_update (desktop/decoration_bridge.cpp).
    float decoration_scale = 1.0f;
    wl_listener decoration_outputs_update = {};
    wl_event_source *decoration_rescale_idle = nullptr;

    // Exists while mapped (desktop/foreign_toplevel.h).
    BiomeForeignToplevel *foreign_toplevel = nullptr;

    wlr_xdg_toplevel *xdg_toplevel = nullptr;         // type == Xdg
    wlr_xwayland_surface *xwayland_surface = nullptr; // type == Xwayland

    wl_listener map = {};
    wl_listener unmap = {};
    wl_listener commit = {}; // xdg only
    // Both backends: a title change after map.
    wl_listener set_title = {};
    wl_listener set_parent = {};
    wl_listener destroy = {};
    wl_listener request_move = {};
    wl_listener request_resize = {};
    wl_listener request_show_window_menu = {}; // xdg only
    wl_listener request_maximize = {};
    wl_listener request_fullscreen = {};
    wl_listener request_minimize = {};
    wl_listener set_decorations = {}; // xwayland only

    // xdg only: negotiation (either protocol) settled on client-side, or the
    // client created no decoration object at all (which both protocols define
    // as client-side).
    bool xdg_client_side_decorated = false;

    // xdg only: kept for its lifetime so a request_mode after map is honored.
    wlr_xdg_toplevel_decoration_v1 *decoration = nullptr;
    wl_listener decoration_destroy = {};
    wl_listener decoration_request_mode = {};

    // xdg only: the same via the KDE protocol (GTK3).
    wlr_server_decoration *kde_decoration = nullptr;
    wl_listener kde_decoration_destroy = {};
    wl_listener kde_decoration_mode = {};

    // Xwayland only: the underlying wlr_surface only exists between
    // associate/dissociate, so map/unmap are (dis)connected there instead
    // of at creation/destroy time like xdg-shell's are.
    wl_listener associate = {};
    wl_listener dissociate = {};
    wl_listener request_configure = {};
    wl_listener set_override_redirect = {};
    wl_listener request_activate = {}; // _NET_ACTIVE_WINDOW
    wl_listener set_hints = {};
};

// An override-redirect Xwayland surface (menus, tooltips, dnd icons, ...).
// These position themselves and are never part of server->toplevels - no
// compositor-driven focus, move, or resize.
struct BiomeUnmanaged {
    wl_list link = {}; // BiomeServer::unmanaged
    BiomeServer *server = nullptr;
    wlr_xwayland_surface *xwayland_surface = nullptr;
    wlr_scene_tree *scene_tree = nullptr;

    wl_listener associate = {};
    wl_listener dissociate = {};
    wl_listener destroy = {};
    wl_listener map = {};
    wl_listener unmap = {};
    wl_listener request_configure = {};
    wl_listener set_override_redirect = {};
};

struct BiomePopup {
    wlr_xdg_popup *xdg_popup = nullptr;
    BiomeServer *server = nullptr;
    wl_listener map = {};
    wl_listener unmap = {};
    wl_listener commit = {};
    wl_listener destroy = {};
    wl_listener reposition = {};
    // wlroots 0.19+ asserts on configure before the initial commit, so unconstraining waits for it.
    bool unconstrain_on_initial_commit = false; // via constrain_popup_to_output()
    bool has_layer_unconstrain_box = false;
    wlr_box layer_unconstrain_box = {};
};

wlr_surface *toplevel_surface(BiomeToplevel *toplevel);

// Asks the client to close itself - doesn't destroy anything directly, the
// client tears its own surface down via the normal unmap/destroy path.
void close_toplevel(BiomeToplevel *toplevel);

void toplevel_get_geometry(BiomeToplevel *toplevel, wlr_box *box);

// The full on-screen frame (position + size, in output-layout coordinates)
// including border/titlebar - what the Alt-Tab switcher's live highlight box
// outlines. Same formula set_toplevel_maximized uses for restore_box.
void toplevel_get_frame_box(BiomeToplevel *toplevel, wlr_box *box);

// False while fullscreen, for an X11 window whose _MOTIF_WM_HINTS ask for no
// decorations (queried live: parsed late), and for client-side xdg windows.
bool toplevel_decorated(const BiomeToplevel *toplevel);

// Xwayland surface with _NET_WM_WINDOW_TYPE_SPLASH: undecorated, centered on
// the cursor's output without cascade, and not focused on map.
bool toplevel_is_splash(const BiomeToplevel *toplevel);

// The BiomeToplevel for a protocol object, or null.
BiomeToplevel *toplevel_from_xdg(wlr_xdg_toplevel *xdg_toplevel);
BiomeToplevel *toplevel_from_xwayland(wlr_xwayland_surface *xsurface);

// xdg uses only the size; X11 needs x/y too.
void toplevel_set_size(BiomeToplevel *toplevel, int x, int y, int width, int height);

// Mirrors a position into the X server (X11 menus position off it); no-op for xdg.
void toplevel_sync_position(BiomeToplevel *toplevel, int x, int y);

// Moves scene_tree and refreshes foreign-toplevel output_enter/leave. Use
// instead of setting scene_tree's position directly.
void toplevel_set_position(BiomeToplevel *toplevel, int x, int y);

// Keyboard focus (plus X11 stacking). Always enters the role surface, never a
// hit-tested subsurface, which caused spurious leaves that closed popups.
void focus_toplevel(BiomeToplevel *toplevel);
void set_toplevel_focused(BiomeToplevel *toplevel, bool focused);

// Brings toplevel forward on the user's behalf (taskbar click, honored
// xdg-activation): switches to its workspace, unminimizes, focuses + raises.
void activate_toplevel(BiomeToplevel *toplevel);

// Updates the decoration, switcher entry and ext-workspace urgent state.
// Ignored (urgent = true) for the focused toplevel.
void set_toplevel_urgent(BiomeToplevel *toplevel, bool urgent);

// Unfocuses the toplevel the seat's keyboard focus is on, if any. Internal
// to the two focus-granting functions; don't call it to grant focus.
void clear_focused_toplevel(BiomeServer *server);

// Deactivates the activated toplevel and clears seat keyboard focus. Use this,
// never wlr_seat_keyboard_notify_clear_focus() directly.
void clear_keyboard_focus(BiomeServer *server);

// The only way to focus a non-toplevel surface (layer surface, popup, X11
// override-redirect): unfocuses the current toplevel first (unless surface is
// its popup), then enters bypassing grabs, which a grabbing popup needs.
void grant_keyboard_focus_to_non_toplevel(BiomeServer *server, wlr_surface *surface);

// The output's usable area (minus exclusive zones) in layout coordinates;
// the full output box if that's degenerate. nullptr: the whole layout.
wlr_box output_target_box(BiomeServer *server, wlr_output *wlr_output);

// The output overlapping box (layout coordinates) the most, or nullptr if
// none - wlroots' own rule for a scene buffer's primary output.
wlr_output *output_with_largest_overlap(BiomeServer *server, const wlr_box &box);

// A dialog centers on its parent; anything else centers on the layout,
// cascaded per open window.
void place_new_toplevel(BiomeToplevel *toplevel);

// Fills the output's usable area (default: the one it mostly overlaps).
void set_toplevel_maximized(BiomeToplevel *toplevel, bool maximized, wlr_output *output = nullptr);

// Snaps to zone on output's usable area (Top = maximize); None restores.
void set_toplevel_snapped(BiomeToplevel *toplevel, SnapZone zone, wlr_output *output = nullptr);

// Drops the snapped state without moving or resizing (the user is resizing it).
void toplevel_clear_snap(BiomeToplevel *toplevel);

// Call on each pointer motion of a titlebar drag: shows the snap preview once
// the pointer has dwelled in an edge/corner zone for a moment.
void snap_drag_motion(BiomeServer *server, double lx, double ly);

// Hides the preview and cancels any pending dwell.
void snap_preview_hide(BiomeServer *server);
// Frees the dwell timer at teardown.
void snap_finish(BiomeServer *server);

// Applies the pending preview zone to the grabbed toplevel and hides the
// preview. Call on button release before reset_cursor_mode.
void snap_drag_finish(BiomeServer *server);

// The layer a non-fullscreen toplevel's scene_tree belongs in.
wlr_scene_tree *toplevel_layer(const BiomeToplevel *toplevel);

// Moves the toplevel between layers.toplevels and layers.above (and raises it).
void set_toplevel_always_on_top(BiomeToplevel *toplevel, bool always_on_top);

// Fills the whole output, undecorated, above the layer-shell layers.
// output: the client's choice if in the layout, else the one it mostly overlaps.
void set_toplevel_fullscreen(BiomeToplevel *toplevel, bool fullscreen, wlr_output *output = nullptr);

// xdg only, between the initial commit and map: replies to the client's
// requested maximized/fullscreen state with a configure already sized for
// the output the toplevel will be placed on, so its first buffer is drawn
// at that size. Sends a plain 0x0 configure if neither is requested.
void toplevel_configure_premap(BiomeToplevel *toplevel);

// Centers the toplevel's current size on the output it overlaps the most.
void toplevel_center_on_output(BiomeToplevel *toplevel);

// Hides the toplevel and moves focus on if it had it.
void set_toplevel_minimized(BiomeToplevel *toplevel, bool minimized);

// Listeners both backends wire up directly.
void toplevel_map(wl_listener *listener, void *data);
void toplevel_unmap(wl_listener *listener, void *data);

// Shared create/destroy pair. toplevel_create allocates with new (IconImage needs its ctor);
// the caller then builds scene_tree/decoration and listeners. toplevel_destroy_common runs
// after the caller removed its listeners and deletes the toplevel.
BiomeToplevel *toplevel_create(BiomeServer *server, BiomeToplevelType type);
void toplevel_destroy_common(BiomeToplevel *toplevel);
// Drops every weak server-side reference to the toplevel (grab, hover/press, menu, switcher, last click).
void toplevel_forget(BiomeToplevel *toplevel);

// Nullable; xdg title/app_id, X11 title/WM_CLASS.
const char *toplevel_title(const BiomeToplevel *toplevel);
const char *toplevel_app_id(const BiomeToplevel *toplevel);
BiomeToplevel *toplevel_parent(BiomeToplevel *toplevel);
void toplevel_request_move(wl_listener *listener, void *data);

// Raw scene hit test; classify the result with the *_at_node functions to
// avoid walking the scene twice per event.
wlr_scene_node *scene_node_at(BiomeServer *server, double lx, double ly, double *sx, double *sy);

BiomeToplevel *desktop_toplevel_at_node(wlr_scene_node *node, wlr_surface **surface);

// Topmost surface at layout coords. Unmanaged surfaces (X11 override-
// redirect) return null with *surface still set.
BiomeToplevel *desktop_toplevel_at(
    BiomeServer *server, double lx, double ly,
    wlr_surface **surface, double *sx, double *sy);

// Call once after outputs were enabled/disabled/moved/resized/removed, or a
// layer surface changed an output's usable_area (desktop/layer_shell.cpp). Pulls
// any window (and maximize/fullscreen restore box) that no longer overlaps an
// output onto the nearest one, and re-fits maximized/fullscreen windows to
// their output's current box. Windows still on an output stay put.
void toplevels_relocate_for_layout(BiomeServer *server);

// Output removal: `begin` before the output leaves the layout remembers its windows,
// `end` after the relocation records where they landed. `restore` on a reconnect with
// the same name moves still-untouched ones back (caller then runs the settle).
void toplevels_home_begin(BiomeServer *server, wlr_output *leaving);
void toplevels_home_end(BiomeServer *server);
void toplevels_home_restore(BiomeServer *server, wlr_output *returned);
