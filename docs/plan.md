# Biome Development Plan

Biome is a Wayland compositor, built on wlroots, that replaces xfwm4 as the
window manager/compositor underneath the Forest desktop shell. It is
purpose-built for Forest — no goal of being a generic, independently
configurable compositor like sway/river.

This plan was written 2026-08-13, before any code existed in this repo.

## Background

Forest currently runs as a Qt6/C++ desktop shell (panel, desktop, systray,
session manager, settings, etc.) on top of X11, with `forest-session`
exec'ing **xfwm4** as the actual window manager. Forest's own components talk
to X11 directly via XCB in several places (`library/xcbutills`, panel
plugins, the hotkeys service). See `forest/CLAUDE.md` for the full shell
architecture (plugin system, DBus `org.forest`, QSS theming via
`fstyleloader`).

An earlier assessment (`forest/docs/wayland_AI_assessment.md`) laid out two
paths: pair Forest with an existing compositor (KWin/Wayfire) and keep
Forest as a pure shell, or write Forest's own compositor to get real control
over window decorations. That doc rated writing a compositor "Very High"
effort and leaned toward the pragmatic middle ground. Biome is the deliberate
decision to take the harder path (Path B) anyway, in order to keep full
control over decorations and the desktop's look/feel long-term.

## Language & rendering decision

**Biome is written in C++,** using wlroots directly (no wrapper library).
wlroots' C headers are wrapped in `extern "C" { }` (a known, solved
compatibility issue — see swaywm/wlroots#682) via a single `wlroots.hpp`
aggregator header. Hyprland is existing proof this approach works at
full-compositor scale.

`extern "C"` alone wasn't sufficient in practice: two headers
(`wlr_scene.h`, `wlr_matrix.h`) declare parameters like
`const float color[static 4]`, which is C99-only syntax and a hard parse
error in C++ (not a linkage issue). Confirmed during Phase 0 that the
`static N` there is only a compiler hint that the caller passes at least N
elements — the parameter still decays to a plain pointer either way, so
stripping it changes neither the signature nor the ABI. `cmake/BiomeWlrootsShim.cmake`
copies just those two headers into the build dir with the hint
regex-stripped and puts that directory ahead of the system one on the
include path. No other headers needed this treatment.

Decoration rendering (title bars, borders) is handled by an isolated
`decoration/` module that uses **Qt in offscreen mode**: a `QGuiApplication`
constructed with the `offscreen` platform plugin (no real display needed,
just to initialize font/style machinery), and `QPainter` onto a `QImage`
driven synchronously by the compositor's own event loop — not by a running
Qt event loop. The resulting ARGB buffer is uploaded into the wlroots scene
graph as a texture (`wlr_scene_buffer`), the same mechanism compositors that
use cairo+pango for decorations rely on. This lets decorations reuse
Forest's existing `fstyleloader`/QSS theming instead of a separate theming
system, while keeping the compositor core itself free of a competing event
loop or GL context owner.

## wlroots version target

Target **wlroots 0.18** (what Debian Trixie ships — `0.18.2-3` — matching
the distro Forest already packages for), not the upstream-latest 0.20.
Revisit this if Trixie's packaged version moves or if a needed protocol
isn't in 0.18.

## Repo layout

```
biome/
  CMakeLists.txt
  cmake/           # wayland-scanner protocol codegen (mirrors forest/cmake/ForestDeps.cmake)
  protocol/        # xdg-shell, wlr-layer-shell, xdg-decoration, foreign-toplevel, etc.
  core/            # event loop, backend/output/input setup, scene graph, seat
  desktop/         # xdg-shell + XWayland surface management, window state, focus
  decoration/      # Qt-based offscreen title bar / border renderer
  ipc/             # DBus service (extends org.forest or new org.biome), hotkey grabbing
  main.cpp
```

## Protocols needed, mapped to Forest components

| Forest component | Wayland protocol |
|---|---|
| App windows generally | `xdg-shell` (core) |
| Existing unmodified X11 apps during migration | XWayland (built into wlroots) |
| Panel dock / desktop background | `wlr-layer-shell-unstable-v1` |
| `windowlist` panel plugin (taskbar) | `wlr-foreign-toplevel-management-unstable-v1` (check whether `ext-foreign-toplevel-list-v1` is available/preferable once on 0.18/later) |
| Title bars / borders | `xdg-decoration-unstable-v1` (negotiate SSD) |
| Screenshots | `wlr-screencopy-unstable-v1` (or `ext-image-copy-capture-v1`); PipeWire + `xdg-desktop-portal` ScreenCast is the alternative if portal-based capture is ever needed |
| Session locker / screensaver | `ext-idle-notify-v1` |
| Display settings (multi-monitor) | `wlr-output-management-unstable-v1` |
| Global hotkeys (currently `qxtglobalshortcut`/`XGrabKey`) | No Wayland equivalent exists (by design) — Biome-native DBus API, since purpose-built |
| System tray | Already DBus/StatusNotifierItem-based — no change needed |
| Cursor theme / numlock | Handled directly via wlroots' xcursor manager and keyboard state — no protocol needed |

## Phased roadmap

**Phase 0 — Skeleton & dev loop.**
CMake project, wayland-scanner protocol codegen, link wlroots 0.18 +
wayland-server. Get a `tinywl`-equivalent running: one output, black screen,
quit on keypress. Run it *nested* inside the current session (wlroots
auto-detects a nested Wayland/X11 backend) as the fast iteration loop; test
from a raw TTY via DRM/KMS only occasionally.

**Phase 1 — Minimal functional compositor.** *(done)*
`xdg-shell` surfaces, pointer/keyboard input via libinput, floating window
placement (Forest/xfwm4 is a floating WM, not tiling — match that model),
basic move/resize, XWayland enabled so existing X11 apps run unmodified
while the rest of Forest migrates. "Minimal but usable" — no polish.

`BiomeToplevel` (`core/main.cpp`) now represents both xdg-shell and Xwayland
windows behind one type tag, so focus/move/resize/placement are one code
path instead of two. Xwayland surfaces are split into managed toplevels
(`server->toplevels`, same treatment as xdg-shell) and unmanaged
override-redirect surfaces (menus/tooltips/DnD icons — positioned by their
own client, never focused unless `wlr_xwayland_or_surface_wants_focus()`
says so, never part of move/resize). New toplevels are centered on the
output layout with a small per-window cascade offset, rather than left at
whatever `(0,0)` a client's scene node defaults to.

Xwayland surfaced a third category of C-vs-C++ incompatibility beyond the
two found in Phase 0: `wlr_xwayland_surface` has a member literally named
`class` (the X11 `WM_CLASS` hint) — valid C, a reserved keyword in C++, and
not fixable by `extern "C"` since that only changes linkage, not the
parser's keyword table. `BiomeWlrootsShim.cmake`'s patching function was
generalized to take an arbitrary `sed` expression per header and now also
patches `wlr/xwayland/xwayland.h`, renaming that field to `class_` (same
offset, same ABI, different source-level name).

Verified via the same nested-X11 dev loop as Phase 0: `foot` (native
Wayland, draws its own CSD) and `xterm` (Xwayland) running side by side,
`xterm` centered with a cascade offset from `foot`, both interactive.

**Phase 2 — xfwm4 feature parity.** *(done)*
Focus-follows-click, alt-tab, workspaces (if Forest uses them), window
rules, and *simple* SSD (flat-colored focus border only, no Qt yet — matches
what sway/river do).

Click-to-focus and raise were already implicit in Phase 1's cursor-button
handler, so this phase's main additions: Alt-Tab/Alt-Shift-Tab now cycle
focus in MRU order with no live preview (matches xfwm4's
`cycle_preview=false` default in `forest/usr/share/forest/xfwm4.xml`);
4 workspaces (matching that same file's `workspace_count=4`, and the
`deskswitch` panel plugin that drives it — confirmed Forest actually uses
them before building this), switched via Ctrl-Alt-Left/Right or
Ctrl-Alt-1..4, with Ctrl-Alt-Shift-Left/Right moving the focused window
along; and one window rule — transient windows (dialogs) center on their
parent instead of the output, matching xfwm4's default dialog placement.
Note the panel's `deskswitch` plugin itself won't drive Biome's workspaces
yet — it talks XCB/EWMH directly to xfwm4, and gets replaced by
Wayland-native IPC in Phase 3.

`BiomeToplevel::scene_tree` is now a container: a plain `wlr_scene_tree`
holding both the border rects and a `content_tree` (the actual xdg/Xwayland
surface tree, offset by `kBorderWidth` inside the container). `scene_tree`
still anchors the window's on-screen position for move/resize/focus-raise,
unchanged from Phase 1; only the code paths that compute the *visible*
content box from that position (interactive resize math, Xwayland position
sync, transient-parent placement) needed the border-width term added in.
The container is created once per toplevel (not per Xwayland
associate/dissociate cycle) so the border survives an X11 window's surface
being torn down and recreated. One known rough edge, acceptable for this
phase: since Biome doesn't negotiate `xdg-decoration-unstable-v1`, CSD
clients (foot, GTK apps) still draw their own decorations *and* get
Biome's border around them — looks like a thin frame around the client's
own titlebar rather than a real double-decoration, but is a real
Phase-4-or-earlier candidate to revisit once `xdg-decoration` is wired up.

Verified in the nested-X11 dev loop: clean build, borders render (blue
focused / gray unfocused) and update on click-to-focus for both xdg (foot)
and Xwayland (xterm) toplevels, cascade placement, clean shutdown with no
orphaned processes. Alt-Tab and the workspace hotkeys were confirmed
working by the user directly (manual testing, not the agent — synthetic
key injection into the nested session kept colliding with the host window
manager's own global Alt-Tab/Ctrl-Alt-Arrow grabs).

That same manual test surfaced a real regression: Ctrl-Alt-F1..F12 (VT
switching) stopped working once Biome was running. On a real KMS/DRM
session, taking over the console puts it in graphics mode, which disables
the kernel's own VT-switch key handling — the compositor is expected to
notice Ctrl-Alt-Fn itself and hand the switch back via
`wlr_session_change_vt()`. `tinywl` (what Biome's still built on) never
wired this up. Fixed by passing `&server.session` to
`wlr_backend_autocreate()` (Phase 0/1 passed `nullptr`, discarding the
`wlr_session*` the backend creates on bare metal) and adding a
`handle_keybinding()` case for the `XF86Switch_VT_1..12` keysyms xkb
produces for Ctrl-Alt-F1..F12, calling `wlr_session_change_vt(session, vt)`.
No-ops safely on nested backends, which have no session.

**Phase 3 — Forest shell integration.**
Layer-shell for panel + desktop, foreign-toplevel-management for the
windowlist plugin, DBus hotkey service replacing `qxtglobalshortcut`,
screencopy for screenshots, idle-notify for the session locker. This is
where Forest's shell processes become Wayland-native instead of X11
clients — effectively executing the `xcbutills` replacement that
`wayland_AI_assessment.md` flagged as the biggest chunk of shell-side work.

**Phase 4 — Qt-based decorations & polish.**
Real title bars/buttons via the offscreen-Qt renderer, shared QSS theming
with the panel, output-management protocol for the display settings app,
HiDPI/scaling, damage tracking tuned for performance.

**Phase 5 — Cutover.**
New `forest-session` variant that execs Biome instead of `xfwm4`, a Wayland
session entry for the greeter, X11 path kept alive in parallel until Biome
is solid, then eventually deprecated.

## Open risks

- **Global hotkeys need a real rewrite, not a port.** Wayland has no
  global-hotkey grab primitive by design (security model), so
  `services/services-app/hotkeys` can't be mechanically translated. Worth
  prototyping early in Phase 3 since it touches both Biome and Forest.
- **Greeter/session integration** (LightDM Wayland session support, a new
  `.desktop` session entry) is Phase 5 infra work, not a blocker for early
  development, but should be scoped before Phase 5 starts.
