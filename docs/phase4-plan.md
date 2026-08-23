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
**Status:** `[~]` in progress — Biome-side foundation built, clean-compiled,
and manually confirmed working (2026-08-22): `swaybg` (background layer),
`waybar` (exclusive-zone bar, correctly structurally on top of windows once
its own config sets `layer: top` - see session log), layer-shell popups
(Waybar's power-menu/clock-tooltip), and a `swaylock` re-test all passed.
Forest-side work (linking `layer-shell-qt`, replacing the XCB strut/window-
type calls below) not started; `usable_area` → toplevel placement/maximize
wiring (see session log) also still open. **Blocks:** B, D (windowlist and
deskswitch are panel plugins — the panel process itself must be a Wayland
client before either can run at all).

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

All three cross-cutting decisions are now resolved — Workstream A can
start.

1. **A** (layer-shell) — foundational, unblocks B and D.
2. **C** (hotkeys/portal) — independent, run in parallel with A.
3. **B** (windowlist/foreign-toplevel) and **D** (workspaces) — after A;
   D is gated on its own protocol decision, so may slip behind B.

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
