# Architecture Notes

Reference material for design decisions and postmortems that are too
detailed for `docs/roadmap.md`'s roadmap altitude but still explain
load-bearing "why is it built this way" invariants in the current code.
`docs/roadmap.md` stays the authoritative source for current status and open
work; `docs/history.md` has the original phase-by-phase narrative; this file
is where the reasoning behind specific pieces of code lives once it's
settled, so a future change doesn't have to re-derive it or accidentally
regress an invariant nothing else documents.

## Module state and teardown (`core/server.h`, `core/main.cpp`)

Each subsystem's globals, listeners and flags live in its own struct embedded
in `BiomeServer` (`server->lock`, `server->pointer`, ...; module-private types
behind an owned pointer, like `server->keys`). Listener-bearing structs are
`static_assert`ed standard-layout, so handlers recover the server through
`biome_server_from()` without `-Winvalid-offsetof`; no `unordered_map`/`set`
in them. Each module has an `X_finish()` that removes its listeners; `teardown()`
calls them in reverse init order, except that the backend goes first (its
`output_destroy()` still needs the scene, cursor and renderer). wlroots asserts
on any listener left on a destroyed object, so a new listener needs its removal
in that module's `X_finish()`. `tests/integration/` covers teardown headless.

## Config sources (`core/config.{h,cpp}`)

`/etc/biome/conf.d/*.conf` (filename order), then `~/.config/Biome/Biome.conf`;
later sources replace earlier ones per key, never merge values. Drop-ins are
how a shell package supplies defaults (Forest ships `50-forest.conf`) without
Biome hardcoding shell names. The `/etc/xdg` QSettings fallback is disabled
so conf.d is the only system location.
Biome only ever writes the user file, and only `[Outputs]` (see "Live output
management"). A shell may write `[Theme]` there (Forest's theme page sets
`name`); both sides use `QSettings`, which rewrites the whole file keeping
the other's keys, so comments are lost.

## Session lock (`desktop/session_lock.{h,cpp}`, Phase 3.5)

Wraps `wlr_session_lock_manager_v1`. Occlusion comes from the fixed layer
stack (`core/layers.cpp`): `layers.session_lock` is the last of the fixed
trees `scene_layers_init()` creates and is enabled only while locked. Each
output has its lock tree there with an opaque full-output rect (created at
output-add, so a hotplugged monitor is covered from its first frame) and
the client's lock surface. Toplevels (including fullscreen ones and
Xwayland override-redirect surfaces) and layer surfaces live in lower
trees, so they're hidden and unhittable while locked without any
per-surface lock check. So do the switcher, window menu and drag icon
(`layers.osd`); lock start also cancels the switcher and ends seat grabs,
and the menu refuses to open while locked.

Keyboard: see "Keyboard-focus chokepoint". Keybindings, the switcher and
modifier taps are gated while locked except VT switch (matches sway); every
other key goes to the lock client.

`session_locked` (survives a lock client crash) and `lock.active` (nulled
the moment that lock's `wl_resource` is gone, crash or not) are deliberately
two separate fields: per spec, a client dying without calling
`unlock_and_destroy` must not unlock the session, so `session_locked` is
only ever cleared by a real `unlock` event. The reject-a-second-lock check
in `handle_new_session_lock` tests `lock.active`, not `session_locked`, so a
replacement client can `lock()` and take over recovery after a crash — the
compositor-policy recovery path the spec names. Any client can do this
(no allowlist), so a crashed lockscreen plus a hostile local client could
unlock; the same holds in sway. An abandoned lock tints the rects red
(sway's polish) so a stuck-locked screen is distinguishable.

`wlr_session_lock_v1_send_locked()` is deferred until every enabled output
has committed a frame since the lock began; with no enabled output it's
sent at once (unlit outputs show nothing and come back covered).

Deliberately out of scope: restricting the session-lock global to a
privileged client (no client-allowlist mechanism yet).

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

Closing a nested backend's host window doesn't terminate Biome: wlroots
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

Layer-shell `keyboard_interactivity` is enforced at the same two
chokepoints: while a mapped top/overlay `exclusive` surface exists
(`layer_shell_exclusive_focus()`), both refuse focus to any other client,
and `focus_topmost_on_active_workspace()` (every focus-restore path) hands
focus back to it. Only that surface takes focus on map; `on_demand` takes it
on click, `none` never.

`BiomeServer::activated_toplevel` tracks the one xdg-activated toplevel
(set in `set_toplevel_focused`, nulled on unmap). Clearing focus entirely goes
through `clear_keyboard_focus()`, never a bare
`wlr_seat_keyboard_notify_clear_focus()`, or the old window stays activated.

While locked, the chokepoint refuses everything but lock surfaces, and
`focus_topmost_on_active_workspace()` hands focus to a mapped lock surface
(`session_lock_refocus()`) instead. Lock start ends any seat grab first: an
`xdg_popup` grab's keyboard enter/clear_focus are no-ops, so it would
otherwise keep the keys.

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

D-Bus dispatch inside Biome rides on the Qt/GLib event loop bridge (see
that section); any future Qt/D-Bus `ipc/` module needs nothing extra.

### Shortcut triggers (`core/trigger.cpp`)

- Key names are lowercased at parse. A trigger matches if its keysym equals
  the lowercased translated keysym **or** the key's level-0 keysym, so
  `SHIFT+exclam` and `SHIFT+1` both fire, and Caps Lock doesn't break letters.
  Caps Lock/Mod3/Mod5/NUM are ignored when matching.
- Empty parts (`CTRL+`, `a++b`) are rejected.
- Bare-key policy: a keyed trigger needs a non-Shift modifier unless it is an
  F-key, `XF86*`, Print/Pause/Break/Sys_Req. Bare-modifier taps are allowed,
  first-come per modifier.
- A keyboard removed mid-hold fires `Deactivated` for its held bindings.

## Workspace protocol (Workstream D)

`ext-workspace-v1` uses wlroots' `wlr_ext_workspace_v1` helper: one group
entered by every output, one handle per workspace with `id` `"<index>"`, name
`"<index + 1>"` and coordinates `[index]`; only `activate` is advertised.
**`org.biome.Workspaces` workspace numbers are the ext-workspace `id` parsed as
an integer** (Forest falls back to arrival order when no `id` came, i.e.
another compositor or an older Biome). It covers switching/listing but has **no toplevel↔workspace
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

- **One geometry path.** `output_sync_geometry()` (layout `change` signal) is
  the only place that positions per-output scene state (layer trees, lock
  tree/rect) and re-runs `arrange_layers`; an output out of the layout has
  its trees hidden, not moved. `output_set_enabled()` owns layout/scene-output
  membership and destroys the scene output before leaving the layout.
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
  layout `change` signal, which fires per head mid-apply): window relocation,
  then the cursor (layer surfaces were already arranged from the `change`
  signal, so exclusive zones are current). Windows still overlapping an output keep absolute coordinates;
  only off-screen windows and their restore boxes are pulled to the nearest
  output, and maximized/fullscreen ones re-fit.
- **Disabling an output closes its layer surfaces** (protocol `closed`), same
  as unplug (`output_set_enabled` -> `layer_shell_close_output_surfaces`).
  Clients re-create them on the remaining screens.
- **DRM gotchas.** A disabled head has no mode, so clients send none and
  wlroots makes a 0x0 custom mode; we substitute the current/preferred mode.
  Batched `wlr_backend_test/commit` allocates no buffer, so enabling or
  re-moding needs a cleared one attached ("No primary frame buffer"). A
  modeset on a DPMS-off connector fails, so apply powers outputs on first.

## Accepted non-standard behavior

Deliberate deviations from protocol or common compositor practice that
Forest depends on. Open ones (bugs, spec gaps slated for removal) live in
`docs/roadmap.md`. Audited 2026-11-15.

- **`org.biome.Workspaces` D-Bus** for toplevel↔workspace linkage (see
  "Workspace protocol" above). Forest's windowlist/deskswitch check
  `isAvailable()` and degrade without it.
- **ext-foreign-toplevel-list ↔ wlr-foreign-toplevel paired by creation
  order.** Forest's windowlist relies on Biome creating both handles
  back-to-back (and on binding both globals in one dispatch); neither
  protocol guarantees it.
- **Bare-modifier triggers in the GlobalShortcuts backend.** A lone
  `LOGO` (etc.) fires on an uninterrupted tap; not in the shortcuts-spec
  grammar, so Forest's Meta menu hotkey won't bind on other backends.
- **Layer-shell fades keyed by namespace** (`[LayerShell]` config,
  `core/fade_config.h`). Compositor-side animation picked by client
  namespace; Hyprland's `layerrule` is the precedent. The shell's package
  supplies its namespaces via a conf.d drop-in (see "Config sources").
- **Every newly mapped toplevel takes focus**, with no xdg-activation token
  check. Common floating-WM convention (sway does the same); xdg-activation
  only governs raising already-mapped windows.
- **An honored xdg-activation token gives an `xdg_popup` keyboard focus**
  (see "xdg-activation"). Forest's hotkey-opened menus rely on it; elsewhere
  they open without keyboard focus.
- **Reconnected outputs get their last layout back** (`[Outputs]`, "Live
  output management"). Forest's displays service skips re-applying a
  profile when a monitor drops and returns within its debounce.
- **ext-workspace: one global group, no `output_enter`.** Workspaces are
  global, not per-output; per-output-filtering clients may show none.

## Idle inhibit visibility (`desktop/idle.cpp`)

An inhibitor counts only while its surface is visible on some enabled
output, on any monitor, focused or not. It's re-evaluated on every output
frame, since anything that shows or hides a surface damages an output.

- Visibility = an enabled scene buffer for that surface with a non-NULL
  `primary_output` (this covers occlusion and offscreen). Disabling an
  ancestor (minimize, workspace switch) clears the buffers below it in
  wlroots 0.19+; 0.18 left `active_outputs` stale, which is why this once
  needed a hand-rolled subtree skip.
- Disabled (DPMS/blanked) outputs don't count, so inhibitors drop while
  everything is dark; the next frame after wake restores them.
- Opaque lock surfaces occlude everything, so a locked session isn't held
  awake by a video behind the lock.

## Layer-shell fade warmup (`desktop/layer_shell.cpp`)

Fade-ins don't stamp their start time at map. Each one raises
`BiomeOutput::fade_warmup_ticks` (Opacity 1, ScanoutSnapshot 2) and renders
invisible until it runs out. Then every pending fade-in on that output starts
on the same tick.

- Why: the first flip after a scanout fade's new output-sized buffers takes
  ~65 ms (4K). A fade stamped at map loses that time, so forest-logout jumped
  to ~30% opacity before this.
- Shared per output, so forest-logout and its dim ramp in step.
- An unstarted fade has a zero `fade_start`. `elapsed_fraction()` returns 0 for
  it, so cancelling during warmup fades out from 0, not 1.

## Screencopy (`desktop/foreign_toplevel.cpp`)

wlroots implements `wlr-screencopy-unstable-v1` entirely; Biome only creates
the global.

- Unrestricted: any client, sandboxed or not, can capture. Same stance as
  session-lock (no allowlist mechanism); `security-context-v1` (Phase 8) is
  the eventual fix.
- Captures while locked show the lock screen: it copies what's presented.

## Image copy capture (`desktop/foreign_toplevel.cpp`)

`ext-image-copy-capture-v1` with output and foreign-toplevel sources (wlroots
implements both; Biome creates the globals). Same unrestricted stance as
screencopy.

- Each `BiomeForeignToplevel` owns a private always-enabled `wlr_scene` holding
  a second copy of the surface tree, and the capture source is built from that
  scene. Capturing `scene_tree` directly would render nothing for hidden
  (minimized/other-workspace) windows and would include windows stacked above.
- One source per toplevel, created on first request, freed with the scene.
- Frames are only sent on damage, so a static window yields one frame.
- Unlike output sources, toplevel sources keep producing window contents
  while locked.

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
  wlroots' checks entirely. Exception: tokens Biome issues itself
  (`xdg_activation_issue_token()`, the GlobalShortcuts `activation_token`),
  minted after the hotkey's press is noted so it doesn't count against them;
- the requester got the most recent button/key press when it was issued, and
  no press has gone to a different client since (`press_recipient_changes`).
  Input-based, not focus-based: a menu closing after the click moves focus
  without invalidating the token. Presses on Biome's decoration count as the
  window's client; compositor-consumed keys and clicks on nothing count as
  "different";
- the session isn't locked; the target is a mapped xdg toplevel, or a popup.

Honored → `activate_toplevel()` (same as a taskbar click; no fullscreen
exception). Otherwise → `set_toplevel_urgent()`: `urgent` QSS property on
`#biomeFrame` and switcher icons, plus `ext-workspace-v1` `urgent` on the
window's workspace; cleared on focus. Token lifetime is wlroots' 30 s.

A popup has no raise or urgency: an honored token gives it keyboard focus
(on map if not yet mapped), otherwise nothing. This is how a non-grabbing
popup opened by a hotkey (no press serial to grab with) gets keys; other
popups take focus on map only if they grab.

Xwayland has no tokens: `_NET_ACTIVE_WINDOW` sets urgent, `WM_HINTS`
urgency sets/clears it; neither focuses.

Launchers must mint the token in the click handler, before hiding the menu
(the issue-time focus check needs the menu surface still focused). A tray
host passes one via SNI `ProvideXdgActivationToken` before `Activate`.
Relaunching an app from a terminal (no `XDG_ACTIVATION_TOKEN`) only marks it
urgent — intended, but only if the client sends a token wlroots accepted
(e.g. seat-less); see the roadmap's rejected-token item.

## Touch (`core/touch.cpp`, `core/touch_map.cpp`)

A touch-down goes natively (`wl_touch`) to the surface under it if that surface's
client binds touch and no compositor UI or grab owns input; otherwise one point at
a time emulates a left-button pointer (sway's model), which covers decorations,
the window menu and touch-less clients. A client move/resize request follows a
touch point down on that window in preference to the pointer.

No input config, so each touchscreen's output is worked out (`touch_pick_output`):
libinput's output name, else an earlier tap-to-identify answer, else the output
with matching physical size (EDID mm), else the sole built-in panel, else the
sole output. It is redone on every layout change, since wlroots keeps a stale
mapping when its output goes. The output's transform is applied to the
coordinates by hand.

When that leaves it unclear among several outputs (cheap USB panels often report
no size, and generic EDIDs carry no name), Biome asks: every output shows a scrim
and a target at its own slot, and since touch coordinates are panel-relative, the
tap's position says which output's target was hit. All of that device's touches go
to the prompt until then; Esc or a pointer press skips it for the session (whole
layout). Answers go to `$XDG_STATE_HOME/biome/touch.conf` (state, not config),
keyed by device name and matched by EDID serial, or connector + make/model
without one. Chosen over a Forest settings control to keep touch mapping out of
the Forest/Biome interface.

## Pointer constraints (`core/pointer_constraints.cpp`)

A lock/confine is active only while its surface's root has keyboard focus
(sway's model; `seat->keyboard_state.events.focus_change` is the single
hook). Every way out of a game — Alt+Tab, the launcher hotkey, session lock,
closing the window — already moves focus, so there's no dedicated
"release pointer" chord. Root matching lets a client constrain a subsurface
(Firefox's content) while focus sits on the toplevel.

- On activation (and region change) the cursor warps into the region if
  outside it; the region stays empty until the surface's first commit after
  the request, so confinement also re-warps lazily from the motion path.
- Clamping only applies while the constrained surface has pointer focus and
  no Biome move/resize grab is running.
- Relative motion is always sent unclamped. Absolute motion (nested
  backends) is converted to a delta so both protocols work nested.
- On unlock, a locked pointer's `cursor_hint` is honored.
- Xwayland translates X11 grabs (Wine, SDL) into these same protocols; no
  X-specific handling.

## External decoration themes (`decoration/theme.cpp`, `ipc/theme_watcher.cpp`)

Config `[Theme] dir=` + `name=` selects a layered theme: `<dir>/<name>/theme.conf`
(`[General] parent_themes`, comma list), then `<dir>/<layer>/biome.css` and
`shared.css` concatenated for parents + the theme itself. Parsing (layer chain,
`[Palette]` -> `QPalette`, CSS concatenation) is the header-only `forest-theme`
library, built from the Forest repo and shared with `fstyleloader` (build
dependency `forest-theme-devel`, no runtime dependency). Layers without
`biome.css` are skipped; no usable layer, or keys unset → compiled-in
`builtin.qss`. An external theme replaces the built-in one, it isn't layered
over it.

The theme's `[Palette]` is applied to the app palette, and `theme.cpp` prepends
palette-derived QSS defaults (currently the snap/Alt-Tab highlight, from
`highlight`) that a layer's `biome.css` can override. The built-in theme keeps
Qt's default palette and its own hardcoded highlight.

Live reload: `theme_watcher` watches `~/.config/Biome/` (atomic-rename writes),
re-reads config fresh (`load_biome_config()`), and only on a `Theme/*` change
calls `reload_decoration_theme()`: restyle, re-layout every frame, keep each
window's content origin fixed, re-render, then `toplevels_relocate_for_layout`
to re-fit maximized/fullscreen windows. Drop-in (`/etc`) changes aren't watched.

### Window menu (`decoration/menu.cpp`, `desktop/window_menu.cpp`)

A real `QMenu` rendered offscreen like the switcher (transparent `MenuRoot`
parent, `Qt::Widget` flags, hover via `setActiveAction`); the compositor does
all input. Its QSS comes from each theme layer's `shared.css`, the same file
Forest's `fstyleloader` appends to every app's stylesheet, so menu styling is
defined once (anything else both should style identically goes there too). `theme.cpp` loads `shared.css` after `biome.css` per layer; an
external theme without one gets unstyled menus. `builtin.qss` carries a copy.

- Modal: while open, `cursor.cpp` and `input.cpp` consume pointer/keyboard
  input before anything else. A release activates only after a press landed
  inside the menu, so the opening click's release can't trigger an item; that
  release is still forwarded to the seat if the seat saw the press.
- Move/Resize from the menu warp the cursor onto the titlebar / bottom-right
  corner and set `grab_from_menu`: no button is held, so the next press ends
  the grab.
- `layers.above` holds always-on-top windows (`toplevel_layer()`); fullscreen
  still wins while set.
- Closed on workspace switch, session lock and target unmap/move.
- xdg `show_window_menu` is honored only from the pointer-focused client
  (`WINDOW_MENU` is advertised). Xwayland has no equivalent request.

### Selector contract (renaming any of these breaks shell themes)

| Selector | Widget |
|---|---|
| `biome_decoration--DecorationFrame#biomeFrame` | Whole frame; props `focused`, `urgent`, `biomeMaximized` |
| `QWidget#biomeTitlebar` | Titlebar |
| `QLabel#biomeTitle` | Title text |
| `QToolButton#biomeTitleIcon` | App icon |
| `biome_decoration--DecorationBorder`, `#biomeBorderLeft/Right/Bottom` | Borders |
| `biome_decoration--DecorationButton#biomeButtonMinimize/Maximize/Close` | Buttons (`qproperty-icon`, `:hover`, `:pressed`) |
| `#biomeContent` | Client-area spacer |
| `QWidget#biomeSwitcherRoot`, `QFrame#biomeSwitcherPanel` | Switcher |
| `QToolButton#biomeSwitcherIcon` | Switcher entry; props `selected`, `urgent` |
| `QLabel#biomeSwitcherTitleLabel` | Switcher title |
| `QWidget#biomeMenuRoot`, `QMenu#biomeMenu` | Window menu (styled by plain `QMenu` rules in `shared.css`) |
| `QWidget#biomeSwitcherHighlightRoot`, `QFrame#biomeSwitcherHighlight` | Switcher highlight |

Icon `url()`s must be absolute paths (Qt resolves relative ones against the
process cwd). Built-in `:/biome/decoration/icons/*.svg` are dark-coloured.
