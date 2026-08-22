# Biome Development Plan

Biome is a Wayland compositor, built on wlroots, that replaces xfwm4 as the
window manager/compositor underneath the Forest desktop shell. It is an
opinionated, fixed-policy compositor — no goal of being a generic,
user-configurable compositor like sway/river — but as of 2026-08-22 it is a
deliberate goal that Biome and Forest stay decoupled at the interface level
rather than hard-wired to each other. See "Decoupling goal" below.

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

## Decoupling goal (decided 2026-08-22)

Revisited whether Biome should be hard-wired to Forest specifically.
Decision: keep them decoupled at the interface level, even though Biome
remains an opinionated, single-policy compositor — that's a separate axis
from Forest-coupling and isn't changing; Biome isn't becoming sway/river-
configurable. Two directions:

- **Biome usable by another shell.** Achieved almost for free: most of the
  protocol surface in the table below (`xdg-shell`, `wlr-layer-shell`,
  `xdg-decoration`, `wlr-foreign-toplevel-management`, `wlr-screencopy`,
  `ext-idle-notify`, `wlr-output-management`) is standard Wayland protocol —
  any shell speaking it can use Biome regardless of what built it. Phase 3's
  decoration theme (`decoration/theme/biome-dark.qss`) is already
  self-contained and only modeled on Forest's look, not dependent on
  Forest's installed theme files at runtime — this already met the goal by
  accident before the goal was made explicit.
- **Forest usable on another compositor.** Follows from Phase 4 targeting
  the same standard protocols instead of Biome-specific escape hatches:
  Forest's shell components become Wayland-native against generic
  protocols, not against Biome internals, so any compositor implementing
  the same protocols can host Forest.

The one place real coupling would otherwise get baked in: **global
hotkeys**, since Wayland has no grab protocol by design (see Open risks
below). Decision: Biome implements the existing
`org.freedesktop.portal.GlobalShortcuts` interface (the same one GNOME/KDE
portals expose) instead of a bespoke schema, and Forest's Phase 4 hotkey
client is written against that same portal interface rather than a
Biome-specific one. Chosen for compatibility over the simpler alternative
(a from-scratch `org.biome` schema): this gives Forest a shot at working
against any other compositor that backs the same portal, and gives Biome a
shot at being usable by any shell that already speaks it.

Practical implication going forward: default to the standard
protocol/interface for anything Phase 4 needs, and treat a Biome-specific
or Forest-specific shortcut as something to justify, not the default.

## Repo layout

```
biome/
  CMakeLists.txt
  cmake/           # wayland-scanner protocol codegen (mirrors forest/cmake/ForestDeps.cmake)
  protocol/        # xdg-shell, wlr-layer-shell, xdg-decoration, foreign-toplevel, etc.
  core/            # event loop, backend/output/input setup, scene graph, seat
  desktop/         # xdg-shell + XWayland surface management, window state, focus
  decoration/      # Qt-based offscreen title bar / border renderer
  ipc/             # DBus service under org.biome (kept separate from org.forest —
                   # see Decoupling goal), implements org.freedesktop.portal.GlobalShortcuts
                   # for hotkeys, output-management wiring, etc.
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
| Global hotkeys (currently `qxtglobalshortcut`/`XGrabKey`) | No Wayland equivalent exists (by design) — Biome implements `org.freedesktop.portal.GlobalShortcuts`; Forest's hotkey client targets that same portal interface rather than a Biome-specific one (see Decoupling goal) |
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

**Phase 3.5 — Input & session completeness.** *(added 2026-08-22; all three
items done - the two clipboard-shaped ones confirmed by manual testing,
`ext-session-lock-v1` not yet manually tested)* Found by
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
- **`ext-session-lock-v1`.** *(done)* New `desktop/session_lock.{h,cpp}`
  module wiring `wlr_session_lock_manager_v1`. A single `server->lock_tree`
  scene node, raised to the top of `server->scene->tree` and enabled for the
  duration of a lock, is the one invariant the whole implementation leans
  on: `desktop_toplevel_at`/`decoration_toplevel_at` already stop their
  scene-graph hit-test at the first node under the cursor, so an opaque
  full-output `wlr_scene_rect` per output (created unconditionally at
  output-add time, so a hotplugged monitor is blanked from its first frame
  even mid-lock) makes every normal window and Biome's own decoration
  unreachable to click/hover with no bespoke lock-aware hit-testing needed.
  The only place a normal toplevel's scene node ever gets raised is
  `focus_toplevel()` (both click-to-focus and auto-focus-on-map go through
  it) - gating that one function on `server->session_locked` is what stops a
  window from being raised above, or stealing keyboard focus from, the lock
  surfaces. `handle_keybinding()` (`core/input.cpp`) swallows nothing while
  locked except VT-switch, which stays live (kernel-level session handoff,
  matches sway, not a Biome-content leak) - every other compositor keybind
  (Escape-quit, Alt-Tab, workspace-switch) falls through as an ordinary key
  event to the lock client instead.

  `session_locked` (survives a lock client crash) and `active_lock` (nulled
  the moment that lock's wl_resource is gone, crash or not) are deliberately
  two separate fields: per spec, a client dying without calling
  `unlock_and_destroy` must not unlock the session, so `session_locked` is
  only ever cleared by a real `unlock` event. Since the reject-a-second-lock
  check in `new_session_lock` tests `active_lock` (not `session_locked`), a
  replacement client can `lock()` and take over recovery after a crash -
  exactly the compositor-policy recovery path the spec names - with no
  extra code for it.

  `wlr_session_lock_v1_send_locked()` is deferred until every currently-
  enabled output has actually committed a frame since the lock began
  (tracked per-`BiomeOutput` via `pending_lock_frame`, checked in
  `output_frame()`), not sent synchronously from the `new_lock` handler -
  the spec's locked-event timing rule exists specifically to prevent a
  suspend-races-resume race, and the compositor's own blanking is enough to
  satisfy it without waiting on the client's own surface to render.
  `output_request_state()` also keeps the blank rect and any live lock
  surface's configured size in sync with a live output resolution change
  (reachable on the nested dev backends), since a stale-sized rect would be
  a real edge leak, not just cosmetic.

  **Follow-up fix, same day:** the user's manual test (real DRM/KMS session,
  swaylock) found a real leak the design above missed - a *new* window
  mapped while locked (tested via a Wayland client launched against Biome's
  socket from another VT) appeared on top of swaylock's UI and could be
  dragged around, though it correctly never got keyboard focus. Root cause:
  keeping `lock_tree` raised to the top only protects scene content that
  already existed when the lock began - `wlr_scene_tree_create()` always
  appends a *new* node as the topmost sibling regardless of history (the
  same mechanism `lock_tree` itself relies on to get on top), so anything
  mapped after the lock started re-topped itself automatically. Found two
  independent instances: (1) `update_toplevel_visibility()`
  (`desktop/workspace.cpp`, already called for every newly placed toplevel
  via `place_new_toplevel()`) didn't factor in `session_locked` at all -
  fixed by adding it to the visibility formula, plus re-running it over
  every existing toplevel on both lock and unlock in `session_lock.cpp`
  (replacing an initial unlock-focus implementation that hand-rolled "just
  focus toplevels.next" with the existing, workspace-aware
  `focus_topmost_on_active_workspace()` helper instead, avoiding a second,
  separate bug where unlocking could've focused a toplevel on a hidden
  workspace). (2) Xwayland override-redirect surfaces (X11
  popups/menus/tooltips, `desktop/xwayland_shell.cpp`'s `BiomeUnmanaged`) are
  a completely separate path with no `BiomeToplevel` at all, invisible to
  fix (1) - and its map handler unconditionally raised to top *and grabbed
  keyboard focus* with no lock check whatsoever, a worse gap than what the
  user actually observed. Fixed by disabling the surface's scene node
  outright (not just skipping the raise) when `session_locked`, which also
  skips the focus grab. xdg-shell popups were checked and don't have this
  problem - `wlr_scene_xdg_surface_create()` parents them under their
  parent's own existing content_tree node, not the scene root, so they're
  transitively hidden whenever their parent toplevel is. Full clean
  incremental rebuild, zero warnings. Still needs a retest by the user to
  confirm the fix.

  **Research + small polish, same day:** user asked for a sanity check
  against how other wlroots compositors actually implement this, worried the
  approach above was "hacky." Compared against sway's real `sway/lock.c`
  (fetched from `swaywm/sway` on GitHub - sway is the closer reference since
  it's built on `wlr_scene` like Biome, unlike Hyprland which has its own
  renderer) and the locally-checked-out Hyprland source
  (`/home/nicholas/ForestProject/misc/Hyprland/src/{managers/SessionLockManager,protocols/SessionLock}.{cpp,hpp}`).
  Conclusion: the core strategy (an opaque layer occluding everything else,
  gated on a locked flag) matches both exactly - not a weird approach. Two
  small, deliberate deviations, both fine, documented in a note added to
  Phase 4 below regarding the real structural difference (no persistent
  per-output layer stack yet). Borrowed one small polish from sway's own
  precedent: `handle_lock_destroy` now tints every output's `lock_rect` red
  (`kSessionLockAbandonedColor`, `desktop/session_lock.h`) when a lock is
  abandoned (crashed without `unlock_and_destroy`) rather than staying the
  same black as a normal in-progress lock, so a permanently-stuck-locked
  screen is visually distinguishable - matches sway's own convention of
  recoloring the background rect on `handle_abandon`. Reset back to black at
  the start of every new lock (`handle_new_session_lock`), covering both a
  replacement client recovering from a crash and the ordinary case (a
  harmless no-op there). `output.cpp`'s original locally-scoped color
  constant was promoted to `session_lock.h`'s new `kSessionLockColor` so
  both files share the same literal instead of duplicating it. Full clean
  incremental rebuild, zero warnings.

  Deliberately out of scope this pass: restricting the session-lock global
  to a privileged client (Biome has no client-allowlist mechanism anywhere
  yet; the global is exposed unrestricted, same trust model as every other
  global Biome currently exposes), cancelling an in-flight drag if a lock
  starts mid-drag, and `idle-notify`/auto-lock-on-idle (still Phase 4 as
  scoped below - this only makes a manually-triggered lock actually secure).
  Full clean rebuild, zero warnings. **Not yet manually tested interactively
  by the user** - per [[feedback-manual-interactive-testing]]; needs a lock
  client to drive it (Forest doesn't have one yet, and no throwaway test
  client was written this pass).

**Phase 4 — Forest shell integration.**
Layer-shell for panel + desktop (bundled with `xdg-output-unstable-v1`,
since layer-shell clients commonly query it for per-output name/logical
geometry), foreign-toplevel-management for the windowlist plugin, a DBus
hotkey service implementing `org.freedesktop.portal.GlobalShortcuts`
(replacing `qxtglobalshortcut` — see Decoupling goal), screencopy for screenshots,
idle-notify for the session locker, and wiring the display settings app to
`wlr-output-management-unstable-v1` (Biome-side protocol support for that
still needs to land too, in Phase 3 or here, whichever comes first). This
is where Forest's shell processes become Wayland-native instead of X11
clients — effectively executing the `xcbutills` replacement that
`wayland_AI_assessment.md` flagged as the biggest chunk of shell-side work,
and the first phase where any `forest/` code itself gets modified.

**Follow-up noted 2026-08-22, don't lose this:** when layer-shell lands
here, build a real persistent per-output scene-layer stack (background /
bottom / normal toplevels / top / overlay / session-lock, each a
`wlr_scene_tree` created once at output-init in that fixed order - mirrors
sway's own `sway_output::layers` in `include/sway/output.h` /
`sway/tree/output.c`, and is the layer-shell protocol's own layer model
anyway, so this isn't extra scope, it's building the thing layer-shell
needs regardless). Once that exists, make `session_lock`'s tree the
permanently-last layer instead of something raised at lock time - z-order
becomes structural (nothing can ever be created above it, by construction)
instead of a rule every content-creation site has to separately remember to
respect. At that point, delete the `session_locked` checks added in Phase
3.5's session-lock work: `update_toplevel_visibility()`
(`desktop/workspace.cpp`), the override-redirect map handler
(`desktop/xwayland_shell.cpp`), and `session_lock.cpp`'s two lock/unlock
visibility sweeps - all of it becomes unnecessary once new content simply
can't be inserted above the lock layer in the first place. Confirmed via
research (see Phase 3.5's entry above) that the current runtime-check
approach is exactly what caused a real bug this session (a newly-mapped
window escaping the lock) - a structural layer stack is not just cleaner,
it's the thing that would have made that bug impossible rather than merely
fixed.

**Phase 5 — Cutover.**
New `forest-session` variant that execs Biome instead of `xfwm4`, a Wayland
session entry for the greeter, X11 path kept alive in parallel until Biome
is solid, then eventually deprecated.

## Open risks

- **Global hotkeys need a real rewrite, not a port.** Wayland has no
  global-hotkey grab primitive by design (security model), so
  `services/services-app/hotkeys` can't be mechanically translated. Decided
  2026-08-22 (see Decoupling goal) to implement this as
  `org.freedesktop.portal.GlobalShortcuts` rather than a bespoke schema, for
  compatibility with other compositors/shells — worth prototyping early in
  Phase 4 since it touches both Biome and Forest and the portal's exact
  semantics (binding registration, conflict handling) aren't yet validated
  against Biome's architecture.
- **Greeter/session integration** (LightDM Wayland session support, a new
  `.desktop` session entry) is Phase 5 infra work, not a blocker for early
  development, but should be scoped before Phase 5 starts.
