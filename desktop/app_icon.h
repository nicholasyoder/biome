// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Resolves a window's icon for decoration/'s titlebar and Alt-Tab switcher.
// Bare xdg-shell carries no icon data at all, so native Wayland clients are
// resolved the way every SSD-drawing shell resolves them: app_id -> a
// matching .desktop file -> its Icon= key -> the XDG icon-theme cascade
// (QIcon::fromTheme(), already linked via biome_decoration's Qt6::Gui - no
// new dependency for this path). Xwayland clients supply their own icon
// directly via the legacy _NET_WM_ICON X11 property instead, falling back
// to the same desktop-file lookup keyed on WM_CLASS when a client doesn't
// set one.
//
// Both resolvers rasterize to one fixed canonical size and cache their
// result in-process, keyed by app_id/wm_class - resolution (a directory
// scan/parse, or an X11 round trip) is too expensive to redo on every
// render. Callers should resolve once (desktop/toplevel.cpp's toplevel_map)
// and cache the result on the BiomeToplevel itself, not call these from the
// render hot path.

#pragma once

#include "decoration/renderer.h" // IconImage

#include <xcb/xcb_ewmh.h>

#include <string>

// Picks and applies (via QIcon::setThemeName()) which installed icon theme
// resolve_app_id_icon()/resolve_xwayland_icon() below search. Must be called
// once, early at startup (core/main.cpp, right alongside
// load_decoration_theme()) before either of those - Qt has no ambient way
// to discover this itself here the way it would inside a real desktop
// session (a running platform-theme plugin like qt6ct normally supplies
// it), since Biome's own QApplication is forced onto the offscreen platform
// and Biome's real target environment is a bare TTY with no session
// present at all. See desktop/app_icon.cpp for the source-of-truth search
// order (qt6ct.conf, then GTK3 settings.ini, then "hicolor").
void init_icon_theme();

// app_id is spec-recommended to equal its .desktop file's basename; that
// exact match is tried first (no directory scan) before falling back to a
// scan matching StartupWMClass=. Returns an empty IconImage (size 0) if
// nothing resolves - callers hide the icon slot in that case.
biome_decoration::IconImage resolve_app_id_icon(const std::string &app_id);

// Tries the client-supplied _NET_WM_ICON X11 property first (ewmh must
// already have had xcb_ewmh_init_atoms()/_replies() run on it - see
// desktop/xwayland_shell.cpp's xwayland_ready handler; pass nullptr to skip
// straight to the fallback, e.g. if that init failed), falling back to the
// same desktop-file lookup as resolve_app_id_icon() above, keyed on
// wm_class instead of app_id, if the client didn't set one.
biome_decoration::IconImage resolve_xwayland_icon(
    xcb_ewmh_connection_t *ewmh, xcb_window_t window, const std::string &wm_class);
