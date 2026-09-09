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

    // Set by set_toplevel_fullscreen. fullscreen_restore_box is the
    // pre-fullscreen visible content box (position + size), in
    // output-layout coordinates - reapplied on un-fullscreen. Independent of
    // maximized/restore_box above: fullscreening a maximized window and then
    // un-fullscreening it restores the maximized fill, not the floating
    // size, since toplevel->maximized itself is never touched by
    // set_toplevel_fullscreen. While true, scene_tree is also reparented
    // into BiomeServer::layers.fullscreen (above every layer-shell layer,
    // still below layers.session_lock) instead of its usual
    // layers.toplevels - see that field's declaration in core/server.h.
    bool fullscreen = false;
    wlr_box fullscreen_restore_box = {};

    // Set by set_toplevel_maximized/set_toplevel_fullscreen for xdg-shell
    // toplevels only: applying the scene node's new position right away
    // would put it ahead of the client's own matching commit (xdg-shell
    // resizes asynchronously), showing the old, wrong-sized buffer at the
    // new position for a frame or more. Deferred until xdg_toplevel_commit
    // sees this request's configure serial come back acked, same idea as
    // process_cursor_resize's deferred edge reposition. Tracking the serial
    // rather than waiting for the buffer size to change matters because the
    // requested size can legitimately equal the size the client already
    // has (e.g. maximizing a window whose remembered size from a
    // differently-sized output happens to match this output's maximized
    // size) - the client is not required to resize in that case, but it
    // still must ack the configure. Xwayland configures x/y/width/height
    // together, so left unset for it. Shared between the two callers
    // (whichever one fires last wins, which is fine - only one resize can
    // be in flight at a time since both are synchronous calls into this
    // same struct).
    bool reposition_pending = false;
    int reposition_pending_x = 0, reposition_pending_y = 0;
    uint32_t reposition_pending_serial = 0;

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

    // Biome's own per-window title bar/border, as a real QSS-styled Qt
    // widget tree - owned, one instance per toplevel (not a process-wide
    // shared instance), so its geometry/paint state always reflects this
    // window and never leaks a stale render/hit-test from some other
    // toplevel's last call. Constructed in create_toplevel_decoration(),
    // destroyed in destroy_toplevel_decoration() (both desktop/
    // decoration_bridge.h) alongside scene_tree - see decoration/theme.h's
    // create_decoration_frame().
    biome_decoration::DecorationFrame *decoration_frame = nullptr;

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

    // State last handed to render_toplevel_decoration(), so it can skip its
    // expensive full-window QImage render when nothing changed (e.g. a
    // plain content-only commit during scrolling). last_decoration_title is
    // a borrowed pointer, not owned: BiomeToplevel is calloc()'d/free()'d,
    // so a std::string member would never construct. Pointer identity is
    // enough since a real title change always re-renders directly.
    bool decoration_rendered_once = false;
    int last_decoration_width = -1;
    int last_decoration_height = -1;
    bool last_decoration_focused = false;
    bool last_decoration_maximized = false;
    const char *last_decoration_title = nullptr;
    const uint8_t *last_decoration_icon_data = nullptr;
    biome_decoration::Region last_decoration_hovered = biome_decoration::Region::None;
    biome_decoration::Region last_decoration_pressed = biome_decoration::Region::None;

    // wlr-foreign-toplevel-management-unstable-v1 (desktop/foreign_toplevel.h) -
    // created in toplevel_map, destroyed in toplevel_unmap. Null between
    // those (or if the manager global failed to create).
    BiomeForeignToplevel *foreign_toplevel = nullptr;

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

    // xdg only: true once decoration negotiation (either protocol) has
    // settled on client-side - see toplevel_decorated. Explicitly set to
    // true in xdg_toplevel_commit's initial_commit branch for a client that
    // creates neither a zxdg_toplevel_decoration_v1 nor a KDE
    // org_kde_kwin_server_decoration object at all - both protocols specify
    // that absence of a decoration object means client-side decorated
    // (verified against sway's handle_map, which does the same: `csd = !deco
    // || ...`), so Biome must not draw its own frame over such a surface.
    bool xdg_client_side_decorated = false;

    // xdg only: the client's decoration negotiation object, if any - kept
    // for its whole lifetime (not just up to the initial commit) so a
    // request_mode fired after the window is already mapped is still
    // honored. nullptr if the client hasn't created one (or it was
    // destroyed) - see server_new_xdg_toplevel_decoration.
    wlr_xdg_toplevel_decoration_v1 *decoration = nullptr;
    wl_listener decoration_destroy = {};
    wl_listener decoration_request_mode = {};

    // xdg only: same idea as decoration/decoration_destroy/
    // decoration_request_mode above, but for a client using the older KDE
    // protocol instead (GTK3, which never adopted xdg-decoration) - see
    // server_new_kde_decoration. A toplevel only ever uses one of the two
    // protocols in practice, so both write the same
    // xdg_client_side_decorated flag.
    wlr_server_decoration *kde_decoration = nullptr;
    wl_listener kde_decoration_destroy = {};
    wl_listener kde_decoration_mode = {};

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
    BiomeServer *server = nullptr;
    wl_listener map = {};
    wl_listener unmap = {};
    wl_listener commit = {};
    wl_listener destroy = {};
    wl_listener reposition = {};
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

// Always false while toplevel->fullscreen is set - a fullscreen window
// fills the whole output with no border/titlebar regardless of what it
// would otherwise negotiate, same convention as every other compositor.
// Otherwise: false for an Xwayland surface that set _MOTIF_WM_HINTS asking
// for no border/title - e.g. a GTK3 app already drawing its own CSD
// titlebar. For an xdg-shell toplevel, false once either negotiation
// protocol (xdg-decoration or the legacy KDE one) settled on client-side, or
// once neither protocol was negotiated at all - see
// xdg_client_side_decorated. A live query rather than a cached flag on the
// Xwayland side - wlroots may not have parsed the property yet when a
// toplevel is first created.
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
// with it) only - not pointer focus. Always enters toplevel's own canonical
// role surface (toplevel_surface(toplevel)), never a caller's hit-tested
// surface (which can be a subsurface, e.g. a Chromium extension popup) -
// entering that instead caused spurious keyboard leaves that closed such
// popups on click.
void focus_toplevel(BiomeToplevel *toplevel);
void set_toplevel_focused(BiomeToplevel *toplevel, bool focused);

// If a toplevel currently holds keyboard focus (per the seat's real
// wlr_surface, not any cached bookkeeping), unfocuses it - the same
// "outgoing toplevel" half of what focus_toplevel() does before handing
// focus to another toplevel. Used internally by focus_toplevel() and by
// grant_keyboard_focus_to_non_toplevel() below - not meant to be called
// directly by anything granting focus itself; see that function instead.
void clear_focused_toplevel(BiomeServer *server);

// The single chokepoint for handing keyboard focus to any surface that
// *isn't* a BiomeToplevel: a keyboard-interactive layer-shell surface (the
// panel), an xdg_popup (any of Forest's panel/panel-library popup/popupmenu
// widgets - context menus, the main menu, tooltips), an Xwayland
// override-redirect surface, or a plain click on one of those. Always calls
// clear_focused_toplevel() first, then grants via the grab-bypassing
// wlr_seat_keyboard_enter() (safe and equivalent to the grab-aware
// wlr_seat_keyboard_notify_enter() whenever no seat keyboard grab is
// active, and necessary when `surface` is an xdg_popup that installed its
// own grab - see desktop/xdg_shell.cpp's xdg_popup_map for the full
// explanation of why notify_enter() would silently no-op there).
//
// Exists because call sites granting focus this way independently have to
// remember to unfocus whatever toplevel currently holds focus first - easy
// to get wrong per-callsite (an ordinary click on the panel is the everyday
// trigger). Routing every such site through this one function makes it
// structurally impossible to skip: nothing outside toplevel.cpp calls
// wlr_seat_keyboard_(notify_)enter()/wlr_seat_keyboard_enter() directly for
// a non-toplevel surface.
void grant_keyboard_focus_to_non_toplevel(BiomeServer *server, wlr_surface *surface);

// Global-coordinate box a window should stay within on this output -
// wlr_output's usable_area (desktop/layer_shell.cpp's arrange_layers(),
// shrunk by any exclusive-zone layer surface) translated from its
// output-local coordinates into the same global layout space
// wlr_output_layout_get_box() itself uses. Falls back to the full output
// box if wlr_output can't be resolved to a BiomeOutput, or if usable_area
// is degenerate (e.g. a pathological exclusive-zone claim consuming the
// whole output) - never returns something that would make a window
// disappear entirely. Pass nullptr for wlr_output to get the whole
// output-layout's combined extents instead of a single output's.
wlr_box output_target_box(BiomeServer *server, wlr_output *wlr_output);

// Places a newly-mapped floating toplevel. A transient window (one with a
// parent, e.g. a dialog) centers on its parent, matching xfwm4's default
// dialog placement. Otherwise it's centered on the output layout, with a
// small cascading offset per concurrently-open window so repeated launches
// don't stack exactly on top of each other.
void place_new_toplevel(BiomeToplevel *toplevel);

// Real maximize/restore: no work-area reservation yet (no panel exists
// under Biome), so this simply fills the current output.
void set_toplevel_maximized(BiomeToplevel *toplevel, bool maximized);

// Real fullscreen/restore: fills the *entire* current output (unlike
// set_toplevel_maximized, not just its usable area - a fullscreen window
// covers a panel/dock rather than reserving space around it), with the
// decorated frame's border/titlebar forced off for the duration (see
// toplevel_decorated) and the scene node raised above every layer-shell
// layer for the same reason (see BiomeServer::layers.fullscreen).
// Independent of maximized state - see fullscreen_restore_box's declaration.
void set_toplevel_fullscreen(BiomeToplevel *toplevel, bool fullscreen);

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

// Raw scene-graph hit test, shared by desktop_toplevel_at and
// decoration_toplevel_at (desktop/decoration_bridge.h) - a caller that needs
// both classifications for the same point (core/cursor.cpp's motion handler)
// does this walk once and feeds the result to desktop_toplevel_at_node/
// decoration_toplevel_at_node, instead of hit-testing the scene twice per
// event.
wlr_scene_node *scene_node_at(BiomeServer *server, double lx, double ly, double *sx, double *sy);

// Classifies an already-found scene node (see scene_node_at) the same way
// desktop_toplevel_at does - split out so a caller holding one hit-test
// result can reuse it instead of re-querying the scene.
BiomeToplevel *desktop_toplevel_at_node(wlr_scene_node *node, wlr_surface **surface);

// Returns the topmost surface node in the scene at the given layout coords.
// Override-redirect Xwayland surfaces never set scene_tree->node.data, so
// clicking one yields toplevel == nullptr - pointer events still reach it
// via *surface, it just isn't managed by us.
BiomeToplevel *desktop_toplevel_at(
    BiomeServer *server, double lx, double ly,
    wlr_surface **surface, double *sx, double *sy);
