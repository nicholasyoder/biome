# GlobalShortcuts hold/repeat semantics — Plan

Let a held global shortcut keep acting while its key is down, instead of once
per press. Status: **planned, not started**. Spans both repos: Biome reports
real press/release, Forest does the repeating.

## Problem

Holding a bound volume key steps the level once instead of ramping it.
Forest's volume up/down D-Bus handlers are a one-shot ±2% step per call.

## Current behavior

- `ipc/global_shortcuts_portal.cpp` fires `Activated` and then `Deactivated`
  back-to-back on press (roadmap 0.2.0, "GlobalShortcuts backend gaps").
- `core/input.cpp`'s `keyboard_handle_key()` only runs `handle_key_press()` on
  `PRESSED`; nothing tracks which shortcut a held key triggered.
- libinput drops kernel autorepeat, so a held key gives one `PRESSED` and one
  `RELEASED`. No raw repeat stream exists to forward.
- Forest's `GlobalShortcutsPortal` (`services/services-app/hotkeys/`) only
  listens to `Activated`; `Deactivated` is ignored.

## Design

The spec's `Activated`/`Deactivated` mean "became active" and "became
inactive". The compositor reports press/release truthfully and the client
repeats between them, the same split as `wl_keyboard` `repeat_info`. Biome
re-emitting `Activated` while held would be Biome-only behavior, and Forest's
ramp would silently break on any other compositor (decoupling goal).

## Biome

1. On a matched portal-shortcut press, record keycode → (session, shortcut
   id). Emit `Activated` as today; drop the immediate `Deactivated`
   (`global_shortcuts_portal.cpp:125-126`).
2. On release of that keycode in `keyboard_handle_key()`, emit `Deactivated`
   and swallow the release. Key on the triggering key, not the modifiers:
   releasing Ctrl first doesn't end the hold.
3. Emit `Deactivated` for any still-held shortcut when its session closes or
   the keyboard is destroyed.
4. No repeat timer in Biome.

## Forest

1. `GlobalShortcutsPortal`: connect `Deactivated`, emit
   `shortcutDeactivated(id)`.
2. Per-hotkey config in `Forest.conf`: `repeat` (bool, default false) and
   `repeat_interval` (ms, default 100). One interval: the first repeat fires
   one interval after the press, then every interval. No separate initial
   delay; an occasional double step on a slow tap is acceptable.
3. `foresthotkeys`/`globalhotkey`: on `Activated`, `exec()` once. If `repeat`
   is set, start a `QTimer` at `repeat_interval` that re-runs `exec()`. Stop
   it on `Deactivated`, `sessionLost`, pause, and reload.
4. Settings (`services-settings/hotkeys/edithotkeywidget`): a "Repeat while
   held" checkbox, plus an interval spinbox (ms) enabled only when the box is
   checked. Save/load both through `HotkeyData` and
   `HotkeySettingItem::save()`.
5. Defaults: turn `repeat` on for the shipped Volume Up/Down and Brightness
   Up/Down items in `etc/forest/Forest.conf`. Add a `SettingsUpgradeManager`
   step that does the same for existing configs whose items match those
   shipped actions.

## Order

Biome first, as its own PR: it's spec-correct alone, and current Forest
ignores `Deactivated`, so nothing regresses. Then the Forest PR.

## Testing

- Hold Volume Up: ramps at the configured interval and stops on release.
- Tap a non-repeat shortcut (e.g. launch-app): fires exactly once.
- Set a repeat hotkey's interval to 500 ms: the ramp visibly slows.
- Hold Ctrl+X (repeat on), release Ctrl first: keeps repeating until X is
  released.
- Kill Forest's services while holding a key, or pause hotkeys: no runaway
  timer, and Biome doesn't leak the held state.
- A second `BindShortcuts` on the same session (existing double-registration
  bug, not fixed here): note whether it now double-fires `Deactivated`.
