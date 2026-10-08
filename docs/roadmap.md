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

`roadmap.md` should only change by items being added, re-scoped, or removed
once done (no "done" markers; history lives in git) — not by accumulating
session logs or design discussion.

## Guiding principles

- **Fixed-policy, not user-configurable.** Biome is an opinionated
  single-policy compositor (like a fixed floating WM), not sway/river-style
  configurable. Don't read gaps below as "add a config option" — they're
  about protocol/feature coverage, not policy flexibility.
- **Decoupling goal.** Default to standard Wayland protocols/interfaces for
  anything crossing the Biome/Forest boundary; a Biome-specific or
  Forest-specific shortcut needs justification, not just convenience. Full
  reasoning in `docs/history.md`'s "Decoupling goal" section (2026-08-22).

## 0.2.0 — input, clipboard & spec compliance (Phases 6–7)

- **`xdg-activation-v1`** (`wlr_xdg_activation_v1` helper). Forest's
  settings app needs it to raise its running window when relaunched
  (Forest `docs/settings-plan.md`, Phase 3); also lets apps reuse existing
  windows (Firefox, GTK). Decide the policy: when a token is honored
  (seat/serial validation, token age) and what an unhonored request does
  (urgent hint, which windowlist can flash). Map-time focus for windows
  without a token stays unconditional for now.
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
- **An Xwayland override-redirect popup can render (and steal focus) behind
  a fullscreen window.** `BiomeUnmanaged` surfaces are parented directly to
  `layers.toplevels`, not nested under their owning toplevel, so they don't
  get carried into `layers.fullscreen` on reparent. Not yet confirmed against
  a real app; fix is likely raising unmanaged surfaces above
  `layers.fullscreen` while any toplevel is fullscreen.
- **Possible: untyped Xwayland popups deactivate their window.** Typed X11
  menus never get focus, so this is fine for Qt/GTK; watch for apps whose
  override-redirect popups lack a window type.
- **Opacity fades lose most of their frames on logout.** `forest-logout`
  barely fades in: its map follows three `forest-logout-dim` scanout-fade
  setups (fresh output-sized buffers, 4K on DP-1) and the log shows a ~73 ms
  main-loop stall, a third of the 220 ms wall-clock fade. Find the stall,
  and whether per-pixel fades can be made cheaper in general.

### Bugs

- **A window that maximizes on map flashes unmaximized first** (seen with
  pcmanfm-qt). xdg `requested.maximized` is only honored in `toplevel_map()`
  (`desktop/toplevel.cpp`), after the client has already drawn its first
  buffer at its own size. Send maximized state + size in the initial-commit
  configure (`xdg_toplevel_commit`) instead, which needs the placement
  output picked before map.
- **Xwayland splash screens get a Biome frame** (e.g. MuseScore's:
  `_NET_WM_WINDOW_TYPE_SPLASH`, `_KDE_NET_WM_WINDOW_TYPE_OVERRIDE`, no
  `_MOTIF_WM_HINTS`). `toplevel_decorated()` only checks Motif hints. Leave
  `SPLASH` windows undecorated (and probably unfocused/centered, like
  sway). wlroots 0.18 has no `has_window_type()` helper; compare
  `xsurface->window_type` against `server->ewmh`'s atoms.
- **Decorations are blurry on scaled outputs** (most visible on the button
  icons). Frames, and the Alt-Tab switcher, are rendered at 1× logical size
  (`decoration/renderer.cpp`, `decoration/switcher.cpp`) and upscaled by the scene;
  render at the output's scale via `QImage::setDevicePixelRatio()`, and
  re-render on output scale change or when a window moves between outputs.
- **Switcher panel keeps its old width once after a window closes;** it only
  shrinks on the second Alt-Tab after the close. Suspect a stale cached size
  hint on the panel's layout item, refreshed by a posted `LayoutRequest`
  between uses rather than by `relayout_and_shrink_to_fit()`
  (`decoration/frame_widget.cpp`). Probably fixed by calling
  `updateGeometry()` on the panel after `setEntries()` changes the icon
  count.

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
  `GlobalShortcuts`), implemented via the existing screencopy support + PipeWire. Until
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
- **Minor opportunistic protocols** — `single-pixel-buffer-v1`, `content-type-v1`,
  `alpha-modifier-v1`. Toolkits probe for these and fall back gracefully if
  absent — pick up opportunistically rather than as a dedicated push.
