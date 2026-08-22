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
while the rest of Forest migrates. Xwayland and xdg-shell windows share one
`BiomeToplevel` type so focus/move/resize/placement is one code path.
New toplevels are centered with a per-window cascade offset. "Minimal but
usable" — no polish.

**Phase 2 — xfwm4 feature parity.** *(done)*
Click-to-focus/raise, Alt-Tab/Alt-Shift-Tab MRU cycling (no live preview,
matching xfwm4's default), 4 workspaces switched via Ctrl-Alt-Left/Right or
Ctrl-Alt-1..4 (Ctrl-Alt-Shift to move the focused window along), transient
dialogs center on their parent, and simple flat-colored SSD borders
(blue focused / gray unfocused, no Qt yet). Ctrl-Alt-F1..F12 VT switching
also wired up for bare-metal sessions (the compositor has to hand this back
itself once it owns the console). Forest's `deskswitch` panel plugin still
talks to xfwm4 directly and won't drive Biome's workspaces until Phase 4's
Wayland-native IPC.

**Phase 3 and 4 were swapped on 2026-08-14** (before either was started):
decorations/polish now comes before Forest shell integration, so Biome gets
further along as a standalone compositor before any `forest/` code is
touched.

**Phase 3 — Qt-based decorations & polish.** *(window-chrome work and a
static per-output config file both done; the `wlr-output-management-unstable-v1`
protocol itself, HiDPI/scaling, and damage-tracking still open)*

Real Qt-rendered title bars and borders replace Phase 2's flat border. A
new `decoration/` module (`biome_decoration` static lib) renders a
persistent Qt widget tree (`DecorationFrame`/`DecorationButton`/
`DecorationBorder`) offscreen into a `QImage`, uploaded to the scene graph
as a `wlr_scene_buffer`. Styling comes from a self-contained theme embedded
in Biome itself (`decoration/theme/biome-dark.qss`, modeled on Forest's
dark+rounded theme rather than depending on Forest's installed files) —
real QSS border/radius/padding/`:hover`/`:pressed` states, driven by actual
pointer input. Window icons (resolved from a client's app_id/WM_CLASS via
its `.desktop` file, falling back to `_NET_WM_ICON` for Xwayland) appear in
both the titlebar and the Alt-Tab switcher. Titlebar/border dragging does
interactive move/resize; maximize, minimize, and close are fully wired; the
Alt-Tab switcher is now a real on-screen overlay (text list, no live
thumbnails) instead of invisible cycling.

Decoration mode is negotiated, not forced. Both `xdg-decoration-unstable-v1`
and the legacy `org_kde_kwin_server_decoration` protocol (which GTK3 clients,
including Firefox, use instead) are honored per whatever mode a client
actually requests — a deliberate pivot away from this phase's original
always-server-side plan, made after that forced-SSD default double-decorated
Chromium/Electron apps and other browsers with genuine, conditional CSD. A
client that negotiates neither protocol at all is treated as client-side
decorated, per both protocols' own spec convention for "no decoration object
was ever created." This also covers libadwaita/GNOME HeaderBar apps (e.g.
`org.gnome.baobab`) that never send a decoration request at all, due to an
upstream GTK4 bug in `gdk_wayland_toplevel_set_decorated()` that silently
drops the request for exactly the HeaderBar case.

A monitor's mode/scale/position/rotation can be pinned via a static,
hand-edited `~/.config/Forest/Biome.conf` (`QSettings("Forest", "Biome")` —
see `core/output_config.h`), read once at startup. This is *not* the
`wlr-output-management-unstable-v1` Wayland protocol listed in the table
above — that protocol is what would let a *running* display-settings client
change output configuration live, and it's still unimplemented; it's still
needed for Phase 4's display-settings app integration.

Deliberately deferred: `wlr-output-management-unstable-v1`, HiDPI/scaling,
damage-tracking tuning, a window-operations menu, drop shadows, and live
theme-switch reload.

Per established preference, visual/interactive verification (drag, keyboard
cycling, hover/press states) is left to the user's own manual testing in
the nested dev loop rather than agent-driven screenshots or synthetic
input.

**Phase 3.5 — Input & session completeness.** *(added 2026-08-22; the two
clipboard-shaped items are done, `ext-session-lock-v1` still open)* Found by
auditing the codebase for gaps a basic usable desktop needs, ahead of
starting Phase 4 — none require touching `forest/`, so they belong before
the phase that does:

- **Drag-and-drop.** *(done)* `core/input.cpp` now wires the seat's
  `request_start_drag`/`start_drag` signals (validating the request's
  serial via `wlr_seat_validate_pointer_grab_serial` before calling
  `wlr_seat_start_pointer_drag`); `core/cursor.cpp`'s `drag_icon_create()`
  uses wlroots' `wlr_scene_drag_icon_create()` scene helper to show the drag
  icon and keeps it positioned on the cursor via a per-motion reposition
  call in `process_cursor_motion`. Touch drags are out of scope (Biome has
  no touch input support anywhere). Landed alongside two related latent-bug
  fixes in `core/cursor.cpp` that DnD made load-bearing: the cursor's
  leaves-all-surfaces path now calls the grab-respecting
  `wlr_seat_pointer_notify_clear_focus()` instead of the raw
  `wlr_seat_pointer_clear_focus()` (needed so a drag's drop target actually
  gets `wl_data_device.leave`), and decoration press-handling
  (focus-on-click, double-click-to-maximize) is now suppressed while
  `server->seat->drag` is non-null, so a second button press mid-drag can't
  refocus or maximize a window out from under it. Confirmed working by the
  user's own manual interactive testing.
- **`primary-selection-unstable-v1`.** *(done)* X11-style select-to-copy +
  middle-click-to-paste. New `wlr_primary_selection_v1_device_manager` in
  `core/main.cpp` plus a `request_set_primary_selection` listener in
  `core/input.cpp` mirroring the existing clipboard selection handler.
  Xwayland bridging needed no extra code — confirmed from wlroots' own
  `xwayland/xwm.c` source that its `PRIMARY`-atom bridging already rides the
  same `wlr_xwayland_set_seat()` call the regular clipboard path uses.
  Confirmed working by the user's own manual interactive testing.
- **`ext-session-lock-v1` is unimplemented.** Phase 4's `idle-notify` only
  reports *when* the session goes idle — it grants no secure lock surface,
  so nothing stops another client from drawing over or under a fake lock
  screen. Needed before Forest's session locker can be trusted. Header
  already present in the linked wlroots 0.18 (`wlr_session_lock_v1.h`).

**Phase 4 — Forest shell integration.**
Layer-shell for panel + desktop (bundled with `xdg-output-unstable-v1`,
since layer-shell clients commonly query it for per-output name/logical
geometry), foreign-toplevel-management for the windowlist plugin, DBus
hotkey service replacing `qxtglobalshortcut`, screencopy for screenshots,
idle-notify for the session locker, and wiring the display settings app to
`wlr-output-management-unstable-v1` (Biome-side protocol support for that
still needs to land too, in Phase 3 or here, whichever comes first). This
is where Forest's shell processes become Wayland-native instead of X11
clients — effectively executing the `xcbutills` replacement that
`wayland_AI_assessment.md` flagged as the biggest chunk of shell-side work,
and the first phase where any `forest/` code itself gets modified.

**Phase 5 — Cutover.**
New `forest-session` variant that execs Biome instead of `xfwm4`, a Wayland
session entry for the greeter, X11 path kept alive in parallel until Biome
is solid, then eventually deprecated.

## Open risks

- **Global hotkeys need a real rewrite, not a port.** Wayland has no
  global-hotkey grab primitive by design (security model), so
  `services/services-app/hotkeys` can't be mechanically translated. Worth
  prototyping early in Phase 4 since it touches both Biome and Forest.
- **Greeter/session integration** (LightDM Wayland session support, a new
  `.desktop` session entry) is Phase 5 infra work, not a blocker for early
  development, but should be scoped before Phase 5 starts.
