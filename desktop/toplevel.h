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

enum class BiomeToplevelType {
    Xdg,
    Xwayland,
};

struct BiomeToplevel {
    wl_list link = {};
    BiomeServer *server = nullptr;
    BiomeToplevelType type = BiomeToplevelType::Xdg;
    int workspace = 0;

    // Set by set_toplevel_maximized. restore_box is the pre-maximize
    // visible content box (position + size), in output-layout coordinates -
    // reapplied on un-maximize.
    bool maximized = false;
    wlr_box restore_box = {};

    // Set by set_toplevel_minimized. No taskbar exists under Biome yet
    // (Phase 4), so the only way to restore a minimized window right now is
    // the graphical Alt-Tab switcher.
    bool minimized = false;

    // scene_tree is the container: its position is the window's on-screen
    // position (what move/resize/focus-raise all act on). content_tree is
    // the actual surface tree, a child of scene_tree offset by
    // (decoration_border_width(), decoration_titlebar_height()) so
    // decoration_buffer (also a child of scene_tree, painted by
    // desktop/decoration_bridge.h) can frame it.
    wlr_scene_tree *scene_tree = nullptr;
    wlr_scene_tree *content_tree = nullptr;
    wlr_scene_buffer *decoration_buffer = nullptr;
    bool focused = false; // drives which QSS [focused=...] state gets applied

    // Which decoration button (if any) is currently hovered/pressed - kept
    // per-toplevel so render_toplevel_decoration can pass the right state to
    // decoration/renderer.h. Region::None for neither.
    biome_decoration::Region hovered_region = biome_decoration::Region::None;
    biome_decoration::Region pressed_region = biome_decoration::Region::None;

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
    wl_listener request_minimize = {};

    // xdg only: set by server_new_xdg_toplevel_decoration when a decoration
    // object arrives before the toplevel's initial commit (the common
    // case), applied once xdg_toplevel_commit reaches that initial commit -
    // see the comment there for why it can't be applied immediately.
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

// Asks the client to close itself (the same request a client's own close
// button/Alt+F4/etc. would send) - doesn't destroy anything directly, the
// client tears its own surface down via the normal unmap/destroy path.
void close_toplevel(BiomeToplevel *toplevel);

void toplevel_get_geometry(BiomeToplevel *toplevel, wlr_box *box);

// content_tree->node.data is set to the owning BiomeToplevel for both xdg
// and Xwayland (mirroring base->data / xsurface->data), so these can
// recover a BiomeToplevel from a bare protocol object - used for looking up
// a parent (transient placement) or the previously-focused surface (border
// color) without needing a wlr_surface in hand.
BiomeToplevel *toplevel_from_xdg(wlr_xdg_toplevel *xdg_toplevel);
BiomeToplevel *toplevel_from_xwayland(wlr_xwayland_surface *xsurface);

// Used during interactive resize: xdg-shell only needs the new size (the
// client acks asynchronously and the compositor owns position via the scene
// graph); Xwayland surfaces track their own absolute geometry, so x/y/width/
// height all have to be sent together.
void toplevel_set_size(BiomeToplevel *toplevel, int x, int y, int width, int height);

// Xwayland surfaces need to be told about every position change (X11 popups
// and menus position themselves relative to their parent's known x/y), so
// every move has to be mirrored into the X server. xdg-shell toplevels have
// no equivalent state - positioning them is purely a scene graph concern.
void toplevel_sync_position(BiomeToplevel *toplevel, int x, int y);

// Keyboard focus (and, for Xwayland, the X11 stacking order that goes along
// with it) only - not pointer focus.
void focus_toplevel(BiomeToplevel *toplevel, wlr_surface *surface);
void set_toplevel_focused(BiomeToplevel *toplevel, bool focused);

// Places a newly-mapped floating toplevel. Window rule: a transient window
// (one with a parent, e.g. a dialog) centers on its parent, matching
// xfwm4's default dialog placement. Otherwise it's centered on the output
// layout, with a small cascading offset per concurrently-open window so
// repeated launches don't stack exactly on top of each other - xfwm4's
// default (non-tiling) placement, not anything protocol-driven.
void place_new_toplevel(BiomeToplevel *toplevel);

// Real maximize/restore: no work-area reservation yet (no panel exists
// under Biome until Phase 4), so this simply fills the current output.
void set_toplevel_maximized(BiomeToplevel *toplevel, bool maximized);

// Minimize just hides the toplevel (update_toplevel_visibility) and moves
// focus elsewhere if it was focused - there's no taskbar under Biome yet
// (Phase 4's foreign-toplevel-management work) for the usual "click to
// restore", so the graphical Alt-Tab switcher is the only way back for now.
void set_toplevel_minimized(BiomeToplevel *toplevel, bool minimized);

// Shared between xdg-shell and Xwayland: both wire toplevel->map/unmap to
// these directly, and both signal request_move with irrelevant (or no)
// event data so the handler is identical either way.
void toplevel_map(wl_listener *listener, void *data);
void toplevel_unmap(wl_listener *listener, void *data);
void toplevel_request_move(wl_listener *listener, void *data);

// Returns the topmost node in the scene at the given layout coords. Only
// cares about surface nodes, as it's specifically looking for a surface in
// the surface tree of a BiomeToplevel. Override-redirect Xwayland surfaces
// never set scene_tree->node.data, so clicking one yields toplevel ==
// nullptr - pointer events still reach it via *surface, it just isn't
// managed by us.
BiomeToplevel *desktop_toplevel_at(
    BiomeServer *server, double lx, double ly,
    wlr_surface **surface, double *sx, double *sy);
