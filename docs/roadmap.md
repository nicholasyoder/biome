# Biome Roadmap

Biome-focused, forward-looking roadmap. Phases 0–5 (skeleton through
Forest-shell integration and cutover) are done — see
[`docs/history.md`](history.md) for that narrative and the design rationale
behind it (Decoupling goal, wlroots version pin, decoration/session-lock
invariants, etc.). This file starts from "what's next" and stays a punch
list, not a narrative.

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

## Known issues

Carried over from `history.md`'s former "Open risks" section — still open,
not resolved by anything since:

- **No Debian packaging.** `biome` has no `debian/` directory at all yet, so
  `forest/debian/control` still can't `Depends: biome` (it still wrongly
  lists X11-era deps). Needs a dedicated packaging pass (control file,
  install rules, changelog).
- **Output `scale` + manual `x`/`y` can open a layout gap that traps the
  cursor.** `core/output_config.{h,cpp}` stores per-connector offsets with no
  cross-output validation against effective (scaled) size; a stale neighbor
  offset opens a gap that `wlr_cursor_warp_closest` snaps the cursor out of.
  Real fix: auto-arrange by default and/or validate+correct at
  output-manager init, not defer entirely to Phase 6's display-settings UI.
- **An Xwayland override-redirect popup can render (and steal focus) behind
  a fullscreen window.** `BiomeUnmanaged` surfaces are parented directly to
  `layers.toplevels`, not nested under their owning toplevel, so they don't
  get carried into `layers.fullscreen` on reparent. Not yet confirmed against
  a real app; fix is likely raising unmanaged surfaces above
  `layers.fullscreen` while any toplevel is fullscreen.

## Phase 6 — Session, idle & display completeness

Already-scoped work (was Phase 6 in the old plan), plus idle-inhibit found
during the 2026-09-07 protocol audit.

- **Screenshots** — `wlr-screencopy-unstable-v1` or `ext-image-copy-capture-v1`.
- **`ext-idle-notify-v1`** — drives idle timeout; replaces `core/idle_blank.cpp`'s
  hardcoded stopgap (delete that module once this lands, per its own
  `STOPGAP(idle-blank)` comments). Needed before a lock-screen client is
  useful.
- **`idle-inhibit-unstable-v1`** — net new, not in the old plan. Lets a video
  player / presentation app / game suppress idle-notify while running.
  Without it, idle-notify + a lock client will interrupt video playback —
  bundle this with the idle-notify work above, not as an afterthought.
- **`wlr-output-management-unstable-v1`** — for a live display-settings
  client. Reuse `libkscreen`'s existing backend for this protocol (has a
  working KScreen-on-Sway implementation) rather than hand-binding it.
- **`wlr-output-power-management-unstable-v1`** (DPMS) — replaces the power
  half of `idle_blank.cpp`'s stopgap; distinct protocol from
  output-management above.
- **`wlr-gamma-control-unstable-v1`** — night-light/redshift-style color
  temperature. Not previously tracked; cheap to add once output code is
  already being touched for the items above.

Forest-side: a lock-screen client speaking `ext-session-lock-v1` (Biome's
compositor side already works, confirmed with swaylock in Phase 3.5) and a
display-settings plugin are `forest/`-side work, tracked there — not detailed
here.

## Phase 7 — Input completeness

Found via the 2026-09-07 protocol audit (`core/input.cpp`/`cursor.cpp` have
zero touch/tablet/gesture handling). Currently the single biggest
user-visible gap versus sway/Hyprland.

- **Touch** — no `wlr_touch` handling at all; touchscreens/tablet-PC hardware
  unusable.
- **Tablet input** (`tablet-v2`) — no pen/stylus support (`cursor.cpp` notes
  this explicitly). Affects Krita/GIMP and 2-in-1 hardware.
- **Trackpad gestures** (`pointer-gestures-unstable-v1`) — pinch/swipe/hold;
  sway and Hyprland both support it. Without it, GTK/Qt apps that key off
  trackpad swipes just see raw pointer motion.
- **Pointer lock/confinement** (`pointer-constraints-unstable-v1`) +
  **`relative-pointer-unstable-v1`** — required for FPS-style mouse look in
  any game or 3D app. Ship together; they're used together.
- **Virtual input** (`virtual-keyboard-unstable-v1`,
  `virtual-pointer-unstable-v1`) — on-screen keyboards, remote-desktop/
  screen-share input injection (wayvnc-style), accessibility input tools.
- **IME / text input** (`text-input-v3`, `input-method-unstable-v1`) — no
  CJK input method support, no on-screen-keyboard text injection path.

## Phase 8 — Rendering & sandboxing protocol gaps

- **`linux-dmabuf-v1`** — never created anywhere in the tree
  (`wlr_linux_dmabuf_v1_create_with_renderer` doesn't appear; confirmed
  tinywl doesn't wire this implicitly either, so it's a real gap, not an
  oversight-safe default). Without it, GPU clients can't negotiate zero-copy
  buffers — affects hardware video decode, some GL/Vulkan paths, and
  zero-copy screencopy (interacts with Phase 6's screenshot work). Verify
  it's actually needed given XWayland's own path, then wire it either way so
  it's a deliberate choice, not an accident.
- **`tearing-control-v1`** — lets fullscreen games/mpv request immediate
  presentation for lower latency. Supported by sway, Hyprland, gamescope.
- **`wp_presentation`** (presentation-time) — frame-timing feedback for
  vsync-aware scheduling in video players/games/toolkits.
- **`color-management-v1` / HDR** — Hyprland and Sway 1.12 (on wlroots 0.20)
  just landed this. Not urgent at Biome's current wlroots 0.18 target;
  revisit alongside any future wlroots version bump.
- **`security-context-v1`** — lets a compositor apply sandbox-aware policy to
  requests brokered through `xdg-desktop-portal` for Flatpak apps. Doesn't
  block Flatpak apps without it, just means no sandbox-aware policy the way
  GNOME/KDE compositors have.
- **Minor opportunistic protocols** — `xdg-activation-v1` (focus-steal
  prevention; also relevant to the `windowlist` plugin's "flash instead of
  steal focus" UX), `single-pixel-buffer-v1`, `content-type-v1`,
  `alpha-modifier-v1`. Toolkits probe for these and fall back gracefully if
  absent — pick up opportunistically rather than as a dedicated push.
