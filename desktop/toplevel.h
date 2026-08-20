// SPDX-License-Identifier: LGPL-3.0-or-later
//
// BiomeToplevel and the toplevel-generic behavior shared between xdg-shell
// and Xwayland: geometry/lookup helpers, focus, placement, maximize/
// minimize, and the map/unmap/request_move listeners both backends wire up
// identically. xdg-shell-only and Xwayland-only signal handling (surface
// creation, commit, the backend-specific request_* signals) lives in
// desktop/xdg_shell.h and desktop/xwayland_shell.h instead.

#pragma once

#include "core/server.h"
#include "decoration/layout.h" // biome_decoration::Region
#include "decoration/renderer.h" // biome_decoration::IconImage

enum class BiomeToplevelType {
    Xdg,
    Xwayland,
};

struct BiomeToplevel {
    wl_list link = {};
    BiomeServer *server = nullptr;
    BiomeToplevelType type = BiomeToplevelType::Xdg;
    int workspace = 0;

    // Set once place_new_toplevel has given this toplevel its first real
    // on-screen position. A scene node is visible from creation at whatever
    // default position it starts at, so render_toplevel_decoration checks
    // this to avoid flashing a decoration buffer before placement runs.
    bool placed = false;

    // Set by set_toplevel_maximized. restore_box is the pre-maximize
    // visible content box (position + size), in output-layout coordinates -
    // reapplied on un-maximize.
    bool maximized = false;
    wlr_box restore_box = {};

    // Set by set_toplevel_maximized for xdg-shell toplevels only: applying
    // the scene node's new position right away would put it ahead of the
    // client's own matching commit (xdg-shell resizes asynchronously), showing
    // the old, wrong-sized buffer at the new position for a frame or more.
    // Deferred until xdg_toplevel_commit sees the buffer's size actually
    // change, same idea as process_cursor_resize's deferred edge reposition.
    // Xwayland configures x/y/width/height together, so left unset for it.
    bool maximize_reposition_pending = false;
    int maximize_pending_x = 0, maximize_pending_y = 0;
    int maximize_pending_old_width = 0, maximize_pending_old_height = 0;

    // Set by set_toplevel_minimized. No taskbar exists under Biome yet, so
    // the only way to restore a minimized window is the Alt-Tab switcher.
    bool minimized = false;

    // scene_tree is the container: its position is the window's on-screen
    // position (what move/resize/focus-raise all act on). content_tree is
    // the surface tree, a child of scene_tree offset by
    // (decoration_border_width(), decoration_titlebar_height()) so
    // decoration_buffer (also a child of scene_tree) can frame it.
    wlr_scene_tree *scene_tree = nullptr;
    wlr_scene_tree *content_tree = nullptr;
    wlr_scene_buffer *decoration_buffer = nullptr;
    bool focused = false; // drives which QSS [focused=...] state gets applied

    // Which decoration button (if any) is currently hovered/pressed - kept
    // per-toplevel so render_toplevel_decoration can pass the right state to
    // decoration/renderer.h. Region::None for neither.
    biome_decoration::Region hovered_region = biome_decoration::Region::None;
    biome_decoration::Region pressed_region = biome_decoration::Region::None;

    // Resolved once, in toplevel_map (see desktop/app_icon.h). icon.size ==
    // 0 means either not-yet-resolved or genuinely no icon found;
    // icon_resolved distinguishes the two so toplevel_map doesn't redo the
    // resolution on a toplevel that legitimately has none. No live
    // re-resolution if app_id/WM_CLASS changes after map.
    biome_decoration::IconImage icon;
    bool icon_resolved = false;

    wlr_xdg_toplevel *xdg_toplevel = nullptr;         // type == Xdg
    wlr_xwayland_surface *xwayland_surface = nullptr; // type == Xwayland

    wl_listener map = {};
    wl_listener unmap = {};
    wl_listener commit = {}; // xdg only
    // Shared: catches a title set/changed after map - Xwayland has no
    // general commit hook to catch this opportunistically like xdg-shell's
    // commit listener above does.
    wl_listener set_title = {};
    wl_listener destroy = {};
    wl_listener request_move = {};
    wl_listener request_resize = {};
    wl_listener request_maximize = {};
    wl_listener request_fullscreen = {};
    wl_listener request_minimize = {};

    // xdg only: set by server_new_xdg_toplevel_decoration when a decoration
    // object arrives before the toplevel's initial commit (the common case),
    // applied once xdg_toplevel_commit reaches that commit - see the
    // comment there for why it can't be applied immediately.
    wlr_xdg_toplevel_decoration_v1 *pending_decoration = nullptr;
    wl_listener pending_decoration_destroy = {};

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

wlr_surface *toplevel_surface(BiomeToplevel *toplevel);

// Asks the client to close itself - doesn't destroy anything directly, the
// client tears its own surface down via the normal unmap/destroy path.
void close_toplevel(BiomeToplevel *toplevel);

void toplevel_get_geometry(BiomeToplevel *toplevel, wlr_box *box);

// False for an Xwayland surface that set _MOTIF_WM_HINTS asking for no
// border/title - e.g. a GTK3 app already drawing its own CSD titlebar.
// Always true for xdg-shell toplevels, since Biome forces server-side mode
// there unconditionally and xdg-decoration has no equivalent "please don't"
// request. A live query rather than a cached flag - wlroots may not have
// parsed the property yet when a toplevel is first created.
bool toplevel_decorated(const BiomeToplevel *toplevel);

// content_tree->node.data is set to the owning BiomeToplevel for both xdg
// and Xwayland, so these can recover a BiomeToplevel from a bare protocol
// object - used for looking up a parent (transient placement) or the
// previously-focused surface without needing a wlr_surface in hand.
BiomeToplevel *toplevel_from_xdg(wlr_xdg_toplevel *xdg_toplevel);
BiomeToplevel *toplevel_from_xwayland(wlr_xwayland_surface *xsurface);

// Used during interactive resize: xdg-shell only needs the new size (the
// client acks asynchronously and the compositor owns position via the scene
// graph); Xwayland surfaces track their own absolute geometry, so x/y/width/
// height all have to be sent together.
void toplevel_set_size(BiomeToplevel *toplevel, int x, int y, int width, int height);

// Xwayland surfaces need every position change mirrored into the X server
// (X11 popups/menus position relative to their parent's known x/y).
// xdg-shell toplevels have no equivalent state.
void toplevel_sync_position(BiomeToplevel *toplevel, int x, int y);

// Keyboard focus (and, for Xwayland, the X11 stacking order that goes along
// with it) only - not pointer focus.
void focus_toplevel(BiomeToplevel *toplevel, wlr_surface *surface);
void set_toplevel_focused(BiomeToplevel *toplevel, bool focused);

// Places a newly-mapped floating toplevel. A transient window (one with a
// parent, e.g. a dialog) centers on its parent, matching xfwm4's default
// dialog placement. Otherwise it's centered on the output layout, with a
// small cascading offset per concurrently-open window so repeated launches
// don't stack exactly on top of each other.
void place_new_toplevel(BiomeToplevel *toplevel);

// Real maximize/restore: no work-area reservation yet (no panel exists
// under Biome), so this simply fills the current output.
void set_toplevel_maximized(BiomeToplevel *toplevel, bool maximized);

// Minimize just hides the toplevel and moves focus elsewhere if it was
// focused - there's no taskbar under Biome yet for the usual "click to
// restore", so the Alt-Tab switcher is the only way back for now.
void set_toplevel_minimized(BiomeToplevel *toplevel, bool minimized);

// Shared between xdg-shell and Xwayland: both wire toplevel->map/unmap to
// these directly, and both signal request_move with irrelevant (or no)
// event data so the handler is identical either way.
void toplevel_map(wl_listener *listener, void *data);
void toplevel_unmap(wl_listener *listener, void *data);
void toplevel_request_move(wl_listener *listener, void *data);

// Returns the topmost surface node in the scene at the given layout coords.
// Override-redirect Xwayland surfaces never set scene_tree->node.data, so
// clicking one yields toplevel == nullptr - pointer events still reach it
// via *surface, it just isn't managed by us.
BiomeToplevel *desktop_toplevel_at(
    BiomeServer *server, double lx, double ly,
    wlr_surface **surface, double *sx, double *sy);
