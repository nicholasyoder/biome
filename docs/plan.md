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
Wayland-native IPC in Phase 4 (Forest shell integration, moved back from
Phase 3 — see the 2026-08-14 note below).

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

**Phase 3 and 4 were swapped on 2026-08-14** (before either was started —
Phase 2 was the last one built). Original order had Forest shell
integration first; decorations/polish now comes first instead, so Biome
gets further along as a standalone, better-feeling compositor before any
code in `forest/` itself is touched. Everything in the new Phase 3 is
verifiable through the same nested-session dev loop as Phases 0–2, with no
Forest process involved — the first phase that actually touches Forest's
codebase is now Phase 4.

**Phase 3 — Qt-based decorations & polish.** *(window-chrome work done;
output-management/HiDPI/damage-tracking still open)*

Real Qt-rendered title bars and borders, replacing Phase 2's flat-colored
`wlr_scene_rect` border entirely. New `decoration/` module (a
`biome_decoration` static library linked into `core/`):
`theme_colors.{h,cpp}` sources title bar/border colors from Forest's actual
QSS theme — not by reusing an existing decoration stylesheet (Forest's QSS
has none; `fstyleloader` only ever styles Forest's own widget chrome) but
by reimplementing its theme-cascade resolution standalone (reading
`~/.config/Forest/Forest.conf`'s active theme, each `theme.conf`'s
`parent_themes` chain, and the resulting `forest.css` concatenation), then
applying that stylesheet to an offscreen `QWidget` tagged the same way
Forest's own code tags its `FSS-color="surface"`/`"pane"` widgets and
reading back the real QSS-engine-resolved colors — rather than a
hand-rolled CSS parser. This keeps Biome independently buildable (no
cross-repo *source* dependency on Forest, only its data files) and falls
back to Phase 2's flat colors if Forest's theme files aren't found. One
color stays Biome-native regardless: the focused-border accent, since
Forest's QSS has no accent/highlight concept to source it from.

`layout.h/cpp` defines the shared titlebar/border geometry and hit-testing
(titlebar height, border width, 8 resize regions, 3 button hotspots) used
by both the renderer and `core/main.cpp`'s pointer handling, so painted and
clickable regions can't drift apart. `renderer.h/cpp` paints the full
decoration frame (titlebar + border, transparent hole for the client's own
surface) as one `QImage`, uploaded into the scene graph as a single
`wlr_scene_buffer` via a small custom software `wlr_buffer` implementation
in `core/main.cpp` (CPU-only pixel data, `DRM_FORMAT_ARGB8888`) — replacing
`BiomeToplevel`'s four border rects with one buffer node. `switcher.h/cpp`
renders the Alt-Tab overlay (below) with the same pipeline.

`xdg-decoration-unstable-v1` is now negotiated: Biome always forces
server-side mode on any client that creates a decoration object, resolving
Phase 2's known CSD-double-decoration rough edge. One wrinkle: clients
typically create that object *before* their first surface commit, and
setting the mode right then hits wlroots' "configure scheduled for an
uninitialized xdg_surface" guard and gets silently dropped — fixed by
deferring the mode-set to `xdg_toplevel_commit`'s `initial_commit` branch
(guaranteed to run after wlroots' own initialization), tracked via a
`pending_decoration` pointer with its own destroy-listener safety net.

Border-drag interaction is new too: `decoration_toplevel_at()` (a sibling
to the existing `desktop_toplevel_at()`, using the same z-order-respecting
`wlr_scene_node_at()` lookup) resolves a click to a decoration region, and
`server_cursor_button` dispatches titlebar clicks to interactive move and
the 8 edge/corner regions to interactive resize — reusing Phase 2's
move/resize math unchanged, just triggered by a direct border click
instead of only a client's own `request_move`/`request_resize`. This also
fixed a pre-existing gap where clicking the flat Phase 2 border did
nothing at all, not even focus. Hovering the border now shows
resize-direction cursors.

Maximize, minimize, and close are all real now (previously stubs or
entirely unwired): maximize fills the current output (no work-area
reservation yet — no panel exists under Biome until Phase 4) via the
button, double-click-titlebar (which un-maximizes-then-moves if you grab
an already-maximized window's titlebar), and both platforms'
`request_maximize` events; close wires the close button to
`wlr_xdg_toplevel_send_close()`/`wlr_xwayland_surface_close()`; minimize
wires the previously-unwired `request_minimize` signals plus a button to
hiding the toplevel and moving focus elsewhere (`update_toplevel_visibility`
now combines minimize state with the existing per-workspace visibility
check, since the two hide-mechanisms have to compose). No taskbar exists
under Biome yet (Phase 4's foreign-toplevel-management work), so the
graphical Alt-Tab switcher is currently the only way to restore a
minimized window — matches xfwm4's own default of showing minimized
windows in its cycle list.

The graphical Alt-Tab switcher replaces Phase 2's invisible MRU cycling
with an actual on-screen overlay: a centered panel listing window titles
(falling back to app_id/class), the focused entry highlighted, shown on
the first Tab press and dismissed on Alt release. Kept to a text list, not
live thumbnails, matching the `cycle_preview=false` convention Phase 2
already mirrored and avoiding a live per-window texture-readback pipeline
this phase doesn't otherwise need. It reuses Phase 2's exact MRU list and
cycling logic unchanged — the only functional addition is that cycling
onto a minimized window now un-minimizes it first, so selecting it
actually brings it back.

Deliberately deferred, not forgotten: `wlr-output-management-unstable-v1`,
HiDPI/scaling, and damage-tracking tuning (all named in this phase's
original scope) weren't built this pass. A right-click titlebar
window-operations menu, drop shadows, app icons, and live QSS theme-switch
reload were scoped out from the start as later polish.

Verified via clean builds (zero warnings throughout) and repeated
nested-X11 crash/orphan smoke tests after every sub-feature, with both
`foot` (xdg-shell) and `xterm` (Xwayland) clients. Per established
preference, visual quality and drag/keyboard-driven interaction were left
to the user's own manual testing rather than the agent self-verifying via
screenshots or synthetic input. User feedback so far: the title bar
renders correctly via QSS; the focused border's blue accent is
intentionally Biome-native rather than a Phase 2 leftover, as clarified
mid-session — specific styling choices (colors, etc.) are planned for a
later session.

**Phase 4 — Forest shell integration.**
Layer-shell for panel + desktop, foreign-toplevel-management for the
windowlist plugin, DBus hotkey service replacing `qxtglobalshortcut`,
screencopy for screenshots, idle-notify for the session locker, and
wiring the display settings app to `wlr-output-management-unstable-v1`
(Biome-side protocol support for that still needs to land too, in Phase 3
or here, whichever comes first). This is where Forest's shell processes
become Wayland-native instead of X11 clients — effectively executing the
`xcbutills` replacement that `wayland_AI_assessment.md` flagged as the
biggest chunk of shell-side work, and the first phase where any `forest/`
code itself gets modified.

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
