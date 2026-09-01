# Phase 4 — Session Log

Detailed, dated session-by-session log for `phase4-plan.md`'s Phase 4 work
(Forest shell integration). Split out from that file on 2026-08-26 once it
grew past ~900 lines — `phase4-plan.md` stays the authoritative summary of
status/scope/workstreams; this file is the append-only history behind it.

Same convention as before: append one dated entry per work session — what
was decided/built, what's still open, what to pick up next time. A few
entries below have been condensed relative to what was originally written
in-session, where the condensed version already captures everything of
lasting value (the full blow-by-blow, for anything trimmed, is still in
`phase4-plan.md`'s own git history from before the split).

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

- **2026-08-23 — Workstream B's Biome-side foundation landed (planned via a
  full EnterPlanMode cycle scoping just the Biome-side protocol work, no
  `forest/` changes yet - mirrors how Workstream A actually sequenced
  itself).** First resolved the open protocol-choice question from
  `docs/plan.md`'s table: compared `ext-foreign-toplevel-list-v1` (staging,
  present on this system) against `wlr-foreign-toplevel-management-unstable-v1`
  by reading both XMLs directly - the `ext` one is identification-only
  (`title`/`app_id`/`identifier`/`closed`, no requests besides `stop`), while
  `wlr-foreign-toplevel-management-unstable-v1` has the
  `set_maximized`/`set_minimized`/`activate`/`close` requests
  `windowbutton.cpp`'s context menu actually needs, and is confirmed present
  in the installed `wlroots-0.18.2` headers. Decision: implement only the
  `wlr` one. Also found neither protocol has a "move to workspace" concept
  at all - `windowbutton.cpp`'s "Move to desktop" submenu has no home in
  this workstream regardless of protocol choice; it's Workstream D's
  territory if it ever gets exposed anywhere.

  New `desktop/foreign_toplevel.{h,cpp}` (added to `biome_desktop` in
  `desktop/CMakeLists.txt`), same per-object-wrapper shape as
  `desktop/session_lock.{h,cpp}`: `foreign_toplevel_init()` creates the
  `wlr_foreign_toplevel_manager_v1` global (`core/main.cpp`, alongside
  `session_lock_init`/`layer_shell_init`); a `BiomeForeignToplevel` wrapper
  (private to the .cpp) holds the handle and its five request listeners, and
  is created/destroyed directly from the existing shared
  `toplevel_map`/`toplevel_unmap` (`desktop/toplevel.cpp`) - no new
  lifecycle to design, since map/unmap already bracket exactly the window's
  visible-to-external-tools lifetime. `BiomeToplevel` gained one new field
  (`foreign_toplevel`, `desktop/toplevel.h`) to hold the wrapper pointer.

  Deliberately no new state cached anywhere: title/app_id are read live off
  `xdg_toplevel`/`xwayland_surface` (from the existing `set_title` listeners
  in `xdg_shell.cpp`/`xwayland_shell.cpp`, which already fire on change) and
  maximized/minimized/activated are pushed from `BiomeToplevel`'s own flags
  by a new `foreign_toplevel_sync_state()` called from
  `set_toplevel_maximized`/`set_toplevel_minimized`/`set_toplevel_focused`
  (`desktop/toplevel.cpp`) - confirmed by reading
  `wlr_foreign_toplevel_management_v1.c` that
  `wlr_foreign_toplevel_handle_v1_set_maximized()` and friends already dedup
  against the handle's current state internally, so no redundant-call
  guarding was needed on Biome's side either. `set_fullscreen`/
  `unset_fullscreen` requests are no-ops and the fullscreen state bit is
  never set - Biome has no fullscreen support anywhere to map them onto
  (`xdg_toplevel_request_fullscreen`/`xwayland_toplevel_request_fullscreen`
  already unconditionally deny it).

  One build error caught by the compiler, not planned for: the four
  `wl_container_of`-based listener handlers initially used `auto *wrapper =
  wl_container_of(listener, wrapper, ...)`, which GCC rejected ("use of
  'wrapper' before deduction of 'auto'") - the macro's offsetof-style
  expansion needs the variable's type already resolved, so `auto` can't
  self-referentially deduce it the way it can for an ordinary initializer.
  Fixed by spelling out `BiomeForeignToplevel *wrapper = wl_container_of(...)`
  explicitly, matching the style `session_lock.cpp`'s equivalent line
  already used (not previously understood as load-bearing, just apparently
  a stylistic choice, until this).

  `wlr_foreign_toplevel_management_v1.h` needed no protocol-XML vendoring or
  wayland-scanner codegen at all, unlike layer-shell's - confirmed by
  reading the installed header directly, it doesn't `#include` a generated
  protocol header the way `wlr_layer_shell_v1.h` does, and has no C++-
  reserved-keyword field names needing `cmake/BiomeWlrootsShim.cmake`
  treatment either. Just one new `#include <wlr/types/wlr_foreign_toplevel_management_v1.h>`
  in `core/wlroots.hpp`, alongside the existing wlr includes.

  Full clean rebuild (`rm -rf build`, fresh configure + `cmake --build
  -j$(nproc)`), zero warnings, zero errors.

  **Manually tested, same day: confirmed working.** User added
  `"wlr/taskbar"` to `~/.config/waybar/config.jsonc` and confirmed in the
  nested session: buttons appear with correct titles, click
  activates/raises, and maximize/minimize/close drive the same visible
  behavior as the existing keyboard/decoration paths. Biome's server-side
  Workstream B is now verified end-to-end.

- **2026-08-23 — Forest-side work started.** Planned via a full
  EnterPlanMode cycle (see `forest/`'s own conversation) covering the Qt
  client-extension binding (replacing `KX11Extras`/`Xcbutills::*` in
  `windowlist.cpp`/`windowbutton.cpp`) and the thumbnail-fallback decision.
  Two scope decisions made with the user: ship with no toplevel filtering
  (the protocol has no window-type/skip-taskbar concept, and the `parent`
  event that could approximate transient-dialog suppression isn't sent by
  Biome yet — revisit if this proves annoying in practice rather than
  expanding Biome's scope preemptively); and the thumbnail popup goes
  icon-only for now rather than being dropped outright — see the open-
  questions log entry below for the thumbnail decision specifically. Code
  changes tracked in `forest/`'s own history, not here.

- **2026-08-23 — Windowlist stale-focus bug, multi-round debug session.**
  User reported that opening a new window while another had focus left
  both showing as focused in `windowlist` until an unrelated later focus
  change (alt-tab or click). Getting to the real root cause took three
  iterations, each surfaced by the user retesting and reproducing the bug
  from a different site than the one just fixed: first, a bypass in
  `layer_shell.cpp`'s keyboard-interactive grab and
  `xwayland_shell.cpp`'s unmanaged-surface grab (both granted keyboard
  focus via a direct `wlr_seat_keyboard_notify_enter()` call, skipping
  `focus_toplevel()`'s bookkeeping entirely); then a third, structurally
  identical bypass in `xdg_shell.cpp`'s `xdg_popup_map()` (Forest's own Qt
  context menus/dropdowns/tooltips all map as plain `xdg_popup`s, so this
  was the actual everyday trigger); then a fourth in `core/cursor.cpp`'s
  pointer-button handler (the `toplevel == nullptr` branch hit by any
  ordinary click landing on a layer-shell surface/popup/lock surface) -
  found via a full-codebase grep for every raw
  `wlr_seat_keyboard_(notify_)enter()` call after the user asked directly
  whether this was an architectural problem rather than one more one-off
  site, which it was.

  Rather than patch a fifth site, refactored to make the whole bug class
  structurally impossible: new `grant_keyboard_focus_to_non_toplevel(
  BiomeServer*, wlr_surface*)` (`desktop/toplevel.{h,cpp}`) is now the
  single chokepoint for granting keyboard focus to anything that isn't a
  `BiomeToplevel` - it calls a new `clear_focused_toplevel()` (extracted
  from `focus_toplevel()`'s own unfocus logic) and then grants focus. All
  five external call sites (the two grab sites above, `xdg_popup_map`,
  `xdg_popup_unmap`'s focus-return fallback, and `cursor.cpp`'s click
  handler) were migrated onto it; `focus_toplevel()` itself still uses the
  grab-aware `notify_enter()` directly, deliberately, so an active popup
  grab still wins over a newly-mapped toplevel trying to steal focus. A
  follow-up grep confirmed only `toplevel.cpp`'s own two chokepoint
  implementations and `session_lock.cpp`'s narrower lock-surface grab
  (different invariant, deliberately untouched) remain outside it.

  Confirmed fixed by the user on 2026-08-26. Temporary `wlr_log(WLR_DEBUG,
  ...)` tracing added across the investigation (`set_toplevel_focused`,
  `clear_focused_toplevel`, `focus_toplevel`, `foreign_toplevel_sync_state`)
  was stripped once confirmed, per this file's established convention. Full
  clean rebuilds throughout, zero warnings, zero errors each time.
  Workstream B is done on both sides.

- **2026-08-26 — Workstream C started: step 1 (Biome-side
  `GlobalShortcuts` portal prototype) landed and manually confirmed
  working.** Planned via a full EnterPlanMode cycle. Research first
  (web search against the real xdg-desktop-portal docs, the shortcuts-spec,
  and niri's own stalled attempt at this same thing) resolved the "least-
  understood piece of the phase" flagged in `docs/plan.md`'s Open risks:
  `org.freedesktop.portal.GlobalShortcuts` is a *frontend* interface that
  `xdg-desktop-portal` brokers to a desktop-specific *backend* implementing
  `org.freedesktop.impl.portal.GlobalShortcuts` (what KWin/Mutter actually
  implement) — decided Biome implements the backend, not the frontend, per
  the Decoupling goal. Also found the shortcuts-spec's real trigger grammar
  (`CTRL`/`ALT`/`SHIFT`/`NUM`/`LOGO`, `+`-joined, xkbcommon key names) and
  that `BindShortcuts` can only be called once per session (a later-step
  concern for `reloadhotkeys()`, not this one).

  Per user feedback during planning, unified Biome's fixed compositor
  keybindings and portal-registered shortcuts into one mechanism instead of
  two independent ones: new `core/keybindings.{h,cpp}` holds a shared
  `parse_trigger()` (the shortcuts-spec grammar, via `xkb_keysym_from_name()`)
  and `handle_key_press()`, the single dispatch entry point checking, in
  order, the VT-switch range, the `session_locked` gate, the Alt-Tab
  switcher (kept as a bespoke pre-check - stateful, and xkb's
  Shift+Tab→ISO_Left_Tab keysym substitution doesn't fit the table's exact
  match), a compiled-in built-in table (Escape-quit,
  Ctrl-Alt-arrows/Shift-arrows/1-4 - each trigger defined as a string and
  parsed through `parse_trigger()` itself rather than hand-built from
  `WLR_MODIFIER_*` constants), then a dynamic vector of portal-registered
  bindings. Built-ins always win on an exact-trigger collision with a
  portal shortcut - a deliberate, conservative default; whether a portal
  shortcut should ever be allowed to shadow a built-in is left open for
  later. This replaces `core/input.cpp`'s old Alt-only-gated
  `handle_keybinding()` switch entirely - `keyboard_handle_key()` now calls
  `handle_key_press()` on every press regardless of modifiers, since portal
  shortcuts aren't restricted to Alt-chords. A relevant-modifiers mask
  (Shift/Ctrl/Alt/Logo) is applied before matching so CapsLock/NumLock
  toggling doesn't spuriously break a binding - the spec's "NUM" modifier
  parses successfully but isn't honored at match time yet, deliberately
  (nothing needs a NumLock-conditioned shortcut, and defaulting to honoring
  it would make NumLock silently break every existing binding instead).

  New `ipc/` module (first thing to land there, as this file anticipated):
  `ipc/global_shortcuts_portal.{h,cpp}` implements
  `org.freedesktop.impl.portal.GlobalShortcuts` via `Qt6::DBus`
  (`Qt6::Core` was already linked into the `biome` target for
  `output_config.cpp`'s `QSettings`, so this added `Qt6::DBus` to an
  already-present dependency). `CreateSession`/`BindShortcuts`/
  `ListShortcuts`/`ConfigureShortcuts` reply synchronously
  `(u response, a{sv} results)`, per real `impl.portal.*` backend
  convention (confirmed via `busctl introspect` against the real spec's
  method signatures once running) - unlike the frontend, no per-call
  Request-object dance is needed on the backend side. `BindShortcuts`
  auto-accepts every requested shortcut with no confirmation dialog
  (matches Biome's fixed-policy identity - no shortcut-picker UI exists or
  is planned), parses each `preferred_trigger` via `core/keybindings.h`'s
  shared `parse_trigger()`, and registers a closure that emits `Activated`
  then `Deactivated` (every shortcut is treated as instant/press-only for
  this prototype - no hold semantics yet). A `GlobalShortcutSpec` struct
  with hand-written `QDBusArgument` marshalling operators represents one
  `(sa{sv})` array entry - registered via `qDBusRegisterMetaType`, and
  confirmed working end-to-end including the nested struct-in-variant-in-
  array shape. Session teardown is a `CloseSession()` stand-in
  a test client calls directly, deliberately not the real per-session
  `org.freedesktop.impl.portal.Session` object with its own `Close()` the
  portal daemon would call - flagged as a known gap for the later step that
  wires up the real broker.

  One real integration gap found (not anticipated in the plan) and fixed
  during implementation: `main.cpp`'s `QApplication` is offscreen and its
  event loop is deliberately never run (decoration/ drives Qt synchronously
  off Biome's own loop instead). Without pumping Qt's event loop at all,
  `QDBusConnection::registerObject()`/`registerService()` succeed (they're
  synchronous setup calls) but no incoming method call or outgoing signal
  ever actually dispatches - confirmed via `busctl introspect` timing out
  before the fix, succeeding after. Fixed with a `wl_event_loop` timer
  (`ipc/global_shortcuts_portal.cpp`) that calls
  `QCoreApplication::processEvents()` every 10ms - still Biome's own loop
  driving this (not a second, competing event loop, matching the same
  principle decoration/ already established), just polling rather than
  being woken by the exact fd Qt's dispatcher is waiting on. Flagged as a
  pragmatic prototype fit, worth revisiting for real fd-based integration
  later if latency ever matters.

  Full clean rebuild (`rm -rf build`, fresh configure + `cmake --build
  -j$(nproc)`), zero warnings, zero errors.

  **Manual test pass, same day.** Verified via a throwaway Python D-Bus
  test script (deleted once done) calling Biome's exposed
  `impl.portal.GlobalShortcuts` object directly - not through the real
  `xdg-desktop-portal` broker yet, which is deferred to a later step.
  `CreateSession`/`BindShortcuts`/`ListShortcuts` all round-tripped with the
  exact real wire shapes (cross-checked against `busctl introspect`'s own
  signature output). First keypress test (`LOGO+Return`, nested in the
  user's real X11 session) produced no signal at all - root-caused to host
  interference, not a Biome bug: `xfconf-query` confirmed the host
  `xfwm4`'s own defaults already grab `<Alt>F12`/`<Ctrl>F12`-shaped
  combos, and a nested-in-X11 Biome window never even sees a key the host
  window manager has globally grabbed. Switched to `CTRL+ALT+SHIFT+F12`
  (confirmed unclaimed) and it worked immediately - user's keypress
  produced a real `Activated` followed by `Deactivated` on the test
  script, timestamp confirmed to line up with when the key was pressed.
  `Alt+Escape` (built-in quit) also re-confirmed working, exercising the
  new unified `handle_key_press()` path for a built-in binding, not just a
  portal one. Workstream C's step 1 is done; next up is either the real
  `xdg-desktop-portal`/`portals.conf` broker wiring or starting the
  `forest/`-side `foresthotkeys.cpp`/`hotkey.cpp` rewrite (see this
  workstream's own bullets above for what's deferred to those steps).

- **2026-08-26 (same day) — Follow-up audit: confirmed the new periodic
  Qt event-loop pump (added for the portal work above) didn't break any of
  `decoration/`'s existing "Biome never pumps a Qt event loop" assumptions.**
  A full-tree grep for every Qt deferred/queued mechanism found exactly two
  such assumptions, both in `decoration/` (`switcher.cpp`/`theme.h` using
  plain `delete` over `deleteLater()`, and `frame_widget.cpp`'s
  `force_activate_layouts()` calling layout invalidate/activate
  synchronously instead of trusting a posted `LayoutRequest`). Neither
  actually breaks: both are synchronous workarounds that don't depend on
  the loop never running, only on not being able to assume it runs
  *promptly* - still true with a coarse 10ms poll. Their stale comments
  ("never runs") were reworded to reflect the new periodic-pump reality.
  Confirmed empirically too (10+ second nested run with a real client
  mapped, no crashes/warnings), and separately verified against Qt 6.8.2's
  actual `qlayout.cpp` source that `force_activate_layouts()` can't cause a
  double layout pass once the periodic pump later delivers a now-stale
  posted `LayoutRequest` (`QLayout::activate()`'s own `d->activated` guard
  handles it) - no code change needed for that part. Full clean rebuild,
  zero warnings, zero errors.

- **2026-08-26 — Workstream C step 2: real `xdg-desktop-portal`/`portals.conf`
  broker wiring done and manually confirmed end-to-end.** Planned via a full
  EnterPlanMode cycle. Step 1's test called Biome's `impl.portal.
  GlobalShortcuts` backend object directly, bypassing `xdg-desktop-portal`
  entirely - this step proves the real path a consumer (eventually Forest's
  hotkey client) will actually use: the *frontend* interface
  (`org.freedesktop.portal.GlobalShortcuts` on `org.freedesktop.portal.
  Desktop`), brokered by the real daemon to Biome's backend.

  Researched directly against this machine's installed `xdg-desktop-portal
  1.20.3` (Debian Trixie) rather than from memory: `man 5 portals.conf`, the
  real `gtk.portal`/`lxqt.portal`/`lxqt-portals.conf` files installed by
  `xdg-desktop-portal-gtk`/`-lxqt`, and `misc/Hyprland/assets/` as a
  wlroots-compositor precedent. Added two new git-tracked files, deliberately
  placed to mirror their real install destination 1:1 (confirmed against the
  installed `lxqt.portal`'s/`lxqt-portals.conf`'s real package paths) so a
  future Biome packaging pass can install them as-is - no `install()` rule
  wired yet since nothing else in the tree has one either:
  `biome/data/xdg-desktop-portal/portals/biome.portal` (`DBusName=org.
  freedesktop.impl.portal.desktop.biome`, `Interfaces=org.freedesktop.impl.
  portal.GlobalShortcuts;`, `UseIn=biome` - matches the object path/service
  name `ipc/global_shortcuts_portal.cpp:179-194` already registers, no code
  changes needed there) and `biome/data/xdg-desktop-portal/biome-portals.conf`
  (`[preferred]` `default=biome`). Deliberately shipped no `.service`
  D-Bus-activation file: Biome self-registers at startup because it *is* the
  compositor, always running before any portal call makes sense (same
  pattern GNOME Shell's own portal backend uses) - unlike gtk/lxqt's lazily
  D-Bus-launched helper daemons, or Hyprland's own split (its repo ships only
  `hyprland-portals.conf`; the actual backend is a separate,
  separately-packaged `xdg-desktop-portal-hyprland` helper). An `Exec=` here
  would risk D-Bus trying to launch a second `biome` compositor process as a
  side effect of a stray portal call.

  Dev-loop harness needed a real correction mid-implementation: the first
  attempt pointed the `XDG_DESKTOP_PORTAL_DIR` env var at the repo's
  `.portal` directory for zero-install testing, but the daemon's own
  `--verbose` log showed it replaces xdg-desktop-portal's *entire*
  `portals.conf` search path too (not just the `.portal` directory,
  undocumented in `man 5 portals.conf`) - so the real `biome-portals.conf`
  was silently skipped and the daemon fell back to the deprecated
  `UseIn`-only matching instead (logged as an explicit warning). Fixed by
  symlinking both files into the standard unprivileged XDG search dirs
  instead - `~/.local/share/xdg-desktop-portal/portals/biome.portal` and
  `~/.config/xdg-desktop-portal/biome-portals.conf`, both pointing back at
  the git-tracked repo files as the one source of truth - confirmed via the
  log this time showing real `(config)`-tagged backend selection, not the
  fallback. Also confirmed live that `biome-portals.conf`'s `default=biome`
  gracefully falls back to `gtk.portal` for every interface `biome.portal`
  doesn't declare, so a scoped test instance doesn't actually break the host
  login session's other portal traffic (file choosers etc.) during the test
  window, contrary to the more cautious assumption in the original plan.

  Test procedure: a foreground, `--replace`d `xdg-desktop-portal --verbose`
  instance with only `XDG_CURRENT_DESKTOP=biome` set in that one shell - no
  root, no touching the host's systemd-managed `xdg-desktop-portal.service`.
  Confirmed live via `busctl --user introspect` that killing it hands
  `org.freedesktop.portal.Desktop` cleanly back to the host's normal
  auto-activated instance (`GlobalShortcuts` disappears, `FileChooser`
  reappears). Rewrote the manual test client (throwaway `dbus-python` script,
  deleted once done - not committed, matching step 1's convention) to speak
  the real frontend's async `Request`-object protocol instead of the
  backend's synchronous reply shape - confirmed the exact real signatures
  live via `busctl --user introspect org.freedesktop.portal.Desktop
  /org/freedesktop/portal/desktop org.freedesktop.portal.GlobalShortcuts`
  rather than assuming them: `CreateSession(a{sv})`/`BindShortcuts(oa(sa{sv})
  sa{sv})`/`ListShortcuts(oa{sv})` all return only a `Request` object path
  `o`, replying later via that object's `Response` signal;
  `Activated`/`Deactivated` signals are shaped `osta{sv}`.

  **Manual test pass.** First attempt showed no `Activated` signal at all
  after the user's real keypress - root-caused via direct `/proc` inspection
  (not log inference) to a false alarm unrelated to the broker wiring: the
  previous Biome instance's nested X11 window had been closed, but Biome's
  X11 backend logs that `DestroyNotify` as "Unhandled" and never exits the
  process - it keeps running headless. A second Biome instance launched
  afterward collided with the still-alive first one over both
  `WAYLAND_DISPLAY=wayland-0` and the `org.freedesktop.impl.portal.desktop.
  biome` bus name. Killed both stray processes, relaunched a single clean
  instance, and retested. Confirmed via a raw `busctl --user monitor` capture
  on Biome's own backend object (independent of the frontend test client, so
  it shows Biome's real wire behavior directly) that `CreateSession`/
  `BindShortcuts` round-trip correctly and `Activated`/`Deactivated` fire
  with matching timestamps (`1787793329753` on both), relayed all the way
  through the real broker to the frontend test client with the correct
  `session_handle`/`shortcut_id`.

  That same raw monitor capture also resolved the plan's open question about
  the missing real `org.freedesktop.impl.portal.Session` object
  (`ipc/global_shortcuts_portal.h:24-31`'s documented gap) precisely rather
  than just "seems fine": the real daemon does attempt
  `org.freedesktop.DBus.Properties.GetAll` and `org.freedesktop.impl.portal.
  Session.Close()` against that session object path, and gets
  `UnknownObject` errors back both times, since Biome doesn't implement it.
  `CreateSession`/`BindShortcuts`/the `Activated`-`Deactivated` relay all
  work correctly without it - only teardown (`Close()`) and property
  introspection are actually broken today. Per the plan's own criteria, this
  stays a known, now precisely-characterized gap for a later
  teardown-focused pass rather than an in-scope fix here.

  Small new finding, not scheduled, just flagged for whenever nested-X11
  testing ergonomics come up again: Biome's X11 backend doesn't exit when its
  own nested window is closed (the nested-window-close path is genuinely
  unhandled, not merely unlogged) - a tester closing the window can leave a
  stale headless process squatting on `WAYLAND_DISPLAY`/D-Bus names, as
  happened here.

  Workstream C's step 2 is done. Next up: the `forest/`-side
  `foresthotkeys.cpp`/`hotkey.cpp` rewrite against this now-validated real
  frontend contract - replacing `qxt/`'s `XGrabKey` internals with `QtDBus`
  calls against `org.freedesktop.portal.GlobalShortcuts`, per this
  workstream's own bullets above.

- **2026-08-26 (same day) — Workstream C step 3: forest-side port off
  `XGrabKey`, plus the two Biome-side companion pieces it needed.** Planned
  via a full EnterPlanMode cycle covering both repos at once. Built, not yet
  manually tested by the user - per [[feedback_manual_interactive_testing]]
  in Claude's memory, that pass is deferred to the user.

  **Biome-side companion work (`biome/ipc/`, `biome/core/`):**

  - Real `org.freedesktop.impl.portal.Session` object, closing the exact gap
    step 2's `busctl` monitor capture characterized (`Properties.GetAll`/
    `Close()` both returning `UnknownObject` against the session_handle
    path). `ipc/global_shortcuts_portal.h`/`.cpp` gained a `PortalSession`
    class (`Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.
    Session")`, a `version` property, a `Close()` slot) - one instance
    created and `registerObject()`-ed at the session_handle path inside
    `CreateSession()`, torn down by its own `Close()` slot (unregisters
    itself, calls back into `GlobalShortcutsPortal::closeSession()` - the
    renamed, no-longer-a-D-Bus-method version of the old stand-in
    `CloseSession()` slot, which is deleted - then `deleteLater()`s). First
    attempt put `closeSession()`'s declaration under the class's `signals:`
    section by mistake, which made moc synthesize a signal-emitter body for
    it - collided at link time with the real definition in the .cpp
    (`multiple definition of GlobalShortcutsPortal::closeSession`); fixed by
    moving it to a plain private method section, caught immediately by the
    first incremental build.
  - Modifier-only ("bare tap") trigger support, needed for forest's
    Meta-only "Show menu" binding (`etc/forest/Forest.conf` item-0013),
    which the shortcuts-spec grammar has no syntax for.
    `core/keybindings.h`'s `ParsedTrigger::keysym == XKB_KEY_NoSymbol` is now
    a documented sentinel for "modifier-only"; `parse_trigger()`
    (`core/keybindings.cpp`) returns it for a trigger string that's a single
    recognized modifier name on its own (`"LOGO"`, `"CTRL"`, etc.), ahead of
    the existing multi-part parsing. Matching/firing is new state machine
    logic, not a `trigger_matches()` extension: `BiomeServer` gained
    `modifier_tap_candidate`/`modifier_tap_interrupted` (`core/server.h`,
    next to `switcher_active`), and a new `handle_modifier_tap()` is called
    from `core/input.cpp`'s `keyboard_handle_key()` for every key event -
    press *and* release, unlike the press-only `handle_key_press()` it runs
    alongside - mirroring the old X11-era `XCB_KEY_RELEASE`/`lastkeypressed`
    logic from forest's pre-port `hotkey.cpp`: a press of a lone matchable
    modifier (arbitrated via `is_modifier_keysym()` against
    `Shift_L/R`/`Control_L/R`/`Alt_L/R`/`Super_L/R`) arms a candidate; any
    other key press (or a second modifier) marks it interrupted without
    disarming it; release of that same modifier back to zero matchable bits
    fires the matching portal-registered binding if it was never
    interrupted. Deliberately doesn't try to disambiguate `Super_L` from
    `Super_R` (holding both and releasing one is treated as still-held,
    per the popcount check on `kMatchableModifierMask` bits, not physical
    keycodes) - an accepted edge case, not exercised by any real trigger.

  Full incremental rebuild of `biome` (touched targets: `biome_ipc`,
  `biome`), zero errors, after the one signals/private mistake above was
  caught and fixed. A destructive full clean rebuild (`rm -rf build/*` or
  `cmake --build --target clean`) was blocked by the coding agent's own
  sandbox classifier as a destructive action outside this task's scope, so
  the full-clean-rebuild bar this phase's other workstreams held to
  couldn't be verified this session - left for the user (or a future
  session with that permission granted) to confirm.

  **Forest-side work (`forest/services/services-app/hotkeys/`):**

  - Deleted `qxt/qxtglobalshortcut.cpp`/`.h`/`_p.h`, `qxt/qxtglobal.h`,
    `qxt/qxtglobalshortcut_x11.cpp` outright (confirmed dead: the
    `QxtGlobalShortcut` class was compiled but never instantiated, and
    `hotkey.h`'s include of it was already commented out) - removed their
    entries from `services/services-app/CMakeLists.txt` too. Kept
    `qxt/keymapper_x11.h`'s `KeyTbl` (Qt::Key → numeric X11/xkb keysym
    table) in place, repurposed below rather than relocated (left as a
    cosmetic follow-up, per the plan).
  - `hotkey.h`/`.cpp`'s `globalhotkey` is now a pure data holder: dropped
    `setShortcut`/`unsetShortcut`/`pause`/`resume`/`XcbEventFilter`/
    `nativeKeycode`/`nativeModifiers`/`registerShortcut`/
    `unregisterShortcut` and the `<X11/Xlib.h>`/`Xcbutills` dependency that
    came with them. Gained a stable `id` (the QSettings group name,
    `"item-0001"` etc., threaded through from `foresthotkeys::loadhotkeys()`
    - already unique) and a `description` (now actually read from
    Forest.conf's existing `description` key, previously parsed by nothing),
    plus `triggerString()`: looks the stored `QKeySequence`'s key up in
    `KeyTbl` to get its numeric keysym (covers media/brightness/function
    keys the same way the old `nativeKeycode()` did), falls back to
    `xkb_keysym_from_name()` on `QKeySequence::toString()` for keys not in
    the table, then resolves the canonical shortcuts-spec name via
    `xkb_keysym_get_name()` - confirmed `KeyTbl`'s `XK_*` numeric values and
    libxkbcommon's `XKB_KEY_*` values are the same numbers (shared X11/xkb
    keysym numbering), so no translation layer was needed beyond a cast.
    Special-cases `Qt::Key_Meta` with no modifiers (how
    `loadhotkeys()`'s pre-existing `"Meta"` sentinel already gets built into
    a `QKeySequence`) straight to the trigger string `"LOGO"`, matching the
    Biome-side grammar extension above.
  - New `hotkeys/globalshortcutsportal.{h,cpp}` (`GlobalShortcutsPortal`)
    wraps the real frontend contract validated in step 2:
    `createSession()`/`bindShortcuts()`/`closeSession()`, each driving the
    real async `CreateSession`/`BindShortcuts` `Request`-object/`Response`-
    signal dance (a private `PortalRequest` QObject, defined directly in the
    `.cpp` via the `#include "globalshortcutsportal.moc"` pattern, forwards
    a `Request`'s one-shot `Response` signal to an arbitrary
    `std::function` per call - `QDBusConnection::connect()` only takes a
    real slot, not a lambda, so this is the shim that avoids one fixed slot
    per call site). `bindShortcuts()` redeclares its own `PortalShortcutSpec`
    marshalling struct rather than sharing Biome's `GlobalShortcutSpec` -
    the two repos have no shared header, so it's the same `(sa{sv})` wire
    shape hand-duplicated, same as the trigger-string grammar itself is
    duplicated knowledge between `hotkey.cpp` and
    `biome/core/keybindings.cpp`. `Activated` is connected once, at
    construction, straight to a `handleActivated()` slot that re-emits a
    plain Qt `shortcutActivated(QString id)` signal; `Deactivated` stays
    unconnected (unused on the Biome side too, per step 1/2's own notes).
  - `foresthotkeys.cpp`'s `setup()` now constructs the portal and defers
    `loadhotkeys()` into `createSession()`'s callback - portal setup is
    inherently async, nothing can bind before a session exists.
    `loadhotkeys()` builds the `QList<globalhotkey*>` exactly as before
    (unchanged QSettings parsing, now also reading `description`) and hands
    it to `portal->bindShortcuts()` instead of letting each `globalhotkey`
    grab its own key. A new `dispatch(QString id)` slot, connected to
    `shortcutActivated`, looks up the matching entry by `id` and calls
    `exec()`, gated by the pre-existing `paused` flag.
    `pauseHotkeys()`/`resumeHotkeys()` collapsed to pure flag toggles - no
    portal calls needed now that dispatch is centralized here rather than
    each hotkey grabbing/ungrabbing its own key, a genuine simplification
    over the old per-item loop. `reloadhotkeys()` chains
    `closeSession()` → `createSession()` → `loadhotkeys()` through their
    callbacks, so a reload actually gets a fresh session (needed once the
    portal's `Session::Close()` companion piece above lands - otherwise the
    daemon's "bind once per session" behavior would make a second
    `loadhotkeys()` a no-op). `showdesktop()` is now a `qWarning()` stub
    (no Biome equivalent to `_NET_SHOWING_DESKTOP` exists yet - deferred to
    Workstream D, which it's conceptually adjacent to) in place of the old
    `Xcbutills::showDesktop()` call, dropping that include.
  - `services.h`/`.cpp`: `XcbEventFilter()` is now an empty-body override
    (matching `windowlist`'s post-port precedent) instead of forwarding to
    `fhotkeys`, and `needs_xcb_events()` flipped to `return false` - nothing
    in `services-app` needs XCB events anymore.
  - Build: added `pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET
    xkbcommon)` to the top-level `CMakeLists.txt` (found system xkbcommon
    1.7.0), linked `PkgConfig::XKBCOMMON` into `services-app`, dropped
    `forest_link_xcbutills(services-app)` (confirmed via grep that neither
    `notify`/`notifyadapter`/`notifypopup` nor `polkitagent`/`polkitdialog`
    use `Xcbutills` or raw XCB events either). Added `libxkbcommon-dev` to
    `debian/control`'s Build-Depends, alongside the existing X11/layer-shell
    entries.

  Full incremental rebuild of `forest` (`services-app` target, then the
  whole tree), zero errors, no new warnings. Same sandbox-classifier
  restriction as the Biome side prevented a destructive full clean rebuild
  this session.

  **Manual test pass, same day, bare metal (tty1, not nested).** Biome
  launched directly on a free VT (`./build/core/biome -s foot`); a
  `--replace`d `XDG_CURRENT_DESKTOP=biome /usr/libexec/xdg-desktop-portal
  --verbose` run from inside that session (the `.portal`/`.conf` symlinks
  from step 2 were already in place, so no re-setup needed) took over
  `org.freedesktop.portal.Desktop` from the host's normal instance -
  confirmed via its log picking `biome.portal` for `GlobalShortcuts`
  specifically ("Using biome.portal for org.freedesktop.impl.portal.
  GlobalShortcuts (config)") while still falling back to `gtk.portal` for
  everything else. The local `~/.config/Forest-wayland/Forest.conf`
  `[hotkeys]` section was restored first, per `WAYLAND-TESTING-NOTES.md`'s
  own note to do this once this workstream lands.

  First attempt failed immediately: `BindShortcuts` errored client-side
  with `Marshalling failed: Invalid object path passed in arguments`, even
  though the daemon's own log showed `CreateSession` had succeeded
  (`global shortcuts session owned by ':1.194' created`). Root cause:
  `GlobalShortcutsPortal::createSession()` (`forest/services/services-app/
  hotkeys/globalshortcutsportal.cpp`) tried to read `session_handle` back
  out of `CreateSession`'s `Response` results - but per the portal spec's
  session-handle convention, that path is never returned there at all; the
  client is required to derive it itself from its own sender name and the
  `session_handle_token` it chose (the same convention already used
  correctly for the Request path, just missed for the Session path).
  `m_sessionHandle` was silently left at its default-constructed, empty
  `QDBusObjectPath()`, which is what `BindShortcuts` failed to marshal.
  Fixed by computing `/org/freedesktop/portal/desktop/session/<escaped
  sender>/<session_handle_token>` locally right after choosing the token,
  the same way `libportal` and every other session-based portal client
  do it, instead of trusting `results`. Rebuilt just `services-app`
  incrementally, zero errors; restarting the test `forest` binary picked
  up the fix immediately.

  **Confirmed working after the fix:** a normal combo (`Meta+E` → file
  manager). Still open at the end of this session: a media key, the bare
  `Meta` tap, `pauseHotkeys`/`resumeHotkeys` during a settings-UI key
  capture, and `reloadhotkeys()` - continued in the next entry. Also still
  open: a destructive full clean rebuild of either tree, blocked both times
  by the coding agent's own sandbox classifier - left for the user (or a
  future session with that permission granted).

- **2026-08-31 — Workstream C manual test pass continued, two more bugs
  found and fixed; workstream now fully confirmed.** New session (after a
  reboot/relogin) resumed testing on the same tty1 bare-metal setup. First
  attempt failed with `GlobalShortcutsPortal: CreateSession call failed:
  "No such interface \`org.freedesktop.portal.GlobalShortcuts\` on object
  at path /org/freedesktop/portal/desktop"` - not a code bug, just that the
  `--replace`d `xdg-desktop-portal` instance from the prior session doesn't
  persist (it's a foreground process, not a service) and needed relaunching
  the same way as before; the `.portal`/`biome-portals.conf` symlinks
  themselves were still in place from step 2 and needed no re-setup.

  With that running, forest's new per-hotkey `GlobalShortcutsPortal:
  binding <id> -> <trigger>` log line (added as a diagnostic during this
  investigation, left in) showed every trigger string generated correctly,
  including `"LOGO"` for the bare Meta tap - narrowing the still-open items
  down to two real bugs, both in runtime behavior rather than trigger
  parsing:

  1. **The bare `Meta` tap never fired**, even though every combo including
     `LOGO+`-prefixed ones worked fine. Root-caused by reading
     wlroots' actual source rather than assuming
     (`~/Misc/wlroots/types/wlr_keyboard.c:110-131`, the local checkout
     with real `.c` sources, not the headers-only `misc/wlroots` in this
     repo): `wlr_keyboard_notify_key()` emits `events.key` (what
     `keyboard_handle_key()` listens to) *before* calling
     `xkb_state_update_key()` for that same event - so
     `wlr_keyboard_get_modifiers()` read during a modifier key's own
     press/release event is always one event behind, missing that key's
     own contribution. Invisible for ordinary combos (the modifier's own
     press already landed in `xkb_state` by the time some *later* key's
     event arrives - a separate, earlier call to
     `wlr_keyboard_notify_key()`), but fatal for a bare-tap trigger, which
     only ever looks at the modifier key's own press/release events: at
     Super_L's press, `modifiers` still read `0` (LOGO not yet applied) so
     the arm condition never matched; at release, `modifiers` still read
     `LOGO` (not yet cleared) so the fire condition never matched either -
     the tap silently never armed or fired, either direction, for the
     structurally same reason. Fixed in `core/keybindings.cpp` by adding
     `modifier_bit_for_keysym()` (`is_modifier_keysym()` now just checks it
     against `0`) and having `handle_modifier_tap()` patch that key's own
     bit into `mods` by hand (OR it in on press, AND-NOT it out on release)
     instead of trusting `modifiers` for the key currently being processed.
     Required restarting `biome` itself, not just `forest` - the compositor
     process, so the whole nested session - since the fix is in `biome`'s
     own binary.
  2. **`pauseHotkeys` didn't actually let a new hotkey be captured** -
     `edithotkeywidget::keyPressEvent()`
     (`forest/services/services-settings/hotkeys/edithotkeywidget.cpp`)
     calls `pauseHotkeys()` right before it needs to see a raw keypress
     (e.g. re-binding the Meta key to something else), but Biome kept
     swallowing the key at the compositor level regardless, because
     `foresthotkeys::pauseHotkeys()` (this workstream's own step 3 change)
     had simplified pause/resume down to a pure `paused` flag toggle - per
     the original plan's own reasoning, quoted in the plan doc, that "no
     portal calls needed at all now that dispatch is centralized." That
     reasoning was wrong: the flag only gates whether `dispatch()` *acts*
     on an `Activated` signal client-side, but Biome's compositor-side
     `portal_bindings()` table still matches and swallows the raw key
     before it ever reaches the focused client - unlike the old
     `XGrabKey`-based pause, which released the actual OS-level grab. Fixed
     by making `pauseHotkeys()`/`resumeHotkeys()` (`forest/services/
     services-app/hotkeys/foresthotkeys.cpp`) actually call
     `portal->closeSession()` / `portal->createSession()` +
     `portal->bindShortcuts()` - the same close/recreate/rebind machinery
     `reloadhotkeys()` already used - rather than only toggling the flag.
     Forest-only fix, `services-app` rebuilt incrementally, zero errors;
     picked up by restarting just the test `forest` binary.

  **All items on the manual test checklist now confirmed working**: a
  normal combo (`Meta+E`), media keys (`Volume Mute` and friends, which
  turned out to have been working correctly since the first fix - the
  earlier "not yet tried" note was just untested, not broken),
  the bare `Meta` tap, `pauseHotkeys`/`resumeHotkeys` during a real
  settings-UI key capture, and `reloadhotkeys()` (confirmed live via the
  reload log line appearing in forest's terminal on edit+save, no stale
  bindings). Workstream C is done. Still open: the destructive full clean
  rebuild of either tree, same sandbox-classifier restriction as before.
