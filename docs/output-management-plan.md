# Live output management — findings and plan

Temporary handoff doc (2026-09-18). Delete once implemented; move settled
design into `architecture-notes.md`.

## Goal

Change output mode/scale/position/transform/enabled at runtime, so two
monitor-layout presets can be switched with plain shell scripts calling
`wlr-randr` (the old xrandr workflow). Compositor side only —
Forest-side display-settings UI stays later.

## Current state

- `core/output_config.cpp` reads `~/.config/Forest/Biome.conf` once
  (`load_output_configs()`, called from `output_manager_init`).
- `server_new_output` (`core/output.cpp`) applies config once and sets up
  everything else positionally, one time: scene output, `layer_*` trees,
  `lock_tree`/`lock_rect`, `arrange_layers`.
- `wlr-randr` here reports "compositor doesn't support
  wlr-output-management-unstable-v1" — that protocol is the missing piece.
- wlroots 0.18.2 ships `wlr_output_manager_v1`
  (`/usr/include/wlroots-0.18/wlr/types/wlr_output_management_v1.h`), so no
  hand-binding needed.
- `output_request_state` already resizes `lock_rect`/lock surface and calls
  `arrange_layers` on a live resolution change (nested backends only today) —
  reuse that logic.

## Status

- **Part 1 (work item 1, geometry refactor): done, tested by hand.**
  `output_sync_geometry()` + `output_set_enabled()` in `core/output.cpp`,
  `disabled` replaces `config_disabled`, `session_lock_maybe_send_locked()`
  shared with `output_frame`. Still unverified until the protocol exists:
  runtime disable of a lit output and `wl_output` global removal.
- **Part 2 (protocol handler): done, tested by hand (scale/mode/pos/off/on).**
  `core/output_management.{h,cpp}`; republish is idle-coalesced and hooked
  into layout change, new/destroyed output, `request_state`. Apply wakes an
  idle-blanked session first (modeset fails on a blanked DRM connector).
  Also fixed `output_set_enabled` re-adding an existing scene output to the
  scene layout (asserts). Cursor is warped back into the layout after apply.
  Two DRM gotchas found in testing: a disabled head has no mode, so clients
  send none (wlroots makes it a 0x0 custom mode; we substitute the last/
  preferred mode); and batched `wlr_backend_test/commit` allocates no buffer,
  so enabling/re-moding needs a cleared one attached ("No primary frame
  buffer").
- Parts 3-4: not started (toplevel relocation, layout validation, scale check).

## Work items

1. **Geometry refactor (bulk of the work) — DONE.** Factor a single "output geometry
   changed" path out of `server_new_output` that:
   - repositions `layer_background/bottom/top/overlay` and `lock_tree` to the
     output's layout coords (listen to `output_layout` `change`);
   - resizes `lock_rect` / re-configures `lock_surface` (security-relevant —
     see comment in `output_request_state`);
   - re-runs `arrange_layers`;
   - handles runtime enable/disable: a disabled output is currently kept out
     of `output_layout` entirely and `BiomeOutput::config_disabled` is set
     once at creation. Both must become runtime state
     (`wlr_output_layout_remove` / add + `wlr_scene_output_layout_add_output`).
   - `core/idle_blank.cpp` skips `config_disabled` outputs — keep that
     working with runtime changes.
   - Existing constraint: committing `enabled=false` as a connector's first-ever
     state crashes the DRM backend (hence the bring-up-then-blank two-step in
     `server_new_output`). Runtime disable of an already-lit output should be
     fine, but test it.
2. **Protocol handler.** `wlr_output_manager_v1_create`; on `apply`/`test`
   events build a `wlr_output_state` per head (enabled, mode or custom mode,
   scale, transform, position, adaptive sync), commit atomically
   (`wlr_backend_test` / `wlr_backend_commit` with a states array), update
   the layout, send succeeded/failed. Republish
   `wlr_output_manager_v1_set_configuration` after hotplug and any layout
   change. Estimated 150–250 lines. Reuse `find_matching_mode`.
3. **Windows and cursor.** Disabling/moving/resizing an output must relocate
   windows and keep the cursor from stranding. Not yet checked: whether
   output unplug (`output_destroy`) already moves toplevels — if so, reuse it
   for disable. Fix the known-issue cursor trap (layout gap +
   `wlr_cursor_warp_closest`) in the same pass: validate/auto-arrange after
   every apply.
4. **Scale changes (unverified).** `wlr_scene` should propagate
   fractional-scale updates to surfaces. Not checked: whether Qt-rendered
   decorations and the alt-tab switcher (`desktop/decoration_bridge.cpp`,
   `decoration/`) re-render at the new scale. Test by hand.

## Decisions

- Don't persist applied changes to `Biome.conf`; it stays the startup
  default and the preset scripts are the source of truth.
- Not in scope: `wlr-output-power-management` (separate protocol, Phase 6),
  Forest display-settings plugin, `kanshi`-style auto-profiles (could be
  layered on later with no Biome changes).

## Suggested order

1. ~~Geometry refactor with no protocol yet; verify startup behavior unchanged
   (nested backend and real DRM).~~ Done.
2. Add the manager handler; test with `wlr-randr` scale/mode/position first.
3. Enable/disable + window relocation + cursor validation.
4. Two real presets as scripts, e.g.
   `wlr-randr --output DP-1 --on --mode 2560x1440@144 --pos 0,0 --output eDP-1 --off`.

Interactive/visual checks (cursor, windows moving, decoration quality) should
be tested manually by the user rather than by building injection tooling.
