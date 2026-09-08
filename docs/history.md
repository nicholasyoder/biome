# Biome Development History

**Archived 2026-09-07.** This was `docs/plan.md`, the original roadmap
written before any code existed. It grew phase-by-phase during development
into a mix of forward-looking roadmap and after-the-fact narrative/rationale,
which made it hard to use as a living roadmap. It's kept here, frozen, as the
record of *why* Phases 0–5 were built the way they were — several sections
are still cited by in-tree comments for load-bearing design rationale (the
Decoupling goal, the wlroots-version pin, session-lock invariants, etc.).

**For current status and planned work, see [`docs/roadmap.md`](roadmap.md)
instead.** Nothing below this point is maintained going forward; treat it as
a historical document, not a source of current truth about open work.

---

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

An earlier internal assessment weighed two paths: pair Forest with an
existing compositor (KWin/Wayfire) and keep Forest as a pure shell, or write
Forest's own compositor to get real control over window decorations. That
assessment rated writing a compositor "Very High" effort and leaned toward
the pragmatic middle ground. Biome is the deliberate decision to take the
harder path (Path B) anyway, in order to keep full control over decorations
and the desktop's look/feel long-term.

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
| `deskswitch` panel plugin (workspaces) | Hybrid, decided 2026-09-05: `ext-workspace-v1` for switching/listing/active-highlight, plus a narrow `org.biome.Workspaces` DBus interface for toplevel↔workspace linkage (no standard protocol covers that) |
| Title bars / borders | `xdg-decoration-unstable-v1` (negotiate SSD) |
| Screenshots *(Phase 6 — net-new, not a port)* | `wlr-screencopy-unstable-v1` (or `ext-image-copy-capture-v1`); PipeWire + `xdg-desktop-portal` ScreenCast is the alternative if portal-based capture is ever needed |
| Session locker / screensaver *(Phase 6 — net-new, not a port)* | `ext-idle-notify-v1` |
| Display settings (multi-monitor) *(Phase 6 — net-new, not a port)* | `wlr-output-management-unstable-v1` |
| Global hotkeys (currently `qxtglobalshortcut`/`XGrabKey`) | No Wayland equivalent exists (by design) — Biome implements `org.freedesktop.portal.GlobalShortcuts`; Forest's hotkey client targets that same portal interface rather than a Biome-specific one (see Decoupling goal) |
| System tray | Already DBus/StatusNotifierItem-based — no change needed |
| Cursor theme / numlock | Numlock: keyboard state, no protocol needed. Cursor rendering: `wlr_xcursor_manager` + `cursor-shape-v1`; live theme-change push uses a Biome-specific `org.biome.Cursor` DBus interface (`ipc/cursor_bridge.h`) since no protocol covers pushing a cursor-theme change to a running compositor — see the Phase 5 cursor note below |

## Phased roadmap

**Phase 0 — Skeleton & dev loop.**
CMake project, wayland-scanner protocol codegen, link wlroots 0.18 +
wayland-server. Get a `tinywl`-equivalent running: one output, black screen,
quit on keypress. *Nested* (wlroots auto-detects a nested Wayland/X11
backend) is the loop for quick, disposable checks — e.g. an agent verifying
a build actually runs without crashing. The user's own manual verification
normally runs from a raw TTY via DRM/KMS instead, not nested — see the
Phase 3 note below.

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
cycling, hover/press states) is left to the user's own manual testing rather
than agent-driven screenshots or synthetic input. The user normally runs
this from a real TTY session (DRM/KMS), not nested — nested is for an
agent's own quick, disposable checks (see Phase 0).

**Phase 3.5 — Input & session completeness.** *(added 2026-08-22; all three
items done and confirmed working by manual testing)* Found by
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
  module wiring `wlr_session_lock_manager_v1`, using an opaque full-output
  scene rect raised above everything else to occlude input/output while
  locked (same strategy sway and Hyprland both use — checked against their
  source). Two real leaks were found and fixed during this work (a
  newly-mapped window and an Xwayland override-redirect popup could each
  still render, and the latter could still steal focus, over the lock
  surface) — see `docs/architecture-notes.md`'s "Session lock" section for
  the design invariants and both fixes in full. Confirmed working via
  manual testing (real DRM/KMS session, swaylock as the test client).

**Phase 4 — Forest shell integration.** *(done, both sides manually
confirmed 2026-09-05)*
Layer-shell for panel + desktop (bundled with `xdg-output-unstable-v1`,
since layer-shell clients commonly query it for per-output name/logical
geometry), foreign-toplevel-management for the windowlist plugin, a DBus
hotkey service implementing `org.freedesktop.portal.GlobalShortcuts`
(replacing `qxtglobalshortcut` — see Decoupling goal), and a Wayland-native
workspace-switching mechanism for the deskswitch plugin. This is where
Forest's shell processes became Wayland-native instead of X11 clients —
effectively the `xcbutills` replacement, the biggest chunk of shell-side
work — and the first phase where any `forest/` code itself was modified.

Final protocol/architecture decisions from this phase, kept here since
they're load-bearing for anything that touches these areas later:
`wlr-foreign-toplevel-management-unstable-v1` was chosen over
`ext-foreign-toplevel-list-v1` for windowlist (the `ext` protocol is
identification-only, no control requests); Biome implements the
`org.freedesktop.impl.portal.GlobalShortcuts` *backend* interface (brokered
by the system's `xdg-desktop-portal`), not the frontend, per the Decoupling
goal; workspaces landed as the `ext-workspace-v1` + `org.biome.Workspaces`
hybrid described in the protocol table above, with `ext-foreign-toplevel-list-v1`
also adopted (beyond windowlist's original need) purely to give Biome a
stable per-toplevel identifier to correlate the two DBus/protocol surfaces
by creation order. A real persistent per-output scene-layer stack
(background/bottom/toplevels/top/overlay/session-lock, mirroring sway's
`sway_output::layers`) was built as part of layer-shell support, which let
Phase 3.5's runtime `session_locked` visibility checks be deleted in favor
of structural z-order.

Screenshots, the session locker, and display settings were originally
scoped into this phase but moved out 2026-08-22: none of the three exist as
Forest features today (X11 or otherwise), so building them is net-new app
work, not a port — bundling that into an already-large port-focused phase
just added scope for no dependency reason. See Phase 6 below.

**Phase 5 — Cutover.** *(done, manually confirmed 2026-09-06 — login via
the new wayland-sessions entry works)*
New `forest-session` variant that execs Biome instead of `xfwm4`, a Wayland
session entry for the greeter. **Decided 2026-08-22: this is a hard switch,
not a dual-maintained transition** — Phase 4's X11 mechanisms (struts,
window enumeration, hotkeys, workspaces) get replaced outright by their
Wayland equivalents rather than kept working behind a runtime-selectable
abstraction. Revised from this phase's original wording ("X11 path kept
alive in parallel until Biome is solid, then eventually deprecated"): a
live dual-backend would mean building and maintaining two implementations
of every Phase 4 mechanism indefinitely, for comparatively little benefit
since Biome isn't meant to become a second-class option — it's meant to
replace xfwm4 outright. Forest simply requires Biome (or another
Wayland compositor speaking the same protocols, per the Decoupling goal)
from Phase 4 onward; there's no X11-fallback code path to design or keep
working.

**Status:** done. The process model inverts from the X11 flow: Biome (not
`forest-session`) is now the top-level process, since it owns the seat/DRM.
`forest/usr/share/forest/startforest-wayland` sets up session env and
`exec`s `biome -s /usr/bin/forest-session`; Biome's existing sway/tinywl-style
`-s` flag (`biome/core/main.cpp`) forks that command once its Wayland socket
is live, so `forest-session` inherits `WAYLAND_DISPLAY` and just launches
`forest` + autostart, no longer the WM itself. `forest-session`'s old
`window_manager` QSettings key and its General-settings text box were
removed outright rather than left inert — compositor choice now lives at
the session/`.desktop`-file layer (swap `wayland-sessions/Forest.desktop`'s
`Exec=` line to point at a different compositor), which fits the
Decoupling goal better than an in-app setting anyway. The old
`usr/share/xsessions/Forest.desktop`, `startforest`, and its `xfwm4.xml`
seeding were deleted, not kept alongside — Forest's shell plugins no longer
have a working X11 path post-Phase-4, so a session entry that still
launched one would silently produce a half-broken desktop.

**Follow-up fix (2026-09-06):** first login worked, but global hotkeys
didn't fire. Root cause: `biome/data/xdg-desktop-portal/biome-portals.conf`
and `.../portals/biome.portal` (which tell the system's xdg-desktop-portal
frontend to route `org.freedesktop.impl.portal.GlobalShortcuts` to Biome's
own backend, per `ipc/global_shortcuts_portal.cpp`) existed in the repo but
were never installed anywhere, and `startforest-wayland` set
`XDG_CURRENT_DESKTOP="Forest"` instead of including `biome` - the portal's
desktop-specific config lookup (see `portals.conf(5)`) only checks
`<desktop-name>-portals.conf` for names actually listed in
`XDG_CURRENT_DESKTOP`, so `biome-portals.conf` was never found regardless.
Fixed by adding `install(FILES ...)` rules for both files to
`biome/CMakeLists.txt`, and changing `startforest-wayland` to export
`XDG_CURRENT_DESKTOP="Forest:biome"`. The manual dev-testing script at the
top-level `start_xdg_desktop_portal.sh` (`XDG_CURRENT_DESKTOP=biome
xdg-desktop-portal --replace --verbose`) had been working around exactly
this gap by hand, which is why hotkeys worked in prior manual Phase 4
testing but not through the real session path until now.

**Cursor theme/size at session startup** *(done — implemented in
`startforest-wayland`)* (found while porting
`system/system-settings/cursorthemesettings.cpp` off X11 - see that file
and `biome/ipc/cursor_bridge.h`): the new session-launch script should
export `XCURSOR_THEME`/`XCURSOR_SIZE` from the user's saved cursor choice
before launching Biome and any Wayland clients. Not strictly required for
Biome itself or for Qt/GTK apps that fall back to the `~/.icons/default`
theme-name convention (`wlr_xcursor_theme_load(NULL, size)` resolves to
literal theme `"default"`, and `cursorthemesettings.cpp` already maintains
`~/.icons/default/index.theme` as a side effect of applying a theme) — but
it's still the convention every wlroots compositor (sway, river, Hyprland)
follows, and it's the only thing anything that reads the env vars directly
(SDL, some GTK contexts) will honor. There is no live-reload path for
already-running clients beyond Biome's own compositor-drawn cursor and any
`cursor-shape-v1` client (`org.biome.Cursor` handles that case) - a known,
ecosystem-wide Wayland limitation (confirmed against sway's own
`xcursor_theme` command), not something to solve here.

**Phase 6 — New capabilities.** *(added 2026-08-22, split out of Phase 4)*
Screenshots (`wlr-screencopy-unstable-v1` or `ext-image-copy-capture-v1`),
a session-locker UI (a new Forest lock-screen client speaking
`ext-session-lock-v1`, plus wiring `ext-idle-notify-v1` so idle timeout
actually triggers it — Biome's compositor-side `ext-session-lock-v1`
support landed and was confirmed working in Phase 3.5, tested with
swaylock, but Forest itself still has no lock-screen client of its own),
and a
display-settings plugin wired to `wlr-output-management-unstable-v1`
(Biome-side protocol support for that still needs to land too, in Phase 3,
Phase 4, or here, whichever comes first). None of these three are ports —
Forest has no existing screenshot tool, lock client, or multi-monitor
settings UI on X11 today, so this is net-new design/build work using the
Wayland protocol as the target from day one. Deliberately sequenced after
cutover since none of the three block Phase 5, but a lock-screen client is
lightweight enough (fullscreen, single-purpose) that it's worth considering
as an earlier pilot for whatever Forest-side Wayland-client plumbing Phase
4 established.

**Display-settings implementation note (found 2026-08-22 while researching
Phase 4's Qt/Wayland binding options):** `libkscreen`/KScreen already has a
working `wlr-output-management-unstable-v1` backend (used for
KScreen-on-Sway) — reuse it for this plugin instead of hand-binding the
protocol directly when this phase starts.

## Open risks

- **Biome has no Debian packaging yet.** `forest/debian/control`'s
  `Depends:` still hard-lists `xfwm4, gtk2-engines-murrine, ..., xinit,
  xserver-xorg, x11-xserver-utils` from the X11 era — that's now wrong
  post-cutover (nothing in this list is required, and Biome is), but it
  can't simply become `Depends: biome` since there's no `debian/` directory
  in the `biome` repo at all yet, i.e. no installable `.deb` to depend on.
  Needs a dedicated packaging pass (control file, install rules, changelog)
  before `forest`'s own packaging can be corrected to match the Wayland-only
  reality.
- **Output `scale` + manual `x`/`y` positions can silently open a layout gap
  that traps the cursor (found 2026-09-06).** `core/output_config.{h,cpp}`
  stores each connector's `x`/`y` as independent, static, hand-entered
  logical-pixel offsets with no cross-output validation. An output's
  effective (logical) size is `mode_size / scale`
  (`wlr_output_effective_resolution`), so changing one output's `scale`
  shrinks/grows only that output's logical box — any sibling output whose
  `x`/`y` was calibrated against the old size is left stale, opening a gap
  in `wlr_output_layout` that belongs to no output. Biome has no custom
  cursor confinement; it relies on wlroots' default
  `wlr_cursor_warp_closest` → `wlr_output_layout_closest_point`, which
  resolves every pointer motion to the closest point across the *entire*
  layout. A position inside the gap that's closer to the scaled output's
  edge than to the neighbor's edge gets snapped straight back on every
  motion event — indistinguishable from the cursor being locked inside the
  scaled screen. Current workaround is hand-computing correct neighbor
  `x`/`y` as `floor(mode_width / scale)` past the scaled output (verified
  working). Real fix should live in Biome itself (auto-arrange by default,
  and/or validate + warn/auto-correct gaps computed from effective
  resolution at output-manager init), not be deferred entirely to the
  eventual display-settings UI — that UI will consume Biome's resolved
  layout via `wlr-output-management-unstable-v1`, which assumes the
  compositor is the source of truth for a valid arrangement, not the UI.
- **An Xwayland override-redirect popup from a fullscreen window can render
  behind it (found 2026-09-07, while adding fullscreen support).**
  `set_toplevel_fullscreen` (`desktop/toplevel.h`) reparents a fullscreen
  toplevel's `scene_tree` into the new `BiomeServer::layers.fullscreen`
  layer (above every layer-shell layer, so a panel/dock in `layers.top`
  doesn't cover it — see that field's doc comment in `core/server.h`). A
  Wayland-native `xdg_popup` isn't affected, since it's created as a scene
  child of its parent toplevel's own tree (`wlr_scene_xdg_surface_create`
  in `desktop/xdg_shell.cpp`) and so is carried along by the reparent
  automatically. But an Xwayland override-redirect surface (an X11-native
  context menu/tooltip/dropdown — `desktop/xwayland_shell.cpp`'s
  `unmanaged_associate`) is a separate `BiomeUnmanaged` object parented
  directly to `layers.toplevels`, not nested under whichever toplevel
  "owns" it, so it stays below `layers.fullscreen` even when its owning
  window is fullscreen. Not yet confirmed against a real app (no Xwayland
  client on hand that both fullscreens and opens an override-redirect
  popup while doing so) - worth a real fix if it turns out to matter in
  practice, likely by also raising unmanaged surfaces above
  `layers.fullscreen` whenever at least one toplevel is currently
  fullscreen.
