# Biome Roadmap

Biome-focused, forward-looking roadmap, grouped by target release. Releases
are small and frequent (a couple of major items plus easy ones), most urgent
first. Phases 0–5
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


## 0.4.0 — touch & tablet input (Phase 7)

Pairs with Forest 0.12.0. Currently the single biggest user-visible gap versus
sway/Hyprland (`core/input.cpp`/`cursor.cpp` have zero touch/tablet handling).

- **Touch** — no `wlr_touch` handling at all; touchscreens/tablet-PC hardware
  unusable.
- **Tablet input** (`tablet-v2`) — no pen/stylus support (`cursor.cpp` notes
  this explicitly). Affects Krita/GIMP and 2-in-1 hardware.
- **`tearing-control-v1`** — lets fullscreen games/mpv request immediate
  presentation for lower latency. Supported by sway, Hyprland, gamescope.
- **`wp_presentation`** (presentation-time) — frame-timing feedback for
  vsync-aware scheduling in video players/games/toolkits.

## 0.5.0 — screen sharing & virtual input (Phase 7)

Pairs with Forest 0.13.0.

- **Screen sharing (portal `ScreenCast`)** — add
  `org.freedesktop.impl.portal.ScreenCast` to biome's portal (currently
  `data/xdg-desktop-portal/portals/biome.portal` only declares
  `GlobalShortcuts`), implemented via the existing screencopy support + PipeWire. Until
  then Zoom-style screen share has no path under biome (no `-wlr` portal
  backend installed either).
- **Virtual input** (`virtual-keyboard-unstable-v1`,
  `virtual-pointer-unstable-v1`) — on-screen keyboards, remote-desktop/
  screen-share input injection (wayvnc-style), accessibility input tools.
- **`ext-data-control-v1`** (from the wlroots 0.20 bump) — unlocks Forest's
  clipboard manager.

## 0.6.0 — input methods & sandboxing (Phase 7/8)

- **IME / text input** (`text-input-v3`, `input-method-unstable-v1`) — no
  CJK input method support, no on-screen-keyboard text injection path.
- **`security-context-v1`** — lets a compositor apply sandbox-aware policy to
  requests brokered through `xdg-desktop-portal` for Flatpak apps. Doesn't
  block Flatpak apps without it, just means no sandbox-aware policy the way
  GNOME/KDE compositors have.

## Later (unscheduled) — rendering & wlroots bump (Phase 8)

- **Adopt wlroots 0.20 features** (Biome now builds on 0.20.2). Remaining
  after `ext-data-control-v1` (0.5.0): `color-management-v1` / HDR (Hyprland
  and Sway 1.12 have it on wlroots 0.20).
- **Per-window urgency in the taskbar.** Biome marks windows urgent
  (decoration, switcher, `ext-workspace-v1` workspace state), but
  `wlr-foreign-toplevel-management` v3 has no urgent state to tell Forest's
  windowlist. Wait for a standard (wlr-protocols MR !129, or
  `ext-foreign-toplevel-state` leaving draft) rather than add a
  Biome-specific channel.
- **Urgent on rejected activation tokens.** Toolkits raising an unfocused
  window without a launch token (Qt: `requestActivate()` fallback) mint one
  from that window's surface. wlroots rejects it at commit, then drops
  `activate` for the unknown token before `request_activate` fires, so the
  window isn't even marked urgent (terminal relaunch of a single-instance
  app). Needs a wlroots change or a Biome-side xdg-activation. Forest works
  around it with a seat-less token.
- **Keep powered-off outputs off across output-management applies.**
  `output_power_wake_all()` lights every blanked output before a modeset,
  since wlroots refuses a modeset on a disabled output. Test the config, apply
  positions now, and defer an off output's mode/scale/transform to its
  power-on. Forest never applies while blanked; this covers other clients.
- **Displays settings: per-output RGB range.** `rgb_range` is hand-edited in
  `Biome.conf` only. No standard protocol carries it (wlr-output-management
  has no such field), so a Forest control needs a Biome-specific mechanism;
  revisit when there's a concrete need.
- **GlobalShortcuts consent / configuration UI.** Today every
  `BindShortcuts` request is auto-accepted unless its trigger collides with
  a built-in or another binding. A real consent dialog needs a Wayland
  surface (layer-shell overlay, or a Forest-provided dialog over an IPC
  path), persisted per-app decisions, and would back `ConfigureShortcuts`
  (currently "not supported"). Open question: whether Biome or Forest owns
  the UI, given the decoupling goal.
