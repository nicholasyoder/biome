# xdg-activation-v1 — Plan

Implement `xdg-activation-v1` so an **already-open** window can be brought
forward on the user's behalf, and give unhonored requests somewhere to go:
urgency. Status: **proposed, not started** — policy below is a draft
pending review.

## What it's for

Wayland gives clients no way to raise/focus themselves; this protocol is
the only one, gated by a token proving recent user input. Biome focuses
every new window at map unconditionally, so ordinary launches don't need
it. It matters when no new window appears:

- **Relaunching a single-instance app** (forest-settings, Firefox, many
  file managers): the new process forwards to the running one and exits;
  the running one must raise its existing window.
- **Link click** opening in an already-running browser.
- **Notification click** raising the app's window (the notification spec
  passes an `activation_token`).
- **Tray icon click** showing a hidden window (see Forest side below).
- **Blocked case:** a background app trying to pop up on its own has no
  fresh token → urgent instead of stealing focus.

The windowlist doesn't use this; it goes through foreign-toplevel, where
the panel is already trusted.

## Background

1. Whatever received the click (menu, panel, chat app, notification
   server) requests a token from the compositor *in its input handler*,
   tagged with seat + input serial and optionally its surface.
2. It hands the token on: `XDG_ACTIVATION_TOKEN` env var when spawning a
   process, or IPC (D-Bus, SNI) to a running one. A freshly spawned
   process can't usefully mint its own — it received no input.
3. The process owning the window calls `activate(token, surface)`; the
   compositor decides. Clients get no feedback.

E.g. menu → Settings while settings is running: the panel mints a token,
spawns `forest-settings` with it in the env; that process calls
`org.forest.Settings.OpenPage(path, token)` and exits; the running
instance calls `activate` with it on its window.

`wlr_xdg_activation_v1` (0.18) already, at token commit:

- rejects a seat+serial token whose serial wasn't sent to that client, or
  whose surface has neither keyboard nor pointer focus (checked *at
  issue time only*);
- accepts a token with **no** seat/serial unchecked;
- expires tokens after `token_timeout_msec` (default 30 s).

Everything after `request_activate` is our policy.

### Prior art

| | Token validation | Valid request | Otherwise |
|---|---|---|---|
| Hyprland | none (ignores serial, 12-month tokens) | urgent; focus only if `focus_on_activate` (off) | — |
| sway | wlroots' checks | `focus_on_window_activation` smart/urgent/focus/none | urgent |
| KWin | moving stricter ("Extreme": valid token only) | activate | demands attention |

## Policy

### When a token is honored

All of:

- wlroots accepted it (above), **and** it carries a seat + serial;
- no button/key press has gone to a *different* client since the token
  was issued (the user hasn't moved on) — layer-shell clients (panel,
  desktop) included;
- the session isn't locked;
- the target surface is a mapped toplevel (unmapped: ignore — map-time
  focus already covers new windows).

Staleness is input-based, not focus-based: the menu closing after a click
moves focus without any new user input, so it must not invalidate the
token. Clicking or typing into another window does.

Seat-less and stale tokens fall through to urgency. Token lifetime stays
at wlroots' 30 s (slow cold starts; the input check covers "user moved
on").

### What an honored activation does

Same as the windowlist's activate: switch to the window's workspace,
unminimize, focus + raise. No fullscreen exception — a valid token means
the user asked for it.

### What an unhonored request does

Set `urgent` on the toplevel. Cleared in `focus_toplevel()`. Shown via:

- Biome's decoration: an `urgent` dynamic property on `#biomeFrame`
  (same pattern as `focused` / `biomeMaximized`), styled entirely in QSS —
  no hardcoded look in C++. `biome-dark.qss` ships a titlebar tint;
- the alt-tab switcher (same property-driven approach on its entries);
- `ext-workspace-v1`'s standard `urgent` state on the window's workspace
  (Forest's `library/toplevels` already consumes ext-workspace).

**Deferred:** per-window urgency in Forest's windowlist.
`wlr-foreign-toplevel-management` v3 (what wlroots 0.18 ships) has no
urgent state; wlr-protocols MR !129 proposes one, and
`ext-foreign-toplevel-state` is still a draft. Wait for a standard rather
than add a Biome-specific channel (decoupling goal).

### Xwayland

X11 has no tokens. `request_activate` (`_NET_ACTIVE_WINDOW`) and
`WM_HINTS` urgency (`set_hints`) both just set `urgent`; neither focuses.

## Implementation

Biome, one PR off `develop`:

1. `desktop/xdg_activation.{h,cpp}`; `BiomeServer` holds
   `wlr_xdg_activation_v1 *`. Created in `main.cpp` alongside the other
   globals.
2. `new_token` listener: record the requesting `wl_client` and a server
   input counter in `token->data` (freed on token `destroy`). The counter
   bumps on each button/key press delivered to a client; store the last
   recipient client alongside so "press to a different client" is a
   comparison, not a history.
3. `request_activate` listener: apply the policy above.
4. Move the switch-workspace + unminimize + focus sequence out of
   `foreign_toplevel.cpp`'s `handle_request_activate` into an
   `activate_toplevel()` in `toplevel.cpp`; both paths call it.
5. `BiomeToplevel::urgent` + setter that updates ext-workspace state and
   calls a `DecorationFrame::setUrgentState()` (property + repolish, like
   `setFocusedState()`); `[urgent="true"]` rules in `biome-dark.qss`;
   switcher entry property.
6. Xwayland: listen to `request_activate` and `set_hints`.
7. Roadmap: remove the item; add a deferred "per-window urgency to the
   taskbar" entry pointing at MR !129. Settled policy →
   `architecture-notes.md`; delete this file.

### Forest side (after this lands)

- settings-plan Phase 3 as written: token helper in `library/`, launchers
  pass `XDG_ACTIVATION_TOKEN`, `OpenPage` forwards it. Launchers must
  request the token in the click handler, *before* hiding the menu — the
  issue-time focus check above needs the menu surface still focused.
  Only matters for already-running apps; ordinary launches work without.
- **systray:** call the SNI item's `ProvideXdgActivationToken(token)`
  before `Activate`/`SecondaryActivate`, otherwise a tray app can only
  mark itself urgent instead of showing its window.
- Optional: show workspace urgency in the workspace applet.

## Known consequence

Relaunching `forest-settings` from a terminal (no token in the env) only
marks the existing window urgent. Intended.
