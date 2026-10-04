# Biome Roadmap

Biome-focused, forward-looking roadmap, grouped by target release. Phases 0–5
(skeleton through Forest-shell integration and cutover) are done — see
[`docs/history.md`](history.md) for that narrative and the design rationale
behind it (Decoupling goal, wlroots version pin, decoration/session-lock
invariants, etc.). This file starts from "what's next" and stays a punch
list, not a narrative.

Releases usually ship alongside a Forest release (`forest/docs/roadmap.md`,
same layout), but a Biome-only bugfix release is fine whenever warranted.
Release assignments are a plan: move items between releases freely. Old
phase names (Phase 6/7/8) are kept in headings since code comments
reference them.

## Documentation conventions

Keep this file at roadmap altitude: what's planned, current status, one line
of why per item. When an item is actually worked on:

- Settled implementation detail / design rationale that later code needs to
  not regress → add a section to [`docs/architecture-notes.md`](architecture-notes.md)
  (see that file's own intro for what belongs there).
- A substantial workstream's research/postmortem (the kind of thing that
  used to bloat `plan.md` phase-by-phase) → its own `docs/<topic>.md`, linked
  from here, rather than inlined.

`roadmap.md` itself should only ever grow by items being added, checked off,
or re-scoped — not by accumulating session logs or design discussion.

## Guiding principles

- **Fixed-policy, not user-configurable.** Biome is an opinionated
  single-policy compositor (like a fixed floating WM), not sway/river-style
  configurable. Don't read gaps below as "add a config option" — they're
  about protocol/feature coverage, not policy flexibility.
- **Decoupling goal.** Default to standard Wayland protocols/interfaces for
  anything crossing the Biome/Forest boundary; a Biome-specific or
  Forest-specific shortcut needs justification, not just convenience. Full
  reasoning in `docs/history.md`'s "Decoupling goal" section (2026-08-22).

## 0.1.0 — first release

Ships with Forest 0.9.0. Everything here is required before tagging.

### Idle & display power (Phase 6, core)

`ext-idle-notify-v1`, `idle-inhibit-unstable-v1` (`desktop/idle.cpp`) and
`wlr-output-power-management-unstable-v1` (`core/output_power.cpp`) are done.
Forest 0.9.0's session locker (`forest-locker`) is the client that drives
these.

### Screenshots (Phase 6)

- **`wlr-screencopy-unstable-v1`** — available in wlroots 0.18; enough for
  `grim` or a native Forest screenshot client (Forest 0.9.0). Per-window
  capture needs `ext-image-copy-capture-v1` (wlroots 0.19, see Later).

### Output management fixes

- **Output layouts with a gap can still trap the cursor in two paths.**
  Live `wlr-randr` applies are validated and rejected
  (`layout_is_connected()`), but (1) unplugging a *middle* monitor can't be
  rejected and leaves a gap between the remaining outputs, and (2) the startup
  `Biome.conf` layout is never validated (stale `x`/`y` after a `scale`
  change). Fix idea: auto-close gaps on unplug and validate/auto-arrange at
  startup.
- **Scanout-fade layer surfaces** (logout dim / startup cover) aren't moved
  off a disabled output.
- **`foreign_toplevel` output_enter/leave** is only sent at window creation,
  not updated when windows move between outputs.

### Forest integration

- **Default fade-namespace config shipped by Forest.** Forest's
  `forest-logout-dim`/`forest-startup`/`forest-logout` namespaces need
  `[LayerShell]` fade settings or a fresh install gets no fades. Decide how
  Forest's package supplies them (system-wide `/etc` file Biome reads,
  drop-in directory, or compiled-in default); check `core/fade_config.cpp`'s
  lookup order first. Tracked on both roadmaps.

### Verify

- **`linux-dmabuf-v1`** — never created anywhere in the tree
  (`wlr_linux_dmabuf_v1_create_with_renderer` doesn't appear; confirmed
  tinywl doesn't wire this implicitly either). Without it, GPU clients can't
  negotiate zero-copy buffers — affects hardware video decode, some GL/Vulkan
  paths, and zero-copy screencopy. Verify the real-world impact, then wire it
  either way so it's a deliberate choice, not an accident.

### Release checklist

- `debian/changelog` entry for 0.1.0 summarizing the release (currently just
  "Initial Debian packaging"); `CMakeLists.txt` is already `0.1.0`.
- Remove or gate the `ALT+Escape` built-in (`core/keybindings.cpp`): it
  terminates the compositor, and with it the whole session, on one chord.
- Default to `WLR_INFO` in release builds; `main.cpp` hardcodes `WLR_DEBUG`,
  which Forest's `startforest-wayland` captures to `biome.log` every session.
  Keep a way to turn debug back on (flag or env var) for bug reports.
- Fresh-install VM test together with Forest 0.9.0's packages (packaging has
  only been build-checked with `dpkg-buildpackage` + `lintian`).
- Tag `v0.1.0`, push.
- Switch to the `develop`/`master` branch model: create `develop`, and update
  the branching note in `ForestProject/CLAUDE.md`.

## 0.2.0 — input, clipboard & spec compliance (Phases 6–7)

- **Pointer lock/confinement** (`pointer-constraints-unstable-v1`) +
  **`relative-pointer-unstable-v1`** — required for FPS-style mouse look in
  any game or 3D app. Ship together; they're used together.
- **Trackpad gestures** (`pointer-gestures-unstable-v1`) — pinch/swipe/hold;
  sway and Hyprland both support it. Without it, GTK/Qt apps that key off
  trackpad swipes just see raw pointer motion.
- **Data control** (`wlr-data-control-unstable-v1`, available in wlroots
  0.18; `ext-data-control-v1` needs a newer wlroots) — lets a clipboard
  manager read/set the selection without focus. Needed for Forest 0.11.0's
  clipboard manager; also used by `wl-clipboard`/`cliphist`.
- **`wlr-gamma-control-unstable-v1`** — night-light/redshift-style color
  temperature.
- **Stale fractional-scale info after a live rescale.** `wlr_scene`'s
  `handle_scene_buffer_outputs_update()` (wlroots `types/scene/surface.c`)
  only re-sends `wp-fractional-scale-v1` + `wl_surface.preferred_buffer_scale`
  when a surface's *set* of overlapping outputs changes, not when an
  already-overlapped output's own `scale` changes (e.g. a live
  `wlr-randr --scale` apply, found 2026-09-26 debugging a FreeCAD
  cursor-stutter report). Likely needs biome to force a per-surface update
  (mirroring wlroots' own `force` param) for every surface on an output
  whenever its scale changes.
- **An Xwayland override-redirect popup can render (and steal focus) behind
  a fullscreen window.** `BiomeUnmanaged` surfaces are parented directly to
  `layers.toplevels`, not nested under their owning toplevel, so they don't
  get carried into `layers.fullscreen` on reparent. Not yet confirmed against
  a real app; fix is likely raising unmanaged surfaces above
  `layers.fullscreen` while any toplevel is fullscreen.
- **An Xwayland menu deactivates its window.** xdg_popups keep their root
  toplevel activated, but focus granted to an override-redirect surface still
  clears it; resolve the owner via `xsurface->parent`.

### Spec-compliant focus

Each lands only *after* its Forest-side counterpart (Forest roadmap 0.10.0,
"Compositor portability"), or Forest input breaks.

- **Layer-surface keyboard focus ignores `keyboard_interactivity`.** Clicking
  any non-toplevel surface grants focus (`core/cursor.cpp`), even a layer
  surface that asked for `NONE`; and map grants focus to anything not `NONE`
  (`desktop/layer_shell.cpp`), so an `on_demand` surface (Forest's panel)
  steals focus every time it maps, e.g. on each panel rebuild after a screen
  change. Spec: `NONE` never gets focus, `on_demand` only on click, only
  `exclusive` on map.
- **Non-grabbing layer-shell popups get keyboard focus.**
  `popup_wants_keyboard_focus()` (`desktop/xdg_shell.cpp`, also used by
  `core/cursor.cpp`) focuses any popup chain rooted on a layer surface, a
  Forest-shaped exception for `panel-library/popup.h`'s non-grabbing popups.
  Per spec only grabbing popups get focus; drop the layer-shell branch.
- **`exclusive` keyboard interactivity isn't enforced.** Clicking elsewhere
  steals focus from an exclusive top/overlay surface, and a post-map change
  to `keyboard_interactivity` is ignored. Needed once Forest's logout dialog
  moves to `exclusive`.

### Other protocol deviations

- **Layer surfaces survive their output being disabled.** Biome reparents
  them to another output and rewrites `layer_surface->output` rather than
  sending `closed`, though the `wl_output` global is gone
  (`wlr_output_layout_remove`). Sway closes them. Deliberate (see
  architecture notes, "Layer surfaces have a home"); decide whether to keep.
- **Null-output layer surfaces go to the first output**, not the
  focused/cursor output as in sway/Hyprland.
- **xdg-shell advertised at v3** (wlroots 0.18 supports v6): no
  `configure_bounds`, `wm_capabilities` or `suspended`. Also ignored:
  `show_window_menu`, `set_fullscreen`'s output argument, and min/max size
  hints during interactive resize.
- **GlobalShortcuts backend gaps** (`ipc/global_shortcuts_portal.cpp`):
  `Activated`+`Deactivated` both fire on press (no hold semantics); a second
  `BindShortcuts` on a session double-registers; `trigger_description` is
  the raw trigger string; no consent UI.
- **Foreign-toplevel `parent` never set**, so dialogs list as independent
  windows.

## 0.3.0 — touch, tablet & accessibility input (Phase 7)

Currently the single biggest user-visible gap versus sway/Hyprland
(`core/input.cpp`/`cursor.cpp` have zero touch/tablet/gesture handling).

- **Touch** — no `wlr_touch` handling at all; touchscreens/tablet-PC hardware
  unusable.
- **Tablet input** (`tablet-v2`) — no pen/stylus support (`cursor.cpp` notes
  this explicitly). Affects Krita/GIMP and 2-in-1 hardware.
- **Virtual input** (`virtual-keyboard-unstable-v1`,
  `virtual-pointer-unstable-v1`) — on-screen keyboards, remote-desktop/
  screen-share input injection (wayvnc-style), accessibility input tools.
- **IME / text input** (`text-input-v3`, `input-method-unstable-v1`) — no
  CJK input method support, no on-screen-keyboard text injection path.
- **Screen sharing (portal `ScreenCast`)** — add
  `org.freedesktop.impl.portal.ScreenCast` to biome's portal (currently
  `data/xdg-desktop-portal/portals/biome.portal` only declares
  `GlobalShortcuts`), implemented via 0.1.0's screencopy + PipeWire. Until
  then Zoom-style screen share has no path under biome (no `-wlr` portal
  backend installed either).

## Later (unscheduled) — rendering & wlroots bump (Phase 8)

- **wlroots bump past 0.18.** Unlocks `ext-image-copy-capture-v1` +
  `ext-foreign-toplevel-image-capture-source-v1` (0.19; per-window capture
  for Forest's task view / windowlist previews), `ext-data-control-v1`, and
  `color-management-v1` / HDR (Hyprland and Sway 1.12 have it on wlroots
  0.20).
- **`tearing-control-v1`** — lets fullscreen games/mpv request immediate
  presentation for lower latency. Supported by sway, Hyprland, gamescope.
- **`wp_presentation`** (presentation-time) — frame-timing feedback for
  vsync-aware scheduling in video players/games/toolkits.
- **`security-context-v1`** — lets a compositor apply sandbox-aware policy to
  requests brokered through `xdg-desktop-portal` for Flatpak apps. Doesn't
  block Flatpak apps without it, just means no sandbox-aware policy the way
  GNOME/KDE compositors have.
- **Minor opportunistic protocols** — `xdg-activation-v1` (focus-steal
  prevention; also relevant to the `windowlist` plugin's "flash instead of
  steal focus" UX), `single-pixel-buffer-v1`, `content-type-v1`,
  `alpha-modifier-v1`. Toolkits probe for these and fall back gracefully if
  absent — pick up opportunistically rather than as a dedicated push.
