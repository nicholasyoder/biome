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
**Status:** `[x]` done, both sides manually confirmed working. Biome-side
manager + per-toplevel handle landed and confirmed via Waybar's
`wlr/taskbar` module; Forest's `windowlist` plugin ported off
`KX11Extras`/`Xcbutills::*` onto the protocol and confirmed manually,
including a real Biome-side keyboard-focus bug found and fixed along the
way (see session log's last entries - `grant_keyboard_focus_to_non_toplevel()`
in `desktop/toplevel.{h,cpp}`).

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
  activate/maximize/minimize/close actions (**not** set-workspace — see
  session log, no protocol here or in `ext-foreign-toplevel-list-v1` covers
  that; it's Workstream D's territory if anything's ever exposed there at
  all). Resolved the `ext-foreign-toplevel-list-v1` check `docs/plan.md`
  flagged: it's list-only (title/app_id/identifier), no control requests at
  all, so it can't replace `wlr-foreign-toplevel-management-unstable-v1`
  here — see session log for the full comparison.
- Biome-side work: `[x]` done — manager global + per-toplevel handle, wired
  to the existing `BiomeToplevel` list. See session log.
- Forest-side work: replace `KX11Extras` usage with a
  `QWaylandClientExtensionTemplate`-based listener generated via
  `qt_generate_wayland_protocol_client_sources()` against the standard
  `wlr-foreign-toplevel-management-unstable-v1.xml` (see decision 3 —
  no premade wrapper library exists for this one, but this is Qt's own
  documented generator, not hand-rolled `wl_proxy` code); replace every
  `Xcbutills::*` call in `windowbutton.cpp` with the protocol's request
  methods; decide the thumbnail fallback.
- **Later, optional:** also implement `ext-foreign-toplevel-list-v1`
  (identification-only companion global) for compatibility with any client
  that speaks the newer "standard" protocol instead of the wlr-specific
  one — nothing currently needs this (Forest binds the wlr protocol above;
  Waybar's own `wlr/taskbar` module used for this workstream's manual
  testing does too). Genuinely cheap when it's wanted: its API
  (`wlr_ext_foreign_toplevel_list_v1_create`/
  `wlr_ext_foreign_toplevel_handle_v1_create`/`update_state`/`destroy`,
  confirmed present in the installed `wlroots-0.18.2` headers) is a strict
  subset of what `desktop/foreign_toplevel.cpp` already does - no request
  listeners to wire at all, just title/app_id/a generated stable
  `identifier` string, reusing the exact same `toplevel_map`/
  `toplevel_unmap`/`set_title` hook points already in place. Not scheduled;
  add it if a concrete client that needs it ever comes up.

### C — Global hotkeys via `org.freedesktop.portal.GlobalShortcuts`
**Status:** `[x]` done, both sides implemented and manually confirmed
end-to-end by the user (2026-08-31): normal combos, media keys, the bare
`Meta` tap, `pauseHotkeys`/`resumeHotkeys` during a settings-UI key
capture, and `reloadhotkeys()` all work. Step 1 (Biome-side prototype),
step 2 (real `xdg-desktop-portal`/`portals.conf` broker wiring), and step 3
(`forest/`-side `foresthotkeys.cpp`/`hotkey.cpp` port off `XGrabKey`, plus
the Biome-side `impl.portal.Session` object and modifier-only-trigger
companion pieces it depends on) are all built. See
`docs/phase4-session-log.md`'s dated entries for the full writeup,
including three bugs the manual pass caught and fixed.

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
**Status:** `[x]` done — both sides implemented, building clean (zero
warnings), and manually confirmed on real Wayland/Biome, including the
multi-window creation-order-pairing stress test (several terminals opened
in quick succession via the hotkey launcher, moved between desktops, and
closed — no desync). Two bugs found in the first manual pass (windowlist
showing all desktops instead of just the active one; deskswitch's dot
indicator going stale on last-window-close) and fixed same day, plus a
follow-up to make the "move to desktop" menu match the old X11 UI exactly
(item order/wording/icon, "Desktop N" labels). **Depends on:** A (done).
See `phase4-session-log.md`'s 2026-09-05 entries for the full
implementation writeup and both bugfixes.

**Phase 4 is now fully done** — Workstreams A, B, C, and D are all
implemented and manually confirmed on both sides.

- Current (X11): Pure raw-XCB EWMH, no KWindowSystem — reads
  `_NET_NUMBER_OF_DESKTOPS` once, tracks `_NET_CLIENT_LIST` +
  per-window `_NET_WM_DESKTOP` (for the per-desktop dot indicators) and
  `_NET_CURRENT_DESKTOP` (active highlight) via the shared root XCB filter
  (decision 2 above). Switching sends a `_NET_CURRENT_DESKTOP` ClientMessage
  that xfwm4 listens for. No per-window "move to desktop N" logic lives here
  — that's in windowlist's context menu.
- **Resolved 2026-09-05 — hybrid `ext-workspace-v1` + a narrow `org.biome`
  DBus addition.** Full research: `ext-workspace-v1` has no wlroots
  server-side helper (unlike `wlr_layer_shell_v1.h`/
  `wlr_foreign_toplevel_management_v1.h`, which did the heavy lifting for
  Workstreams A/B) — it's hand-rolled server-side from the raw XML (three
  interfaces: manager/group/handle), the highest from-scratch effort of any
  Phase 4 protocol. It fully covers switching/listing, and its wire format
  is inherently dynamic-count (`workspace`/`removed` events, no fixed
  number baked in) — adopting it forces `kWorkspaceCount` to become a
  runtime `BiomeServer::workspace_count` field instead of a compile-time
  constant, which is a deliberate improvement, not incidental.

  But it has **no toplevel↔workspace linkage at all** — checked both
  `ext-workspace-v1.xml` and `wlr-foreign-toplevel-management-unstable-v1.xml`
  (Workstream B's protocol); neither ties a window to a workspace. That
  breaks two real Forest features with no standard-protocol path: the
  per-desktop window-count dots and windowlist's "move to desktop" menu
  item (currently stubbed — see `windowbutton.cpp`'s `desk_menu` comment).
  Decided with the user: use `ext-workspace-v1` for everything it covers,
  and add one small `org.biome.Workspaces` DBus interface (in `ipc/`,
  alongside Workstream C's `GlobalShortcutsPortal`) purely for the
  toplevel↔workspace relationship — the same "justify the exception, don't
  make it the default" bar the `GlobalShortcuts` portal choice was held to,
  just landing on the DBus side for this one narrow piece instead.

  A second finding falls out of that: correlating a `windowlist` toplevel
  (identified via its `wlr_foreign_toplevel_handle_v1` object, no stable
  string identity) with the new DBus interface's toplevel argument needs a
  shared identifier. `ext-foreign-toplevel-list-v1` — Workstream B's
  "later, optional, add it if a concrete client needs it" companion
  protocol — is exactly that (wlroots helper confirmed present,
  `wlr_ext_foreign_toplevel_list_v1.h`, auto-generates a stable
  `identifier` string per toplevel). A concrete need now exists, so it's
  being adopted now instead of staying deferred. The two protocols have no
  cross-reference on the wire, so Biome creates both handles for a toplevel
  back-to-back in `foreign_toplevel_create()` and Forest's windowlist pairs
  them by creation-order arrival — the same approach other wlr-ecosystem
  clients use for this exact gap. **Needs manual multi-window stress
  testing** to confirm the pairing never desyncs, same spirit as the real
  focus bug Workstream B's manual pass caught.
- Biome-side work: `workspace_count` field (`core/server.h`,
  `desktop/workspace.{h,cpp}`); new `desktop/ext_workspace.{h,cpp}`
  (hand-rolled `ext_workspace_manager_v1` server, single group spanning all
  outputs, only `activate` capability advertised — no dynamic
  create/remove/assign, matching Biome's fixed-policy identity);
  `desktop/foreign_toplevel.cpp` extended to also create/destroy a
  `wlr_ext_foreign_toplevel_handle_v1` alongside the existing
  `wlr_foreign_toplevel_handle_v1`; new `ipc/workspace_bridge.{h,cpp}`
  exporting `org.biome.Workspaces` (`GetWindowWorkspaces`,
  `WindowWorkspacesChanged`, `MoveToplevelToWorkspace`) — identifier ->
  workspace index for every open window, not just aggregate counts, so
  windowlist can filter its own button list down to the active workspace
  (deskswitch tallies the same map into per-desktop counts client-side).
- Forest-side work: `deskswitch` rewritten onto an `ext-workspace-v1`
  client binding (`QWaylandClientExtensionTemplate`, same mechanism as
  Workstream B) for switching/listing/active-highlight, plus a
  `QDBusInterface` to `org.biome.Workspaces` for the per-desktop dot
  counts (the XCB atom reads/filter go away entirely); `windowlist` gets a
  small `ext-foreign-toplevel-list-v1` binding for stable identifiers and
  wires its stubbed `desk_menu` to `MoveToplevelToWorkspace`.

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

All four workstreams are done — both sides landed and manually confirmed
for each. Phase 4 is complete.

1. ~~**A** (layer-shell) — foundational, unblocks B and D.~~ done.
2. ~~**B** (windowlist/foreign-toplevel) — unblocked by A.~~ done.
3. ~~**C** (hotkeys/portal) — independent, can run any time.~~ done, manually
   confirmed.
4. ~~**D** (workspaces) — unblocked by A.~~ done, manually confirmed
   2026-09-05 (including the multi-window creation-order-pairing stress
   test).

## Open questions log

- [x] Workspace protocol: `ext-workspace-v1` vs Biome-specific DBus — see
      Workstream D. Decided 2026-09-05: hybrid, `ext-workspace-v1` for
      switching/listing plus a narrow `org.biome.Workspaces` DBus addition
      for the toplevel↔workspace linkage no standard protocol covers.
- [x] Windowlist thumbnail fallback once client-window pixel capture is
      gone — see Workstream B. Decided 2026-08-23: icon-only for now
      (`imagepopup.cpp`'s popup/timer/positioning/shadow scaffolding kept
      intact, only the X11 capture call swapped out), with a live
      `wlr-screencopy` capture of currently-mapped windows left as a
      possible later upgrade — not scheduled, no Biome-side `wlr-screencopy`
      support exists yet either.

## Session log

Moved to `phase4-session-log.md` (split out 2026-08-26 once this file grew
past ~900 lines of session history). A couple of entries there were also
condensed relative to what was originally written in-session, where the
condensed version already captures everything of lasting value — the
multi-round windowlist stale-focus debug saga (2026-08-23) and the
Qt-event-loop-pump follow-up audit (2026-08-26). Append new dated entries
to that file going forward, not here — same convention as before: one
entry per work session, what was decided/built, what's still open, what
to pick up next time.

Latest status as of that file's last entry (2026-09-05): Workstreams A, B,
C, and D are all done and manually confirmed on both sides. **Phase 4 is
complete.**
