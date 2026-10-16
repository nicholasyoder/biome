# GlobalShortcuts hold/repeat semantics — Plan

Let a bound global shortcut keep firing while its key is held, instead of
once per press. Status: **proposed, not started**.

## Problem

Volume keys bound via `org.freedesktop.impl.portal.GlobalShortcuts` only
nudge the level once per press; holding the key down no longer ramps it up
or down. Forest's `AudioEngine` volume up/down D-Bus handlers are a one-shot
±2% step per call — by design, they rely on being re-invoked repeatedly
while the key is held, the same way a normal (non-global) key-repeat would
drive a text field.

## Current behavior

- `ipc/global_shortcuts_portal.cpp`'s `BindShortcuts` callback treats every
  bound shortcut as an instant action: on a matched press it fires
  `Activated` immediately followed synchronously by `Deactivated`. This is
  original to the Wayland port (commit 6474945) and is already called out in
  `docs/roadmap.md` (0.2.0, "GlobalShortcuts backend gaps") as "no hold
  semantics" — this plan is that gap's fix.
- `core/input.cpp`'s `keyboard_handle_key()` runs `handle_key_press()` once
  per `wlr_keyboard_key` event where `event->state ==
  WL_KEYBOARD_KEY_STATE_PRESSED` (`core/input.cpp:71-74`). There's no timer
  or repeat synthesis in this path.
- `wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600)` (`core/input.cpp:136`)
  only sets the `repeat_info` Biome advertises to focused Wayland clients via
  `wl_keyboard`, so normal apps can self-repeat typed input on their own
  timer. It has no effect on the global-shortcut dispatch path, which
  doesn't go through seat/focus at all.

**Open question, check first:** whether a held key already produces a
stream of raw `PRESSED` events reaching `keyboard_handle_key()` (i.e.
hardware/kernel-level autorepeat passed through unchanged by libinput), or
whether holding a key truly produces exactly one `PRESSED` + one `RELEASED`
and Biome would have to synthesize the repeat itself. Confirm by logging
`event->state` timestamps in `keyboard_handle_key()` while holding a key.
This decides which of the two shapes below applies.

## Shape

No new wlroots/protocol capability needed — the shortcuts-spec portal
already supports separate `Activated`/`Deactivated` signals with real
press/release timing; Biome just needs to stop firing them back-to-back.

1. Track "shortcut armed" state per bound shortcut in
   `global_shortcuts_portal.cpp`: `Activated` fires on press (as today),
   `Deactivated` fires on the matching release instead of immediately after.
2. While armed, keep the bound action re-firing at the same cadence Biome
   already uses for client-side repeat (25 ms period after a 600 ms initial
   delay, mirroring `wlr_keyboard_set_repeat_info`'s values) — exact
   mechanism depends on the open question above:
   - If raw repeat events already arrive at `keyboard_handle_key()`, forward
     each one to the armed shortcut's `Activated` instead of adding a timer.
   - If not, add a `wl_event_source_timer` in the portal backend (or
     `core/keybindings.cpp`) started on arm, re-firing `Activated` on the
     delay/period above, cancelled on release.
3. `AudioEngine`'s step-per-call handlers need no change — they already do
   the right thing if `Activated` repeats while held.

## Scope

Confined to `ipc/global_shortcuts_portal.cpp` (arm/disarm + re-fire) and
possibly `core/input.cpp`/`core/keybindings.cpp` if a timer is needed there
instead. No Forest-side change expected; `AudioEngine` already behaves
correctly given a repeating `Activated`.

## Testing

- Hold a bound volume key: level should ramp continuously, not step once.
- Tap a bound shortcut (e.g. launch-app): still fires exactly once, no
  double-fire from the arm/disarm change.
- Release the key before the initial repeat delay elapses: exactly one
  `Activated`+`Deactivated` pair, no repeat fired.
- A second `BindShortcuts` on the same session (existing known gap, not
  fixed here): shouldn't double-fire repeats on top of the existing
  double-registration bug.
