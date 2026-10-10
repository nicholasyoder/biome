# Architecture Notes

Reference material for design decisions and postmortems that are too
detailed for `docs/roadmap.md`'s roadmap altitude but still explain
load-bearing "why is it built this way" invariants in the current code.
`docs/roadmap.md` stays the authoritative source for current status and open
work; `docs/history.md` has the original phase-by-phase narrative; this file
is where the reasoning behind specific pieces of code lives once it's
settled, so a future change doesn't have to re-derive it or accidentally
regress an invariant nothing else documents.

## Config sources (`core/config.{h,cpp}`)

`/etc/biome/conf.d/*.conf` (filename order), then `~/.config/Biome/Biome.conf`;
later sources replace earlier ones per key, never merge values. Drop-ins are
how a shell package supplies defaults (Forest ships `50-forest.conf`) without
Biome hardcoding shell names. The `/etc/xdg` QSettings fallback is disabled
so conf.d is the only system location.
Biome only ever writes the user file, and only `[Outputs]` (see "Live output
management"); `QSettings` rewrites it whole, so comments are lost.

## Session lock (`desktop/session_lock.{h,cpp}`, Phase 3.5)

Wraps `wlr_session_lock_manager_v1`. The whole implementation leans on one
invariant: a single `server->lock_tree` scene node, raised to the top of
`server->scene->tree` and enabled for the duration of a lock, occludes
everything else. `desktop_toplevel_at`/`decoration_toplevel_at` already stop
their scene-graph hit-test at the first node under the cursor, so an opaque
full-output `wlr_scene_rect` per output (created unconditionally at
output-add time, so a hotplugged monitor is blanked from its first frame
even mid-lock) makes every normal window and Biome's own decoration
unreachable to click/hover with no bespoke lock-aware hit-testing needed.
The only place a normal toplevel's scene node ever gets raised is
`focus_toplevel()` — gating that one function on `server->session_locked` is
what stops a window from being raised above, or stealing keyboard focus
from, the lock surfaces. `handle_keybinding()` swallows nothing while locked
except VT-switch (kernel-level session handoff, matches sway) — every other
compositor keybind falls through as an ordinary key event to the lock client.

`session_locked` (survives a lock client crash) and `active_lock` (nulled
the moment that lock's `wl_resource` is gone, crash or not) are deliberately
two separate fields: per spec, a client dying without calling
`unlock_and_destroy` must not unlock the session, so `session_locked` is
only ever cleared by a real `unlock` event. The reject-a-second-lock check
in `new_session_lock` tests `active_lock`, not `session_locked`, so a
replacement client can `lock()` and take over recovery after a crash — the
compositor-policy recovery path the spec names — with no extra code needed.

`wlr_session_lock_v1_send_locked()` is deferred until every currently
enabled output has actually committed a frame since the lock began (the
spec's timing rule exists to prevent a suspend-races-resume race); the
compositor's own blanking already satisfies it without waiting on the
client's own surface to render.

Compared against sway's real `sway/lock.c` and Hyprland's session-lock
source: the core strategy (an opaque occluding layer gated on a locked
flag) matches both — not a "hacky" approach, just missing (at the time)
the persistent per-output layer stack that Workstream A later added, which
made `lock_tree`'s position permanent-by-construction instead of something
raised at lock time. One polish borrowed from sway: an abandoned lock
(crashed without `unlock_and_destroy`) tints the blank rect red instead of
staying the same black as a normal lock, so a stuck-locked screen is
visually distinguishable.

**Two real leaks found and fixed post-implementation**, both from the same
root cause: keeping `lock_tree` raised to the top only protects scene
content that *already existed* when the lock began — `wlr_scene_tree_create()`
always appends a new node as the topmost sibling regardless of history, so
anything mapped after the lock started re-topped itself automatically.
1. `update_toplevel_visibility()` (`desktop/workspace.cpp`) didn't factor in
   `session_locked` at all — fixed by adding it to the visibility formula,
   plus re-running it over every existing toplevel on both lock and unlock.
2. Xwayland override-redirect surfaces (X11 popups/menus/tooltips,
   `desktop/xwayland_shell.cpp`'s `BiomeUnmanaged`) are a completely
   separate path with no `BiomeToplevel` at all, invisible to fix (1) — and
   its map handler unconditionally raised to top *and grabbed keyboard
   focus* with no lock check at all, worse than the window-leak this was
   chasing. Fixed by disabling the surface's scene node outright (not just
   skipping the raise) when `session_locked`, which also skips the focus
   grab. xdg-shell popups don't have this problem — they're parented under
   their own toplevel's content tree, not the scene root, so they're
   transitively hidden whenever their parent toplevel is.

Deliberately out of scope: restricting the session-lock global to a
privileged client (Biome has no client-allowlist mechanism anywhere yet),
cancelling an in-flight drag if a lock starts mid-drag, and
`idle-notify`/auto-lock-on-idle (Phase 6 — this only makes a
manually-triggered lock actually secure).

## Qt/GLib event loop bridge (`core/qt_glib_bridge.{h,cpp}`)

Qt runs on `QEventDispatcherGlib`, but Biome never calls `QApplication::exec()`.
The bridge drives `g_main_context_default()` from `wl_event_loop` instead, so
Qt/QtDBus (posted events, `deleteLater()`, D-Bus replies) dispatch fd-driven
with zero idle CPU. Replaced a 10ms polling pump (`8677ffb` has the full
research writeup).

- **`wl_display_run()` stays primary.** Flipping to `exec()` would lose
  wlroots' `wl_display_terminate()` (host hangup, DRM/libinput failure).
- **Re-query and resync fds every cycle.** Dispatch changes GLib's fd set;
  skipping the resync is how a D-Bus reply silently never dispatches.
- **Acquire once, never release.** Single-threaded and the context's sole
  driver for the process lifetime.
- **Livelock guard.** A source whose `prepare()` always reports ready would
  spin `rearm()` and starve the compositor (including the terminate check);
  it's capped at `kRearmLivelockGuard` iterations, then deferred via an idle
  source.
- Any future `GSource` on the default context is dispatched for free.

Closing a nested backend's host window doesn't terminate Biome: wlroots 0.18
handles `xdg_toplevel.close` with `wlr_output_destroy()`, not
`wl_display_terminate()`. Intended, not a bridge bug.

## Layer-shell reconfigure-storm bug (Workstream A)

Found on the first bare-metal multi-monitor test: cursor motion was jerky
system-wide whenever the desktop wallpaper (a full-screen layer-shell
surface) was enabled. Low CPU while visibly stuttering ruled out a
busy-loop; a `WAYLAND_DEBUG=1` capture showed every mapped layer-shell
surface cycling through configure/ack/commit on repeat, forever, with the
configured size identical every time.

Root cause, confirmed by reading wlroots' own
`wlr_layer_surface_v1_configure()`: it's unconditional — it always sends a
fresh configure regardless of whether the box changed, and leaves dedup
entirely up to the compositor. `handle_layer_surface_commit()` called
`arrange_layers()` on *every* commit from *any* layer surface on the
output, including a plain content-only repaint — so any surface committing
reconfigured every other layer surface, each of which acks and recommits
per normal client behavior, retriggering `arrange_layers()` again. A
self-sustaining storm, naturally paced by buffer-release/vsync timing
(which is why it stayed cheap on CPU while still starving pointer-motion
delivery every ~20–40ms).

Fixed by gating the commit handler's `arrange_layers()` call on the
surface's `committed` state actually containing a layout-relevant bit
(`DESIRED_SIZE`/`ANCHOR`/`EXCLUSIVE_ZONE`/`MARGIN`/`LAYER`) — a plain
content commit now leaves every other layer surface untouched. A narrow
safety net (`arrange_layers()` also called once at map time) covers the one
transition the now-gated commit handler can no longer be relied on to catch.

**Related, smaller finding on the way to this fix:** `exclusive_zone == -1`
(not `0`) is the documented idiom for "a wallpaper/lock-screen surface that
claims no space of its own but should still be sized against the *full*
output, not other layers' reserved space" — `wallpaperwidget.cpp` originally
used `0`, which sized it against the panel-shrunk `usable_area` instead,
leaving an uncovered strip behind the panel as well as contributing to the
reconfigure churn above.

## Keyboard-focus chokepoint (Workstream B)

A recurring bug class: several call sites granted keyboard focus directly
via `wlr_seat_keyboard_notify_enter()`, bypassing `focus_toplevel()`'s
bookkeeping entirely (the layer-shell keyboard-interactive grab, the
Xwayland unmanaged-surface grab, `xdg_popup` map/unmap, and the
"clicked a non-toplevel surface" cursor path) — each one independently
capable of leaving stale/duplicate focus state in `windowlist` and similar.
Rather than patch each site as found, all five were migrated onto one new
chokepoint, `grant_keyboard_focus_to_non_toplevel(BiomeServer*,
wlr_surface*)` (`desktop/toplevel.{h,cpp}`), which clears the previously
focused toplevel's state before granting focus to anything that isn't a
`BiomeToplevel`. `focus_toplevel()` itself still calls `notify_enter()`
directly, deliberately, so an active popup grab still wins over a
newly-mapped toplevel trying to steal focus. Worth remembering as a
pattern: any *new* code path that grants keyboard focus to something other
than a normal toplevel should go through this chokepoint, not call
`wlr_seat_keyboard_notify_enter()` directly.

## `GlobalShortcuts` portal architecture (Workstream C)

`org.freedesktop.portal.GlobalShortcuts` is a *frontend* interface that the
system's `xdg-desktop-portal` daemon brokers to a desktop-specific
*backend* implementing `org.freedesktop.impl.portal.GlobalShortcuts` (what
KWin/Mutter actually implement). Biome implements the backend, not the
frontend, per the Decoupling goal — this is what lets any portal-aware
client (not just Forest) use it. Backend methods
(`CreateSession`/`BindShortcuts`/`ListShortcuts`/`ConfigureShortcuts`) reply
synchronously, unlike the frontend's async `Request`-object protocol.

Broker wiring needs two git-tracked files installed to real
`xdg-desktop-portal` search paths: `data/xdg-desktop-portal/portals/biome.portal`
and `data/xdg-desktop-portal/biome-portals.conf`. No `.service`
D-Bus-activation file is shipped — Biome self-registers at startup since
it's the compositor and is always already running by the time a portal
call could make sense; a D-Bus-activation `Exec=` here would risk launching
a second `biome` process as a side effect of a stray portal call.

**Integration gap that isn't obvious from the code:** `main.cpp`'s
`QApplication` is offscreen and its event loop is deliberately never run
(`decoration/` drives Qt synchronously off Biome's own loop instead).
Without pumping Qt's event loop at all, `QDBusConnection` setup calls
succeed but no incoming method call or outgoing signal ever dispatches. A
`wl_event_loop` timer calling `QCoreApplication::processEvents()` every
10ms is what actually makes the D-Bus object live — still Biome's own loop
driving it, just polling rather than being woken by Qt's own dispatch fd.
Any future Qt/D-Bus-backed `ipc/` module needs the same pump, not a second
one.

## Workspace protocol (Workstream D)

`ext-workspace-v1` has no wlroots server-side helper (unlike layer-shell or
foreign-toplevel-management) — it's hand-rolled from the raw XML
(manager/group/handle), the highest from-scratch effort of any Phase 4
protocol. It covers switching/listing but has **no toplevel↔workspace
linkage at all** (checked against both its own XML and
`wlr-foreign-toplevel-management-unstable-v1.xml`) — that's why the final
design is a hybrid: `ext-workspace-v1` for switching/listing/active-highlight,
plus a narrow `org.biome.Workspaces` DBus interface purely for the
toplevel↔workspace relationship windowlist's "move to desktop" and the
per-desktop dot counts need. Correlating a `wlr_foreign_toplevel_handle_v1`
object with the DBus interface's toplevel argument (no stable string
identity otherwise) is done by also adopting `ext-foreign-toplevel-list-v1`
purely for its auto-generated stable `identifier` string, and pairing the
two handles by creation-order arrival in `foreign_toplevel_create()` — the
same approach other wlr-ecosystem clients use for this exact gap.

## Live output management (`core/output.cpp`, `core/output_management.cpp`)

`wlr_output_manager_v1` handles `test`/`apply` for `wlr-randr`-style clients.

- **The last apply persists.** A successful `apply` (any client) writes each
  configured head's `[Outputs]` entry to the user `Biome.conf`, in the format
  `load_output_configs()` reads (explicit mode, shortest round-trip float
  scale, so a re-apply of the restored layout compares equal). `test`,
  hotplug repair and startup never write; unmentioned connectors keep their
  entries. Identity-based (make/model/serial) profiles are the shell's job.

- **One geometry path.** `output_sync_geometry()` is the only place that
  positions per-output scene state (layer trees, lock tree/rect) and re-runs
  `arrange_layers`; `output_set_enabled()` owns layout/scene-output membership.
  A disabled output is out of `output_layout` entirely (it keeps a
  `current_mode`, so membership would give it hit-testable space).
- **Gap validation is compositor policy.** `wlr_cursor` clamps to the closest
  layout point, so a gap between outputs traps the cursor. `test` and `apply`
  both run `layout_is_connected()` on the *resulting* boxes (effective size =
  mode/scale, transform-swapped) and fail rather than auto-arrange; overlap is
  allowed for mirroring. Hotplug/unplug can't be rejected, so
  `output_relayout()` repairs instead: restore every output's last live
  position (`output_configs`), auto-place positions older than the latest
  apply (`position_generation`), then close gaps around the top-left output
  (`core/output_arrange.cpp`: drop empty bands, then attach leftover groups).
  The repair is never written back, so replug restores the last live layout.
- **`output_layout_settled()`** runs once after a change settles (not from the
  layout `change` signal, which fires per head mid-apply): layer surfaces
  first (their exclusive zones feed maximize targets), then windows, then the
  cursor. Windows still overlapping an output keep absolute coordinates;
  only off-screen windows and their restore boxes are pulled to the nearest
  output, and maximized/fullscreen ones re-fit.
- **Layer surfaces have a `home`.** Disabling an output reparents its layer
  surfaces to the first enabled output; re-enabling moves them back. Unplug
  still closes them (protocol `closed`).
- **DRM gotchas.** A disabled head has no mode, so clients send none and
  wlroots makes a 0x0 custom mode; we substitute the current/preferred mode.
  Batched `wlr_backend_test/commit` allocates no buffer, so enabling or
  re-moding needs a cleared one attached ("No primary frame buffer"). A
  modeset on a DPMS-off connector fails, so apply powers outputs on first.

## Accepted non-standard behavior

Deliberate deviations from protocol or common compositor practice that
Forest depends on. Open ones (bugs, spec gaps slated for removal) live in
`docs/roadmap.md`. Audited 2026-10-04.

- **`org.biome.Workspaces` D-Bus** for toplevel↔workspace linkage (see
  "Workspace protocol" above). Forest's windowlist/deskswitch check
  `isAvailable()` and degrade without it.
- **ext-foreign-toplevel-list ↔ wlr-foreign-toplevel paired by creation
  order.** Forest's windowlist relies on Biome creating both handles
  back-to-back; neither protocol guarantees it.
- **Bare-modifier triggers in the GlobalShortcuts backend.** A lone
  `LOGO` (etc.) fires on an uninterrupted tap; not in the shortcuts-spec
  grammar, so Forest's Meta menu hotkey won't bind on other backends.
- **Layer-shell fades keyed by namespace** (`[LayerShell]` config,
  `core/fade_config.h`). Compositor-side animation picked by client
  namespace; Hyprland's `layerrule` is the precedent. The shell's package
  supplies its namespaces via a conf.d drop-in (see "Config sources").
- **Every newly mapped toplevel takes focus**, with no xdg-activation token
  check. Common floating-WM convention (sway does the same); xdg-activation
  is planned for 0.2.0 in the roadmap.
- **ext-workspace: one global group, no `output_enter`.** Workspaces are
  global, not per-output; per-output-filtering clients may show none.

## Idle inhibit visibility (`desktop/idle.cpp`)

An inhibitor counts only while its surface is visible on some enabled
output, on any monitor, focused or not. It's re-evaluated on every output
frame, since anything that shows or hides a surface damages an output.

- Visibility = an enabled scene buffer for that surface with nonzero
  `active_outputs` (this covers occlusion and offscreen). The walk must skip
  disabled subtrees (`wlr_scene_node_for_each_buffer` does): wlroots 0.18
  doesn't refresh `active_outputs` or fire `output_leave` when an *ancestor*
  is disabled, so minimized and other-workspace windows keep stale values.
- Disabled (DPMS/blanked) outputs don't count, so inhibitors drop while
  everything is dark; the next frame after wake restores them.
- Opaque lock surfaces occlude everything, so a locked session isn't held
  awake by a video behind the lock.

## Screencopy (`core/main.cpp`)

wlroots implements `wlr-screencopy-unstable-v1` entirely; Biome only creates
the global.

- Unrestricted: any client, sandboxed or not, can capture. Same stance as
  session-lock (no allowlist mechanism); `security-context-v1` (Phase 8) is
  the eventual fix.
- wlroots 0.18.2 attaches the frame's `output_enable` listener to
  `events.destroy`, so a frame pending when its output is DPMS'd/disabled
  stalls until the output comes back (then completes). Requests on an
  already-off output fail at once. Fixed upstream in 0.19 (31f9d6bb); not
  worked around, since sending `failed` ourselves risks a later `ready`.
- Captures while locked show the lock screen: it copies what's presented.

## Data control (`core/main.cpp`)

wlroots implements `wlr-data-control-unstable-v1` entirely; Biome only creates
the global. Sets go straight to `wlr_seat_set_selection`/
`wlr_seat_set_primary_selection`, bypassing `core/input.cpp`'s request handlers.

- Unrestricted, like screencopy: any client can read/set both selections,
  including while locked. `security-context-v1` is the eventual fix.
- Xwayland picks up data-control sets through the normal seat selection path.

## xdg-activation (`desktop/xdg_activation.cpp`)

Only path for a client to raise an already-open window (single-instance
relaunch, link/notification/tray click). New windows don't need it: map-time
focus is unconditional.

A token is honored iff all of:

- wlroots accepted it (serial sent to the requester; source surface focused
  *at issue*) **and** it has a seat + serial — seat-less tokens skip
  wlroots' checks entirely;
- the requester got the most recent button/key press when it was issued, and
  no press has gone to a different client since (`press_recipient_changes`).
  Input-based, not focus-based: a menu closing after the click moves focus
  without invalidating the token. Presses on Biome's decoration count as the
  window's client; compositor-consumed keys and clicks on nothing count as
  "different";
- the session isn't locked; the target is a mapped xdg toplevel.

Honored → `activate_toplevel()` (same as a taskbar click; no fullscreen
exception). Otherwise → `set_toplevel_urgent()`: `urgent` QSS property on
`#biomeFrame` and switcher icons, plus `ext-workspace-v1` `urgent` on the
window's workspace; cleared on focus. Token lifetime is wlroots' 30 s.

Xwayland has no tokens: `_NET_ACTIVE_WINDOW` sets urgent, `WM_HINTS`
urgency sets/clears it; neither focuses.

Launchers must mint the token in the click handler, before hiding the menu
(the issue-time focus check needs the menu surface still focused). A tray
host passes one via SNI `ProvideXdgActivationToken` before `Activate`.
Relaunching an app from a terminal (no `XDG_ACTIVATION_TOKEN`) only marks it
urgent — intended.
