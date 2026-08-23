# Phase 4 — Forest Shell Integration — Progress Tracker

Detailed, session-spanning tracker for `docs/plan.md`'s Phase 4 ("Forest shell
integration"). `docs/plan.md` stays the authoritative source for phase
*numbering* and the one-paragraph summary; this file is where the actual
week-to-week/session-to-session work is broken down and checked off, since
Phase 4 is large enough (and spans both `forest/` and `biome/`) that it
doesn't fit in one sitting.

Started 2026-08-22. Nothing implemented yet — this is the planning pass.

**Status legend:** `[ ]` not started · `[~]` in progress · `[x]` done ·
`[?]` blocked on an open question below.

## Scope (narrowed 2026-08-22)

Phase 4 is now the four genuine *ports* only — real, working X11 features
in Forest with a clear Wayland-protocol target: panel/desktop struts
(layer-shell), windowlist (foreign-toplevel-management), deskswitch
(workspace protocol, still undecided), and global hotkeys (portal).

Screenshots, the session locker, and display settings were originally
scoped into this phase (as workstreams E/F/G below) but were split out into
a new **Phase 6 — New capabilities** in `docs/plan.md` once a full audit of
`forest/`'s actual X11 call sites (2026-08-22) found none of the three
exist as Forest features today at all — no screenshot tool (only an
incidental `xcb_image_get()` used for windowlist thumbnails, and an
unrelated `QScreen::grabWindow` used for a fade-transition effect), no
lock-screen client, no multi-monitor settings plugin. Building them is
net-new app design/build work with no dependency forcing it into the same
phase as the four ports above, which are what actually block Phase 5
cutover. See `docs/plan.md`'s Phase 6 entry for that scope now — this file
tracks Phase 4 only.

## Cross-cutting decisions to resolve before/early in Workstream A

These affect how *every* workstream below gets built, so resolve them first
rather than discovering the answer mid-workstream.

1. **`[x]` Backend strategy — resolved 2026-08-22: hard switch, not
   dual-backend.** Each workstream below replaces its X11 mechanism outright
   with the Wayland equivalent — no runtime-selectable abstraction layer, no
   `XDG_SESSION_TYPE`-branched implementations, no keeping the X11 code path
   working alongside the new one. `library/xcbutills` call sites in
   `panel/`, `desktop/`, and `services/services-app/hotkeys/` get replaced
   directly, workstream by workstream, not routed behind an interface with
   two backends. (Revises this file's original framing, which read
   `docs/plan.md`'s old Phase 5 wording as implying a live dual-backend was
   needed — that wording has since been corrected there too.)
2. **`[x]` Event-delivery pattern — resolved 2026-08-22: no shared
   dispatcher needed.** Today, `forest/forest/forestxcbeventfilter.h`
   installs one `QAbstractNativeEventFilter` that snoops every raw XCB
   event and fans it out to any plugin whose `getpluginfo()["needsXcbEvents"]`
   is true (`panel/panel-app/panel.cpp`) — windowlist, deskswitch, and
   hotkeys all ride this one firehose today because raw X11 events have no
   built-in per-component routing. That constraint doesn't carry over: every
   mechanism below delivers events as native Qt signals once bound the
   standard way (see decision 3), straight to whichever component created
   that binding. No replacement for the shared filter needs designing —
   each workstream just connects to its own binding's signals directly, no
   further decision required.
3. **`[x]` qtwayland/Qt protocol-coverage audit — resolved 2026-08-22,
   concrete per-workstream findings (researched via web search, not
   assumed):**
   - **Layer-shell (Workstream A):** Debian Trixie packages `layer-shell-qt`
     (KDE-maintained, v6.3.4-1, in the `kde` component) — a ready-made Qt6
     library wrapping `wlr-layer-shell-unstable-v1` with a clean C++ API
     (`LayerShellQt::Shell::useLayerShell()`, `LayerShellQt::Window`). No
     protocol binding to write — link the library. (Verify at
     implementation time that its `Window::get(QWindow*)` API works cleanly
     with Forest's `QWidget`-based panel via `QWidget::windowHandle()` — a
     small check, not an open design question.) `xdg-output-unstable-v1` is
     likely already consumed transparently by qtwayland's own QPA platform
     plugin for `QScreen` name/geometry — confirm at implementation time
     rather than assuming a separate binding is needed.
   - **Foreign-toplevel-management (Workstream B):** No premade Qt/KDE
     wrapper exists (KDE's own KWin maintainers pushed back on implementing
     `wlr-foreign-toplevel-management` there in favor of their own
     `plasma-window-management` protocol, and that stance seems to extend
     to not building consumer-side tooling for it either). Qt6 does have an
     official, documented path for exactly this case:
     `qt_generate_wayland_protocol_client_sources()` (CMake function) +
     `QWaylandClientExtensionTemplate` run against the standard
     `wlr-foreign-toplevel-management-unstable-v1.xml` (the same upstream
     file Biome already vendors server-side) — Qt's own generator, not
     hand-rolled `wl_proxy` code. Events arrive as ordinary Qt signals.
   - **Hotkeys (Workstream C):** Not a Wayland-protocol question at all —
     `org.freedesktop.portal.GlobalShortcuts` is a DBus portal interface.
     Plain `QtDBus` (`QDBusInterface`/`QDBusConnection`), the same pattern
     `foresthotkeys.cpp` already uses for `org.forest`, is the entire
     mechanism — no Wayland binding involved.
   - **Workspaces (Workstream D):** Whichever way that protocol decision
     lands, the binding mechanism is already settled by B's or C's
     precedent above (client-extension signals for `ext-workspace-v1`,
     plain QtDBus for a Biome-specific interface) — no separate research
     needed once D's protocol choice is made.

     (A `libkscreen` finding relevant to the display-settings item — moved
     to Phase 6 — lives in `docs/plan.md`'s Phase 6 entry, not here.)

## Workstreams

### A — Layer-shell for panel & desktop (+ xdg-output)
**Status:** `[x]` done, fully manually confirmed on both sides, including a
bare-metal multi-monitor pass (the first for either side of this
workstream) that caught and fixed a real Biome-side layer-shell
reconfigure-storm bug (see session log's last entry). Biome-side
foundation built, clean-compiled,
and manually confirmed working (2026-08-22): `swaybg` (background layer),
`waybar` (exclusive-zone bar, correctly structurally on top of windows once
its own config sets `layer: top` - see session log), layer-shell popups
(Waybar's power-menu/clock-tooltip), and a `swaylock` re-test all passed.
`usable_area` is now wired into toplevel placement/maximize
(`desktop/toplevel.cpp`, see session log) - built clean and manually
confirmed working. Forest-side work (linking `layer-shell-qt`, replacing
the XCB strut/window-type calls below) also landed same day (see session
log) - full clean rebuild of the whole `forest` tree passed, zero errors,
no new warnings. **Blocks:** B, D (windowlist and deskswitch are panel
plugins — the panel process itself must be a Wayland client before either
can run at all) - both now unblocked.

- Current (X11): `panel/panel-app/panel.cpp` sets
  `Qt::WA_X11NetWmWindowTypeDock` (Qt-abstracted EWMH hint); actual strut
  reservation is raw XCB in `panel/panel-app/geometrymanager.cpp` →
  `Xcbutills::setPartialStrut()` (`library/xcbutills/xcbutills.cpp`,
  `_NET_WM_STRUT_PARTIAL`, top/bottom only — left/right code path exists but
  is commented out). Desktop background:
  `desktop/desktop-app/wallpaperwidget.cpp` sets
  `Qt::WA_X11NetWmWindowTypeDesktop`, one per `QScreen`
  (`desktop/desktop-app/desktop.cpp`).
- Target (Wayland): `wlr-layer-shell-unstable-v1` — panel → top or overlay
  layer with an exclusive zone (replaces strut reservation structurally);
  desktop background → background layer. `xdg-output-unstable-v1` bundled
  in since layer-shell clients commonly need per-output name/logical
  geometry.
- Biome-side work: `wlr_layer_shell_v1` global + `xdg_output_manager_v1`.
  Per `docs/plan.md`'s Phase 4 follow-up note (added 2026-08-22): build the
  **real persistent per-output layer stack** now (mirrors sway's
  `sway_output::layers`: background/bottom/normal-toplevels/top/overlay/
  session-lock, each a `wlr_scene_tree` created once at output-init in fixed
  order) — this is what layer-shell needs structurally anyway, and doing it
  now lets the Phase 3.5 session-lock `session_locked` runtime checks
  (`desktop/workspace.cpp`, `desktop/xwayland_shell.cpp`,
  `desktop/session_lock.cpp`) be deleted in favor of structural z-order, per
  that same note.
- Forest-side work: link `layer-shell-qt` (see decision 3). Replace
  `Xcbutills::setPartialStrut()` call sites in `geometrymanager.cpp` with
  `LayerShellQt::Window`'s exclusive-zone API; replace the two
  `WA_X11NetWmWindowType*` hints with the layer-shell role/layer the window
  is assigned via the same API.

### B — `wlr-foreign-toplevel-management-unstable-v1` for windowlist
**Status:** `[ ]` not started. **Depends on:** A.

- Current (X11): `panel/panel-plugins/windowlist/windowlist.cpp` uses KDE's
  `KX11Extras` (KWindowSystem) signals — `windowAdded`/`windowRemoved`/
  `windowChanged`/`currentDesktopChanged` — seeded from `KX11Extras::windows()`,
  *not* the raw XCB filter. Filtering via `KWindowInfo`/`NET::WindowTypeMask`.
  Per-window icon/desktop still raw XCB (`Xcbutills::getWindowIcon`/
  `getWindowDesktop`, `_NET_WM_ICON`/`_NET_WM_DESKTOP`). Click → activate via
  `Xcbutills::raiseWindow()` (`_NET_ACTIVE_WINDOW` ClientMessage). Context
  menu (maximize/minimize/close/move-to-desktop) → distinct EWMH
  ClientMessages (`_NET_WM_STATE`, `WM_CHANGE_STATE`, `_NET_CLOSE_WINDOW`,
  `_NET_WM_DESKTOP`). Thumbnail popup (`imagepopup.cpp`) does raw
  `xcb_image_get()` client-window pixel capture — **no Wayland equivalent
  exists** (compositor-side output capture only; can't screenshot an
  arbitrary, possibly-unmapped/minimized client surface). Thumbnail will
  need to fall back to icon-only, or a live `wlr-screencopy` capture while
  the window is actually mapped/visible (design question, not yet decided).
- Target (Wayland): `wlr-foreign-toplevel-management-unstable-v1` maps
  cleanly onto KX11Extras' add/remove/changed signals and the
  activate/maximize/minimize/close/set-workspace actions. Check
  `ext-foreign-toplevel-list-v1` availability/preference on wlroots 0.18 per
  `docs/plan.md`'s existing table note.
- Biome-side work: implement the manager global + per-toplevel handle,
  wired to the existing `BiomeToplevel` list.
- Forest-side work: replace `KX11Extras` usage with a
  `QWaylandClientExtensionTemplate`-based listener generated via
  `qt_generate_wayland_protocol_client_sources()` against the standard
  `wlr-foreign-toplevel-management-unstable-v1.xml` (see decision 3 —
  no premade wrapper library exists for this one, but this is Qt's own
  documented generator, not hand-rolled `wl_proxy` code); replace every
  `Xcbutills::*` call in `windowbutton.cpp` with the protocol's request
  methods; decide the thumbnail fallback.

### C — Global hotkeys via `org.freedesktop.portal.GlobalShortcuts`
**Status:** `[ ]` not started. **Independent** of A — can be built and
tested in parallel, doesn't require the panel to be a Wayland client.

- Current interface (already transport-agnostic, good news): `foresthotkeys`
  exports DBus object `/org/forest/hotkeys` with slots `reloadhotkeys()`,
  `showdesktop()`, `pauseHotkeys()`, `resumeHotkeys()`. Driven purely by
  `QSettings("Forest","Forest")` group `hotkeys/` — each entry a
  `QKeySequence` + an action (`DBUS:...` or shell command). **No in-process
  registration API exists** for other components — it's config-driven only.
  This surface itself needs no redesign; only `hotkey.cpp`'s `XGrabKey`
  internals (`qxt/`) and `showdesktop()`'s `_NET_SHOWING_DESKTOP`
  ClientMessage need replacing.
- Target: Biome implements `org.freedesktop.portal.GlobalShortcuts`
  (matching the GNOME/KDE portal interface — see `docs/plan.md`'s
  Decoupling goal); Forest's hotkey client binds registrations from
  `hotkeys/` config against that portal instead of `XGrabKey`.
- Open item from `docs/plan.md`'s "Open risks": the portal's exact
  binding-registration/conflict-handling semantics aren't yet validated
  against Biome's architecture — **prototype this early**, it's flagged as
  the least-understood piece of the whole phase.
- Biome-side work: `ipc/` doesn't exist yet in the tree — this is the first
  thing to land in it. Implement the portal DBus interface
  (`RequestBindShortcuts`/`Activated`/`Deactivated` per the real
  `org.freedesktop.portal.GlobalShortcuts` spec), backed by
  `core/input.cpp`'s existing keybinding dispatch.
  `showdesktop()`'s target behavior (minimize-all or similar) needs a
  Biome-side equivalent too — check what `_NET_SHOWING_DESKTOP` triggers via
  `handle_keybinding()`/workspace logic today.
- Forest-side work: replace `qxt/` entirely; rewrite `foresthotkeys.cpp`'s
  registration loop against the portal's DBus calls instead of constructing
  `globalhotkey` objects that call `XGrabKey`. No Wayland protocol binding
  needed here at all (see decision 3) — plain `QtDBus`, the same pattern
  `foresthotkeys.cpp` already uses for its own `/org/forest/hotkeys` object.

### D — Workspaces / deskswitch
**Status:** `[ ]` not started. **Depends on:** A. **Blocked on a protocol
decision** — this is the trickiest workstream in the phase.

- Current (X11): Pure raw-XCB EWMH, no KWindowSystem — reads
  `_NET_NUMBER_OF_DESKTOPS` once, tracks `_NET_CLIENT_LIST` +
  per-window `_NET_WM_DESKTOP` (for the per-desktop dot indicators) and
  `_NET_CURRENT_DESKTOP` (active highlight) via the shared root XCB filter
  (decision 2 above). Switching sends a `_NET_CURRENT_DESKTOP` ClientMessage
  that xfwm4 listens for. No per-window "move to desktop N" logic lives here
  — that's in windowlist's context menu.
- **`[?]` Open question:** wlroots/Wayland core has no virtual-desktop
  concept at all; `docs/plan.md`'s protocol table was missing a row for
  this (fixed 2026-08-22). Biome already has its own internal
  4-workspace model (`desktop/workspace.h`, `kWorkspaceCount`,
  `switch_workspace()`, `move_toplevel_to_workspace()`), it just has no
  external protocol exposing it yet. Two directions, need a decision before
  implementing: (a) `ext-workspace-v1` — the closest thing to a standard
  protocol, but still unstable/draft and needs checking against wlroots
  0.18's actual support; picking it fits the Decoupling goal's
  standard-protocols-first bias. (b) a Biome-specific DBus interface under
  `org.biome` (the `ipc/` module) — simpler, guaranteed to work, but a
  deliberate exception to the decoupling default and needs justifying the
  same way the plan already justifies the `GlobalShortcuts` portal choice
  going the *other* way. Given `docs/plan.md`'s explicit stance ("treat a
  Biome-specific or Forest-specific shortcut as something to justify, not
  the default"), default assumption should be to seriously evaluate (a)
  first and only fall back to (b) with a documented reason, mirroring how
  the hotkey decision was made.
- Biome-side work: depends on the decision above.
- Forest-side work: replace the three EWMH atom read paths and the
  `_NET_CURRENT_DESKTOP` ClientMessage send with the chosen protocol/DBus
  equivalent.

## Phase 6 (net-new capabilities — tracked in `docs/plan.md`, not here)

Screenshots, the session locker UI, and display settings moved to a new
Phase 6 in `docs/plan.md` — see that file's Phase 6 entry for scope. One
note carried over here since it's directly relevant to this phase's own
sequencing: a session-lock client is a much simpler Wayland client than the
panel (fullscreen, single-purpose), so it's worth considering as an early
pilot for whatever Forest-side Wayland-client plumbing Workstream A
establishes — even though it's Phase 6 work, building it early
(interleaved with Phase 4 rather than strictly after) could de-risk
Workstream A.

## Suggested sequencing

Workstream A is done (both sides landed, pending the user's manual retest of
the Forest half). B and D are now unblocked.

1. ~~**A** (layer-shell) — foundational, unblocks B and D.~~ done.
2. **C** (hotkeys/portal) — independent, can run any time, not started yet.
3. **B** (windowlist/foreign-toplevel) and **D** (workspaces) — unblocked by
   A; D is gated on its own protocol decision, so may slip behind B.

## Open questions log

- [ ] Workspace protocol: `ext-workspace-v1` vs Biome-specific DBus — see
      Workstream D.
- [ ] Windowlist thumbnail fallback once client-window pixel capture is
      gone — see Workstream B.

## Session log

*(append one dated entry per work session, same convention as the Biome
compositor's own memory log — what was decided/built, what's still open,
what to pick up next time.)*

- **2026-08-22** — Initial planning pass. Audited `forest/`'s actual X11
  call sites across all six of `docs/plan.md`'s table areas plus session
  manager and `xcbutills` broadly; found three of the six areas
  (screenshots, session locker, display settings) are net-new Forest
  features, not ports. Wrote an initial tracker with workstreams A–G,
  three cross-cutting architecture decisions flagged as blocking, and a
  two-tier sequencing.
- **2026-08-22 (same day)** — User asked to split the net-new items
  (screenshots, session locker, display settings) out of Phase 4 entirely,
  since Phase 4 is complicated enough as a port-only phase. Moved them to
  a new **Phase 6 — New capabilities** in `docs/plan.md` (Phase 5 didn't
  fit either — it's cutover/session-exec infrastructure, not feature
  work). This file now tracks only the four real ports (workstreams A–D).
  Also fixed a real gap found along the way: `docs/plan.md`'s protocol
  table had no row for workspaces/deskswitch at all — added one, and added
  workspace-switching to Phase 4's summary paragraph. No code written yet.
- **2026-08-22 (same day)** — Resolved cross-cutting decision 1: hard
  switch from X11 to Wayland, not a dual-maintained backend. Each Phase 4
  workstream replaces its X11 mechanism outright rather than keeping both
  working behind a runtime-selectable abstraction. Updated `docs/plan.md`'s
  Phase 5 wording to match (it previously read as implying X11 stayed
  supported in parallel). Decisions 2 (event-filter replacement pattern)
  and 3 (qtwayland protocol-coverage audit) still open before Workstream A
  can start.
- **2026-08-22 (same day)** — Resolved decisions 2 and 3 via web research
  (not assumed from memory). Decision 2 turned out not to be a real
  decision — Wayland protocols deliver events as Qt signals per-binding, no
  shared dispatcher needed to replace the old XCB filter. Decision 3
  produced concrete, per-workstream findings: `layer-shell-qt` (packaged in
  Debian Trixie) covers Workstream A's protocol with no custom binding
  code; Workstream B has no premade wrapper but Qt's own
  `qt_generate_wayland_protocol_client_sources()` +
  `QWaylandClientExtensionTemplate` mechanism covers it against the
  standard protocol XML; Workstream C needs no Wayland binding at all
  (GlobalShortcuts is DBus-only, plain QtDBus); `libkscreen` already has a
  working `wlr-output-management-unstable-v1` backend, noted for Phase 6.
  Updated each workstream's Forest-side-work bullet with these specifics.
  All three cross-cutting decisions are now resolved — Workstream A is
  unblocked. No code written yet.
- **2026-08-22 (same day) — Workstream A's Biome-side foundation landed
  (planned via a full EnterPlanMode cycle, direct research rather than
  delegated - the scope was well-precedented, mirroring the existing
  session-lock module's shape).** Built the real per-output scene-layer
  stack `docs/plan.md`'s Phase 4 follow-up note called for
  (`BiomeServer::layers` in `core/server.h`: a fixed, ordered
  background/bottom/toplevels/top/overlay/session_lock set of trees,
  created once by new `core/layers.{h,cpp}`), plus `wlr_layer_shell_v1` and
  `wlr_xdg_output_manager_v1` (new `desktop/layer_shell.{h,cpp}` for the
  former; the latter is a single self-contained
  `wlr_xdg_output_manager_v1_create()` call in `output_manager_init()`).
  Toplevel scene trees (`desktop/xdg_shell.cpp`, `desktop/xwayland_shell.cpp`)
  and the override-redirect/unmanaged-surface scene tree
  (`desktop/xwayland_shell.cpp`) were reparented from `server->scene->tree`
  directly onto `server->layers.toplevels`; `desktop/session_lock.cpp`'s
  `lock_tree` now points at `server->layers.session_lock` (created by
  `core/layers.cpp` instead of by session_lock itself). Because
  `layers.session_lock` is structurally the last of the six fixed trees,
  the three Phase 3.5 `session_locked` runtime checks the follow-up note
  said would become obsolete were deleted:
  `update_toplevel_visibility()`'s clause (`desktop/workspace.cpp`), the
  override-redirect map handler's disable-instead-of-raise branch
  (`desktop/xwayland_shell.cpp`), and both toplevel-visibility sweeps in
  `desktop/session_lock.cpp`'s lock/unlock handlers (now dead code once the
  visibility clause they were re-deriving no longer exists). Kept (not
  deleted) `focus_toplevel()`'s own `session_locked` early-return
  (`desktop/toplevel.cpp`) - its real job was always preventing a locked
  session from losing keyboard focus to a newly-focused toplevel, a
  seat-level concern the structural z-order change doesn't touch, not the
  z-order concern its old comment described; comment corrected to say so.
  Applied the identical fix to two other keyboard-focus grabs found by the
  same reasoning that would otherwise have silently regressed: the
  override-redirect map handler's focus grab and
  `desktop/layer_shell.cpp`'s own keyboard-interactivity grab both gained
  `!session_locked` guards, since removing their *old*, structurally-now-
  redundant checks would otherwise have dropped the keyboard-focus-stealing
  protection bundled inside them along with the redundant part.

  Also handled layer-surface popups: `zwlr_layer_surface_v1.get_popup`
  associates an already-created, still-parentless `xdg_popup` with its
  layer surface (confirmed by reading wlroots'
  `types/wlr_layer_shell_v1.c` directly, not assumed) - `desktop/
  xdg_shell.cpp`'s existing global `new_popup` handler
  (`server_new_xdg_popup`) would otherwise hit a null-parent assert the
  first time any real layer-shell client (e.g. a `waybar` tray/menu module)
  created one, a latent crash this feature made reachable for the first
  time. Guarded the scene-node-creation half of that handler on
  `xdg_popup->parent != nullptr` (commit/destroy listeners still always
  attached) and added a `new_popup` listener on `desktop/layer_shell.cpp`'s
  own per-surface wrapper to supply the scene node once the parent is
  actually known.

  Corrected one claim from the approved plan during implementation: the
  plan assumed layer-shell/xdg-output would need no new wayland-scanner
  protocol codegen (reasoning from session-lock's precedent, which needed
  none). `wlr_xdg_output_v1.h` bore that out, but `wlr_layer_shell_v1.h`
  turned out to `#include` a generated `wlr-layer-shell-unstable-v1-
  protocol.h` that no installed system package ships (confirmed via
  `dpkg -L`/`find` - unlike `xdg-shell.xml`, this protocol's XML lives only
  in wlroots' own source tree, not the `wayland-protocols` package). Vendored
  a copy from the local wlroots checkout into a new `protocol/` directory
  (the layout `docs/plan.md`'s "Repo layout" section already reserved for
  exactly this) and extended `cmake/BiomeProtocols.cmake`'s existing
  `biome_generate_protocol_header()` machinery to generate it, mirroring
  xdg-shell's own setup exactly.

  A second, unrelated build break surfaced once that header did generate:
  both it and the real `wlr_layer_shell_v1.h` use `namespace` as a C
  identifier (a field/arg name, legal in C, a reserved keyword in C++) -
  the exact class of problem `cmake/BiomeWlrootsShim.cmake` already exists
  to patch (its header comment documents the precedent: `xwayland.h`'s
  `class` field, renamed `class_`). Extended that shim to also patch
  `wlr_layer_shell_v1.h` (`namespace` → `namespace_`), and fixed the
  vendored protocol XML's own `namespace` arg name at the source (renamed
  to `namespace_`, with a comment explaining why - arg names aren't part of
  the wire protocol, so this doesn't change what wire-compatible clients
  see).

  Deliberately deferred to the next step in this workstream (documented in
  the approved plan, not forgotten): wiring `BiomeOutput::usable_area` (now
  computed and stored by `arrange_layers()`, but unconsumed) into toplevel
  placement/maximize, so a maximized window stops overlapping a panel's
  reserved exclusive-zone space. All of `forest/`'s side (linking
  `layer-shell-qt`, replacing the XCB strut/window-type calls) is also
  still untouched, per this workstream's own sequencing.

  Full clean rebuild (`rm -rf build`, fresh configure+build), zero
  warnings, zero errors.

  **Manual test pass, same day:** user confirmed `swaybg`, `waybar`, and
  layer-shell popups (Waybar's power-menu and clock-tooltip) all worked.
  One real finding, not a Biome bug: Waybar's stock Debian config leaves
  `layer` commented out, and its own compiled-in default for that turned
  out to be `bottom`, not the `top` the comment's placeholder value implied
  - so windows correctly rendered over it per the protocol's own layer
  semantics (a bottom-layer surface is *meant* to sit behind normal
  windows). Root-caused with one temporary `wlr_log` line logging each new
  layer surface's resolved `namespace`/`current.layer`
  (`desktop/layer_shell.cpp`, added and removed same session once
  confirmed, same convention as this file's Phase 3.5 debug-logging
  passes) rather than by continuing to reason about it from code alone -
  confirmed `layer=1` (`bottom` per the vendored protocol XML's own
  `background=0/bottom=1/top=2/overlay=3` enum) instead of the expected `2`.
  Fixed by setting `"layer": "top"` in `~/.config/waybar/config.jsonc`
  (copied from the stock `/etc/xdg/waybar/config.jsonc`, since the user had
  no config of their own yet) - user confirmed Waybar now stays above
  windows including maximized ones. Re-tested `swaylock` too, confirmed
  still blocking as expected under the new structural stack. Workstream A's
  Biome-side foundation is now verified end-to-end for everything in this
  step's scope.

- **`usable_area` wired into toplevel placement/maximize, same day
  (built clean, not yet manually tested).** Planned via a full
  EnterPlanMode cycle plus one AskUserQuestion (scope: fix both maximize
  *and* new-window cascade placement, not maximize alone - user picked
  both, since it's the same underlying gap and cheap once the lookup
  helper exists). `desktop/toplevel.cpp`'s `maximize_target_box()` and
  `place_new_toplevel()`'s no-parent cascade branch both previously sized/
  positioned against the raw output box, ignoring any panel's exclusive
  zone. New static `output_target_box()` (`desktop/toplevel.cpp`) converts
  `BiomeOutput::usable_area` (output-local, as `arrange_layers()` needs it
  for `wlr_scene_layer_surface_v1_configure()`) into the same global
  layout coordinates `wlr_output_layout_get_box()` already uses, with a
  fallback to the full output box if the output can't be resolved or
  `usable_area` is degenerate. `maximize_target_box()` now fills that box
  instead of the raw output; `place_new_toplevel()`'s cascade math is
  unchanged but its result is now clamped into it. Also extracted
  `biome_output_from_wlr()` (`core/output.h`/`.cpp`) - the "find the
  `BiomeOutput*` for a `wlr_output*`" linear scan was already hand-
  duplicated in `desktop/session_lock.cpp` and `desktop/layer_shell.cpp`;
  this step's third need for it was the point past which naming it was
  worth doing, so both existing call sites were switched over too (no
  behavior change). Deliberately left out of scope: live re-flow of an
  already-maximized window if a panel's exclusive zone changes at runtime
  (rare, non-trivial, flagged for later if needed), and redesigning
  `place_new_toplevel()`'s existing multi-monitor whole-layout-box cascade
  algorithm itself (a separate, pre-existing rough edge). Full clean
  rebuild (`rm -rf build`, fresh configure+build), zero warnings, zero
  errors. **Manually confirmed working, same day:** user tested with
  Waybar (`layer: top`) - new windows no longer cascade in under the bar,
  maximize now fills the space below it, and un-maximize restore geometry
  is unaffected.

- **Workstream A's Forest-side landed, same day (planned via a full
  EnterPlanMode cycle, then implemented directly in `forest/`).** Installed
  `liblayershellqtinterface-dev` (Trixie package `liblayershellqtinterface6`
  was already present at runtime, dev headers were not); confirmed it's a
  plain CMake config package (`find_package(LayerShellQt REQUIRED)`, target
  `LayerShellQt::Interface`, no `.pc` file - the plan's guess of
  `pkg_check_modules` was wrong, corrected during implementation) and that
  `LayerShellQt::Window` has no `setSize()` - sizing stays ordinary Qt
  widget sizing, LayerShellQt only ever controls anchors/exclusive-zone/
  layer/keyboard-interactivity/scope/screen. Also found the vendored header
  doesn't call `Q_DECLARE_OPERATORS_FOR_FLAGS` for `Anchors`, so
  `Anchor | Anchor` decays to a plain `int` - every anchor combination needed
  an explicit `LayerShellQt::Window::Anchors(...)` functional-style cast to
  compile, not the bare `|` chain the plan assumed.

  Added `find_package(LayerShellQt REQUIRED)` (top `CMakeLists.txt`) and a
  `forest_link_layershellqt(target)` helper (`cmake/ForestDeps.cmake`),
  linked into `forest`, `panel-app`, and `desktop-app`; added
  `liblayershellqtinterface-dev` to `debian/control`'s `Build-Depends`.
  `forest/forest/main.cpp` calls `LayerShellQt::Shell::useLayerShell()`
  right after constructing `QApplication`, before `forest::setup()` creates
  any window - required since `panel-app`/`desktop-app` are
  `QPluginLoader`-loaded into this same process, not separate ones, so one
  process-wide call covers both. No `platformName()` guard, per
  [[feedback_no_runtime_platform_branching]].

  `GeometryManager` (`panel/panel-app/geometrymanager.{h,cpp}`) - the one
  shared code path for both the real panel and the 1px `HiddenPanel`
  autohide strip - now caches a `LayerShellQt::Window*` (forcing
  `panel_widget->winId()` in the constructor so `windowHandle()` is valid),
  sets `LayerTop` + `KeyboardInteractivityOnDemand` once, and
  `update_geometry()` replaces the old `move()` + `Xcbutills::setPartialStrut()`
  calls with `setAnchors()` (top/bottom, both left+right) and
  `setExclusiveZone(reserve_screen_space ? fixed_panel_size : 0)` - one line
  now covers what used to be two separate `setPartialStrut()` branches.
  `panel.cpp`/`hiddenpanel.cpp` had their `WA_X11NetWmWindowTypeDock`
  attribute deleted outright (meaningless once a real layer-shell surface is
  driving the role).

  `desktop/desktop-app/wallpaperwidget.cpp` follows the same pattern
  directly (only one caller): `LayerBackground` layer, anchored to all four
  edges, `setExclusiveZone(0)`, `KeyboardInteractivityNone`. Per-output
  placement needed a real design decision the plan had flagged as
  unprecedented in this codebase: `desktop.cpp`'s old X11-style absolute
  `setGeometry(screen->geometry())` positioning doesn't carry over, since
  `LayerShellQt::Window`'s default `ScreenConfiguration::ScreenFromQWindow`
  reads `QWindow::screen()`, not window position. Resolved by calling
  `wallwidget->windowHandle()->setScreen(screen)` in `desktop.cpp`'s
  existing per-`QScreen` loop (`loadwallpaperwidgets()`) right where the old
  `setGeometry()` call was - confirmed via Qt's own docs that `setScreen()`
  is safe to call after the platform window already exists (it recreates the
  window on the new screen), so no ordering hazard versus the constructor's
  forced `winId()`.

  `Xcbutills::setPartialStrut()` (`library/xcbutills/xcbutills.{h,cpp}`) -
  confirmed via a full-tree grep it had exactly one caller
  (`geometrymanager.cpp`) - was deleted outright once that caller was gone,
  taking the `wayland`-branch `if (!conn) return;` stopgap guard with it:
  the "replaced, not just reverted" resolution `WAYLAND-TESTING-NOTES.md`
  already called for. That file's own scaffolding-item entry was updated to
  reflect this landing. Left untouched, per the plan's stated scope:
  `services/services-app/notifications/notifypopup.cpp`'s own (likely
  pre-existing, unrelated copy-paste bug) `WA_X11NetWmWindowTypeDesktop`
  attribute; the three `"Forest-wayland"` `QSettings` scaffolding overrides
  (still needed until B/C/D land); and `geometrymanager.cpp`'s pre-existing
  primary-screen-only limitation (panel still doesn't support true
  multi-monitor placement).

  Full clean rebuild of the entire `forest` tree (`rm -rf build`, fresh
  configure + `cmake --build -j$(nproc)`), zero errors, no new warnings -
  every target built including the not-yet-ported `windowlist`/`deskswitch`/
  hotkey code (untouched by this workstream, still X11-only, still builds
  fine since B/C/D haven't started). **Not yet manually tested interactively
  by the user** - per [[feedback_manual_interactive_testing]]; needs a real
  nested-Biome session with the panel showing/reserving space, autohide
  toggled, and a multi-monitor wallpaper check.

- **Follow-up fix, same day - user's first bare-metal multi-monitor test
  found a resize feedback loop.** Wallpaper and panel placement/exclusive-
  zone both looked correct, but cursor movement was severely jerky the
  whole time `forest` ran. Root cause: `GeometryManager::update_geometry()`
  (`panel/panel-app/geometrymanager.cpp`) ran reactively off `pframe`'s own
  `resized` signal (`panel::update_panel_size()`, a pre-existing "hacky way
  to make the panel size change when the theme changes" mechanism) as well
  as output-change signals, and on every call re-requested
  `panel_widget->setFixedSize(qApp->primaryScreen()->geometry().width(), ...)`.
  Since the panel is anchored to both left and right edges, Biome's
  `arrange_layers()`/`wlr_scene_layer_surface_v1_configure()`
  (`desktop/layer_shell.cpp`) unconditionally overrides that dimension to
  the actual output's width regardless of what the client requests - and on
  this machine, nothing pins the panel's `QWindow` to a specific `QScreen`
  (unlike `wallpaperwidget`, which does), so it landed on a different output
  than `qApp->primaryScreen()`. Every compositor-driven resize back to the
  correct width retriggered `pframe`'s `resized` signal, which
  re-requested the *wrong* (primary-screen) width again, which the
  compositor corrected again, forever - an infinite configure/commit
  ping-pong between `forest` and `biome`, both serialized on Biome's
  single-threaded event loop, starving input processing badly enough to
  make cursor motion itself stutter. Only reachable when the panel's actual
  output differs in width from `qApp->primaryScreen()`, which is why
  nothing caught it in the single-output nested dev loop.

  Fix: stopped requesting a width in `update_geometry()` at all -
  `panel_widget->setFixedHeight(fixed_panel_size)` only, anchors/exclusive-
  zone otherwise unchanged. This is what the original plan actually called
  for ("leave width to the compositor") before implementation quietly
  regressed it back to an X11-style explicit width once `LayerShellQt::Window`
  turned out to have no `setSize()` API - the fix removes the guessed width
  outright rather than reintroducing it, since the double-anchored dimension
  was always going to be compositor-controlled regardless. Clean incremental
  rebuild of `panel-app`, zero errors.

  **Re-tested by the user, same day: cursor still jerky.** The panel fix
  above was real but not the cause. User bisected by toggling
  `desktop-app`'s `enabled` key off in `~/.config/Forest-wayland/Forest.conf`
  (per `WAYLAND-TESTING-NOTES.md`'s existing convention for disabling app
  plugins) - confirmed Biome alone (no `forest`) was already smooth on this
  bare-metal multi-monitor rig, and that disabling *just* `desktop-app`
  (wallpaper) fixed the jerk with `panel-app` still running - narrowing it
  to `wallpaperwidget.cpp` specifically, not the panel.

  Root cause, found by reading wlroots' own
  `types/scene/layer_shell_v1.c`, `wlr_scene_layer_surface_v1_configure()`
  (local checkout: `/home/nicholas/Misc/wlroots`, not the docs-only
  `misc/wlroots` in this project - the latter has no `.c` sources):
  `exclusive_zone == -1` sizes the surface against `full_area`;
  *any other value, including `0`*, sizes it against `usable_area` (already
  shrunk by higher-priority layers' own exclusive-zone claims - `arrange_layers()`
  processes overlay/top/bottom/background in that priority order). The
  plan's `wallpaperwidget.cpp` implementation set `setExclusiveZone(0)`,
  reasoning "the background doesn't claim space" - true, but `0` *also*
  means "and respect everyone else's claims", so with the panel reserving
  space (autohide off, the default the user was testing), Biome computed
  the background layer's box against a `usable_area` shorter than the full
  output - while `desktop.cpp` separately forces
  `wallwidget->setFixedSize(screen->size())`, the *un*-shrunk full height.
  That's a standing mismatch between what the compositor configures and
  what the client insists on, and since `handle_layer_surface_commit()`
  (`desktop/layer_shell.cpp`) reruns `arrange_layers()` on *every* commit
  from *any* layer surface on the output (not gated on whether anything
  relevant actually changed), this drove continuous reconfigure/recommit
  churn on Biome's single-threaded event loop - the actual mechanism behind
  the jerk, and also, independently, a real (if visually-masked-by-the-
  panel) under-coverage bug: the wallpaper wasn't actually extending behind
  the panel's reserved strip.

  Confirmed against the vendored protocol XML itself
  (`biome/protocol/wlr-layer-shell-unstable-v1.xml`,
  `set_exclusive_zone`'s doc comment): "*a wallpaper or lock screen might
  set their exclusive zone to -1*" - the documented idiom, and how `swaybg`
  itself behaves. Fixed by changing `setExclusiveZone(0)` to
  `setExclusiveZone(-1)` in `wallpaperwidget.cpp`. Clean incremental
  rebuild of `desktop-app`, zero errors.

  **Re-tested by the user, same day: still just as jerky, and it's not
  Forest-side.** User confirmed the actual `biome` compositor process
  itself, not just `forest`, was affected: cursor was jerky even hovering
  over *other windows* covering the wallpaper, not just the bare desktop.
  CPU readings were the real clue and initially looked backwards -
  `biome`/`forest` both showed *lower* CPU with `desktop-app` enabled
  (jerky) than disabled (smooth) - low CPU while visibly stuttering means
  stalled/blocked, not busy-computing, which ruled out every "expensive
  per-event processing" theory tried so far. Confirmed definitively via a
  `WAYLAND_DEBUG=1 forest 2>log` capture: `wl_pointer.motion` delivery
  paused for ~20-45ms stretches, each pause coinciding with a burst where
  *every* mapped layer-shell surface (`#40`/`#48`/`#46`/`#43` - two
  wallpapers, the panel, and a fourth 1920x1080 surface) cycled through
  `wl_buffer.release()` -> `zwlr_layer_surface_v1.configure()` ->
  `ack_configure()` -> `offset`/`attach`/`damage_buffer`/`commit`, on
  repeat, forever - and the configured size was *identical* on every
  cycle (`#40.configure(1216, 1920, 1080)`, then `#40.configure(1222,
  1920, 1080)`, ...), only the serial changing.

  Root cause, this time genuinely in Biome, confirmed by reading
  wlroots' own source directly rather than assumed:
  `wlr_layer_surface_v1_configure()` (`/home/nicholas/Misc/wlroots/types/wlr_layer_shell_v1.c:316` -
  the actual `.c` sources live only in the separate `~/Misc/wlroots`
  checkout; the `misc/wlroots` inside this project has headers only) is
  **unconditional** - it always allocates a new configure entry and always
  sends `zwlr_layer_surface_v1.configure` with a fresh serial, with no
  check for whether the box it was just given differs from the last one.
  wlroots leaves that dedup entirely up to the compositor. `handle_layer_surface_commit()`
  (`desktop/layer_shell.cpp`) called `arrange_layers()`
  unconditionally on *every* commit from *any* layer surface on the output
  - so any surface's ordinary content-only commit (a client just
  repainting) reconfigured every layer surface on that output, even ones
  whose box hadn't changed at all; each of those, receiving a configure,
  acks and recommits (standard client behavior, regardless of whether the
  size actually differs); that recommit retriggers `arrange_layers()`
  again; forever. A self-sustaining reconfigure/recommit storm across
  every layer surface on the output, naturally paced by buffer-
  release/vsync timing rather than a tight busy-loop - which is exactly
  why it stayed cheap on CPU while still monopolizing enough of Biome's
  single-threaded event loop, every ~20-40ms, to visibly starve pointer-
  motion delivery. Present in principle with the panel alone too (same
  unconditional call site), just small enough - one 1920x36 surface
  reconfiguring itself - to be imperceptible; adding the wallpaper's
  full-screen surfaces (and, per the trace, a fourth 1920x1080 surface -
  the second monitor's own wallpaper) to the same storm made it glaring.

  Fixed by gating `handle_layer_surface_commit()`'s call to
  `arrange_layers()` on `wlr_layer_surface_v1_state::committed` actually
  containing a layout-relevant bit (`DESIRED_SIZE`/`ANCHOR`/
  `EXCLUSIVE_ZONE`/`MARGIN`/`LAYER`) - a plain content commit now leaves
  every layer surface on the output untouched. Added one narrow safety net
  alongside it: `handle_layer_surface_map()` now calls `arrange_layers()`
  once at the map transition, since `wlr_scene_layer_surface_v1_configure()`
  only applies a positive `exclusive_zone` to `usable_area` once
  `layer_surface->surface->mapped` is true (confirmed in the same wlroots
  source read), and the newly-gated commit handler can no longer be relied
  on to reliably catch that exact transition on its own. Full clean
  incremental rebuild of `biome`, zero errors, zero warnings. **Confirmed
  fixed by the user** on the same bare-metal multi-monitor rig - cursor is
  smooth with `desktop-app` enabled and the panel reserving space.
  Workstream A (both sides) is now fully done and manually verified,
  including the first-ever bare-metal multi-monitor pass.
