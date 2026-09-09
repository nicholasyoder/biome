# Biome event loop research: killing the Qt D-Bus polling timer

Status: **Option 1 implemented (2026-09-08).** The "scoped-down, D-Bus-fd-only" variant
floated under Option 1 below was explicitly rejected — the goal is a fully-functioning Qt
event loop for anything Qt normally expects one to be present for (posted events,
`deleteLater()`, `QLayout`'s deferred `LayoutRequest`, future Qt consumers), not just D-Bus
dispatch, so the general GLib-main-context bridge is the right scope. See "Implementation
plan" at the bottom for the concrete design, validated feasibility findings, and step-by-step
changes — all landed as described (`core/qt_glib_bridge.{h,cpp}`, wired into `core/main.cpp`,
old polling pump deleted from `ipc/global_shortcuts_portal.cpp`). Idle CPU confirmed at zero
(0 utime+stime ticks over 6s idle, vs. the old timer's guaranteed 10-40ms tick forever).

**Post-implementation finding, same day: the `wl_display_terminate()` observability concern
that drove the whole Option 1 vs. Option 2 decision turned out to be moot in practice, for a
different reason than either option anticipated.** Testing after landing Option 1 found that
closing a nested backend's host window does *not* terminate Biome. Comparing against a
from-scratch rebuild of pre-Option-1 `git HEAD` (the original always-on 10ms polling pump, no
bridge at all) reproduced the exact same failure — so this is **not a regression from Option
1**; `wl_display_run()` staying primary preserved existing behavior exactly as designed.

The actual gap: this doc's "actual blocker" research (below) enumerated `wl_display_terminate()`
call sites in wlroots and found they're all genuine connection-loss/error conditions
(`backend/wayland/backend.c:62,80` = "host connection hangup/error"). That's correct as far as
it goes, but closing a nested window via its titlebar/decoration close button doesn't hang up
or error the connection — it sends an ordinary `xdg_toplevel.close` *request* to the client,
which the client (wlroots' Wayland backend, in this case) has to explicitly choose to act on.
Nothing in the call-site survey checked whether wlroots' Wayland backend actually wires that
request to `wl_display_terminate()` — likely it doesn't, which would mean this path was never
going to work via a titlebar close, Option 1 or not; only an actual connection hangup (closing
the whole host terminal/socket, a compositor crash, etc.) hits the researched call sites at
all. Not re-verified against wlroots source as part of this note — flagged here as a distinct,
pre-existing gap for a future session to confirm and fix (e.g. wiring an `xdg_toplevel.close`
listener to `wl_display_terminate()` in wlroots itself, or in whatever calls into it), not
something Option 1 introduced or is responsible for closing.

## Context / how we got here

Investigating "biome uses some CPU even when idle." Root cause found and already fixed:
`ipc/global_shortcuts_portal.cpp` ran a `wl_event_loop_add_timer()` polling loop that called
`QCoreApplication::sendPostedEvents()` + `QAbstractEventDispatcher::processEvents()` every
10ms, forever, unconditionally — needed because `core/main.cpp` constructs a `QApplication`
but deliberately never calls `.exec()` (decoration rendering drives Qt synchronously off
Biome's own loop instead; see `docs/architecture-notes.md:154-163`). Without pumping,
`QDBusConnection` setup succeeds but no D-Bus call/signal ever actually dispatches.

**Interim fix already applied** (independent of everything below, safe either way):
- `ipc/global_shortcuts_portal.cpp`'s `pump_qt_events()` now checks whether
  `processEvents()` did real work; after ~200ms of no-op polls it backs off from 10ms to
  40ms, snapping back to 10ms the moment real work shows up. ~4x fewer idle wakeups.
- Added periodic (~10s) `WLR_DEBUG` trace lines in both `pump_qt_events()` and
  `core/output.cpp`'s `output_frame()` (fires/actual-work counts) so a captured run can
  confirm wakeup rates empirically.
- Confirmed via code reading that the render/frame path is genuinely damage-driven
  (`wlr_scene_output_commit()` rolls back and skips rendering when there's no damage;
  nothing reschedules a frame unconditionally) and `idle_blank.cpp`'s timers are one-shot,
  re-armed only on activity. Neither is a hot-path concern.

**The bigger question**, prompted by the user recalling that `decoration/frame_widget.cpp`
has manual workarounds (`repolish_tree()`, `force_activate_layouts()`) for the fact that no
Qt event loop runs: should Biome flip its architecture so a real Qt loop (`QApplication::exec()`)
is the primary loop — KWin-style — instead of `wl_display_run()`? This would let Qt/QtDBus
dispatch natively (fd-driven, zero polling) instead of needing any pump at all.

That investigation surfaced a real, unresolved correctness gap (see below), so the user
asked to pause and get this written up rather than pick a direction under time pressure.

## Current architecture (as of this research)

- **One `QApplication`**, constructed at `core/main.cpp:69` with `-platform offscreen`.
  `.exec()` is never called anywhere in the codebase (confirmed by grep).
- **`wl_display_run(server.display)`** at `core/main.cpp:157` is the sole, only call to it
  anywhere in the tree — this is Biome's actual main loop today.
- **Three `wl_event_loop_add_timer()` call sites total**, no `add_fd`/`add_signal`/`add_idle`
  anywhere in Biome's own code:
  - `core/idle_blank.cpp:91` — 10-minute idle-blank timeout, re-armed on activity.
  - `core/idle_blank.cpp:71` — 250ms commit-retry timer, only armed after a failed
    `wlr_output_commit_state()`.
  - `ipc/global_shortcuts_portal.cpp:268` — the Qt pump (see above). This is the one that
    would be deleted entirely under either fix direction below.
- **Three `QDBusConnection` consumers**, all depending on the pump to dispatch:
  `ipc/global_shortcuts_portal.cpp` (owns the pump), `ipc/cursor_bridge.cpp`
  (`org.biome.Cursor`), `ipc/workspace_bridge.cpp` (`org.biome.Workspaces`). `desktop/`
  deliberately doesn't link Qt at all — `workspace_bridge` reaches it via a plain C function
  pointer (`server->window_workspaces_changed`).
- **CMake**: `Qt6::Core`/`Qt6::DBus` already linked in `core/` and `ipc/`;
  `Qt6::Gui`/`Qt6::Widgets` in `decoration/`. `QSocketNotifier` (QtCore) needs no new Qt
  module dependency under any option below.
- **`wl_display_terminate()`**: exactly **one** call site in Biome —
  `core/keybindings.cpp:140`, the built-in `ALT+Escape` keybinding.
- **No SIGINT/SIGTERM/SIGCHLD handling exists anywhere in Biome today.** Ctrl-C already
  hits default disposition and skips `main.cpp`'s teardown sequence entirely. This is a
  pre-existing gap, useful context for calibrating how much a new regression would actually
  matter (see Option 2 below).
- **`core/main.cpp` teardown** (after `wl_display_run()` returns, lines 159-179):
  `wl_display_destroy_clients` → ewmh wipe → `wlr_xwayland_destroy` →
  `wlr_scene_node_destroy` → cursor/allocator/renderer/backend destroy → remove
  `server.new_session_lock` listener → `wl_display_destroy`. One documented ordering
  constraint: the session-lock listener removal must happen before `wl_display_destroy()`,
  because wlroots' internal session-lock-v1 destroy handler asserts nothing is still
  listening on its `new_lock` signal.
- **`decoration/frame_widget.cpp`'s workarounds, precisely**:
  - `repolish_tree()` exists because Biome never `QWidget::show()`s these widgets (offscreen
    rendering only) — Qt only auto-polishes on first show. **This is independent of whether
    a loop runs** — it would still be needed even with a live Qt loop, since the widgets
    still wouldn't be shown.
  - `force_activate_layouts()` exists because `QLayout` normally reflows via a posted
    `QEvent::LayoutRequest`, delivered only when the event loop runs. **This looks
    loop-related but likely isn't fully solvable by running a loop either** — decoration
    rendering happens synchronously inside a wlroots callback (cursor motion/hover/press),
    and needs correct geometry immediately to call `QWidget::render()` into a `QImage` in
    that same call, not "whenever Qt's loop next ticks." A live loop doesn't change that
    synchronous contract, so this workaround likely stays regardless of which option below
    is chosen. Don't assume flipping the loop lets this be deleted without checking each
    call site's actual timing requirement first.
  - The one genuine, low-risk win from a truly live Qt dispatcher: `decoration/switcher.cpp`
    currently does a manual synchronous `delete icon` instead of `deleteLater()` (comment at
    `switcher.cpp:86-93`) because `deleteLater()`'s `DeferredDelete` event needs a running
    loop. This could safely revert to `deleteLater()` under either option that gets Qt's
    dispatcher genuinely live (Option 1 or Option 2 below) — small, separable cleanup, not
    load-bearing for the main decision.
  - **Net effect: the "decoration maintainability" argument for flipping the loop is weaker
    than it first appeared.** The real, validated payoff of this whole effort is killing the
    D-Bus polling timer — both options below achieve that equally.

## XWayland / wlroots-internals: confirmed NOT a blocker either way

- XWayland startup/shutdown (`desktop/xwayland_shell.cpp`) is 100% `wl_signal`-based
  (wlroots' own listener mechanism, unrelated to `wl_event_loop`) plus ordinary fd/timer
  sources wlroots manages internally: the Xwayland-ready pipe fd
  (`wl_event_loop_add_fd`), the X11 window-manager connection fd
  (`wl_event_loop_add_fd`), and a 10-second per-surface ping-timeout timer
  (`wl_event_loop_add_timer`). None of this is sensitive to *which* code calls
  `wl_event_loop_dispatch()`, only that it keeps being called regularly — true under either
  option below.
- wlroots' own backend/session bring-up (`wlr_backend_autocreate()` →
  `session_create_and_wait()` in `backend/backend.c`, `wlr_session_find_gpus()` in
  `backend/session/session.c`) calls `wl_event_loop_dispatch()` directly and synchronously,
  entirely *before* any application-level loop starts. Unaffected by this decision.
- `wl_event_loop_get_fd()` / `wl_event_loop_dispatch()` are libwayland's documented,
  sanctioned way to embed the loop in a foreign one (struct-level doc comment: "create an
  event loop context, add sources to it, and call `wl_event_loop_dispatch()` in a loop").
  `wl_display_run()` itself is confirmed (via grep across wlroots' own source) to be a pure
  convenience wrapper used only by `tinywl.c`/`examples/*.c` — wlroots' library code never
  calls it internally. So embedding wl_event_loop into something else is a legitimate,
  sanctioned pattern, not a hack.

## The actual blocker: `wl_display_terminate()` observability

`wl_display_terminate()` just sets a private flag inside the opaque `struct wl_display`.
**Only `wl_display_run()`'s own internal `while` loop ever reads that flag** — there is no
public getter, no destroy-listener, no signal for it anywhere in
`/usr/include/wayland-server-core.h`.

wlroots calls `wl_display_terminate()` directly, from inside the library, on fatal backend
conditions — confirmed by reading each call site:

| Call site | Condition |
|---|---|
| `backend/drm/drm.c:1528` | `drmHandleEvent()` failed |
| `backend/libinput/backend.c:53` | libinput dispatch failed |
| `backend/x11/backend.c:137,150` | nested X11 backend: host connection hangup/error |
| `backend/wayland/backend.c:62,80` | nested Wayland backend: host connection lost |
| `backend/session/session.c:43` | `libseat_dispatch()` failed |

None of these fire any other public/observable signal — `wl_display_terminate()` is the
*only* notification. The nested-backend cases matter most for day-to-day dev use: per
`docs/history.md`, the nested Wayland/X11 backend is the normal way an agent does quick
disposable test runs, and **closing that window is exactly the kind of event that hits this
path.**

If `wl_display_run()` stops being the thing actually running (i.e. Qt owns the loop
instead), these calls become silently unobservable: Biome would sit running indefinitely
with a dead backend instead of exiting, requiring a manual kill.

### KWin comparison (researched via KDE's public GitHub mirror)

KWin's `src/wayland/display.cpp` embeds `wl_event_loop` into Qt using **exactly** the
mechanics that would be needed here: `QSocketNotifier` on `wl_event_loop_get_fd()`, calling
`wl_event_loop_dispatch(loop, 0)` on activation, flushing clients on
`QAbstractEventDispatcher::aboutToBlock`. But that file has **no `wl_display_terminate()`
and no terminate/quit signal wired to it at all.**

Why: KWin's shutdown is Qt-native throughout. When KWin's own DRM backend, libinput
integration, or nested X11/Wayland backend detects a fatal condition, **it calls
`qApp->quit()` directly**, because that code is first-party — KWin wrote its own DRM
backend, its own libinput handling, its own nested backends. It never needed libwayland's
terminate flag as a shutdown signal because the code that detects the fatal condition is
KWin's own and already has direct access to Qt.

**This is the structural difference from Biome.** Biome sits on top of wlroots specifically
so it doesn't have to write DRM/libinput/nested-backend code itself. That code lives inside
wlroots (third-party), and wlroots' only public way of saying "something fatal happened" is
the flag that only `wl_display_run()` can see. KWin's pattern isn't a solution Biome can
adopt as-is — it works for KWin *because* KWin doesn't have this dependency in the first
place.

No other public wlroots signal fires in parallel at any of the five call sites above — this
was checked directly, not assumed. Symbol interposition (defining Biome's own
`wl_display_terminate()` to shadow libwayland's linked symbol) is technically possible but
fragile/non-portable/build-order-dependent, and against the project's general preference
for standard, non-hacky solutions — **not recommended** unless nothing else works out.

## Prior art survey: does any existing wlroots compositor already do this?

Follow-up research, same day, prompted by wanting to check for an existing wlroots
compositor that runs a foreign loop (Qt, GLib, or otherwise) as primary with `wl_display`
embedded as the guest, in case one of them found an elegant way around the
`wl_display_terminate()` observability gap above. Checked by fetching each project's actual
source (GitHub/GitLab), not from memory.

**Every third-party wlroots compositor checked uses `wl_display_run()` (or a thin
same-thread wrapper) as its primary loop — none run a foreign loop as primary:**

| Project | Main loop call | Source checked |
|---|---|---|
| Sway | `wl_display_run(server->wl_display)` | `sway/server.c:805` |
| Wayfire | `wl_display_run()` (via `wl_display_get_event_loop`) | `src/main.cpp` |
| labwc | `wl_display_run(server.wl_display)` | `src/main.c:294` |
| dwl | `wl_display_run(dpy)` | `dwl.c:2010` |
| Cage | `wl_display_run(server.wl_display)` | `cage.c:695` |
| river | `server.wl_server.run()` (zig-wayland wrapper, same call underneath) | `river/main.zig:211` |

Where any of these add their own timers/fds (e.g. Sway's IPC socket, dwl's own bits), they do
it the same direction Biome does *today*: `wl_event_loop_add_fd()`/`add_timer()` into
wlroots' loop, with `wl_display_run()` still the outer call. None invert it.

**Hyprland was the one lead worth chasing directly** — it's the only wlroots-era compositor
known for a heavily custom event/loop architecture. Checked `v0.39.1`'s
`src/managers/eventLoop/EventLoopManager.cpp` (last release line before Hyprland dropped
wlroots for its own Aquamarine backend in mid-2024, per
[the HN discussion of that change](https://news.ycombinator.com/item?id=41055507)): even
there, `enterLoop()` still calls `wl_display_run(display)` as the actual blocking call, after
registering its own timer fd into wlroots' loop via `wl_event_loop_add_fd()`. Same shape as
every project above — Hyprland was never a counterexample, even before it stopped being a
wlroots consumer at all (and once it did, it's no longer relevant here — it owns its own
backend now, same structural category as KWin/Mutter below, not a wlroots-layered project
anymore).

**The only two real "foreign loop is primary" examples found are KWin (Qt, already
documented above) and GNOME/Mutter (GLib) — and both are exempt from this problem for the
same structural reason, not because they solved it:** both wrote their own DRM/libinput/nested-backend code
first-party, so the code detecting a fatal backend condition already has direct access to
`qApp->quit()` / `g_main_loop_quit()` and never needed `wl_display_terminate()` as a
cross-library notification path in the first place. This reinforces rather than changes the
existing KWin finding.

**Re-checked the actual gap against current wlroots (`master`, i.e. post-0.19/0.20) and
current libwayland (`main`), not just the 0.18 this project targets** — confirms it isn't a
version-specific quirk:
- All five `wl_display_terminate()` call sites (`backend/drm/drm.c`,
  `backend/libinput/backend.c`, `backend/x11/backend.c` ×2, `backend/wayland/backend.c` ×2,
  `backend/session/session.c`) are unchanged.
- `wlr_session`'s only signals are `active`/`add_drm_card`/`destroy`
  (`include/wlr/backend/session.h`) — `destroy` fires from `wlr_session_destroy()` during
  normal teardown, not from the fatal `libseat_dispatch()`-failed path, which calls
  `wl_display_terminate()` directly and emits nothing else.
- `wlr_backend`'s only signals are `destroy`/`new_input`/`new_output`
  (`include/wlr/backend.h`) — same story, `destroy` is teardown-only.
- Plain libwayland's `wl_display_add_destroy_listener()` fires on `wl_display_destroy()`,
  a different, later event than `wl_display_terminate()` setting the flag — doesn't help
  either.

**One more useful data point, non-wlroots but structurally relevant: Smithay (Rust) compositors
(e.g. `niri`) run `calloop` as primary with Wayland's dispatch as one source among many —
the same overall shape Option 1 above proposes.** Checked `niri`'s `src/backend/tty.rs`:
session pause/resume surfaces as an ordinary `smithay::backend::session::Event` (e.g.
`SessionEvent::PauseSession`) delivered straight into a calloop callback — i.e., a Rust
`Result`/enum the compositor already owns, not a hidden C-level flag a third-party library
sets behind your back. This is a real precedent that "foreign-loop-primary" can be done
cleanly — but it works because Smithay's backend crates expose failures as ordinary return
values to calloop-owned code, a different API shape than wlroots' C library, not because
anyone solved *this* problem in *this* API. Not directly portable to a wlroots-based
project.

**Conclusion: no existing wlroots-based project already does what Option 1/Option 2 below are
choosing between — there's no prior art with an already-elegant fix to copy.** The
`wl_display_terminate()` gap is specific to being layered on top of wlroots' C API, holds at
every version checked including tip-of-tree, and both real "foreign loop primary" precedents
in the wild (KWin, Mutter) sidestep it only by not depending on a third-party backend library
the way Biome depends on wlroots. This doesn't change the options below, just closes off the
"maybe someone already found a clean trick" question — Option 1 vs. Option 2 remain the real
choice.

## Options for a future session

### Option 1 — Keep `wl_display_run()` primary; bridge Qt's dispatcher in as the guest
Zero regression risk: `wl_display_terminate()` keeps working exactly as today, for every
case above, because `wl_display_run()` is untouched. Instead, `ipc/global_shortcuts_portal.cpp`'s
timer is replaced with real fd-driven integration: Qt's default dispatcher on Linux
(`QEventDispatcherGlib`) wraps a `GMainContext`; bridge it by calling
`g_main_context_prepare()`/`query()` to get its current fd set + next timeout, registering
each fd with `wl_event_loop_add_fd()`, and calling `g_main_context_check()` +
`g_main_context_dispatch()` when one of those fires — then re-querying, since the fd set can
change after every dispatch (new D-Bus connection, a QTimer starting/stopping, etc.). This
is the harder-to-hand-roll direction (the fd-set resync is the fiddly part — miss it and a
D-Bus reply can silently never get dispatched), but it's bounded, ordinary engineering
complexity, not an open correctness gap. Needs `g_main_context_acquire()` to legitimately
own the context.

*Possible scoped-down variant worth investigating first*: rather than a fully general
GLib-main-context bridge, check whether `QDBusConnection`/`QAbstractEventDispatcher` expose
anything narrower — e.g. direct access to the D-Bus socket fd — that would let this be a
much smaller, D-Bus-specific `wl_event_loop_add_fd()` integration instead of a general
GLib-context bridge covering all of Qt's internal timers/sources too. Not yet researched;
would reduce the plumbing significantly if it exists cleanly through public Qt API.

### Option 2 — Flip to `QApplication::exec()`, accept the known gap
Mechanically simpler: `wl_event_loop_get_fd()` + `QSocketNotifier` + `wl_event_loop_dispatch()`,
same pattern KWin uses for dispatch (not termination). Trade-off: nested-backend
host-window-close, DRM GPU/event-handling failure, and libinput fatal errors stop cleanly
exiting Biome — it would sit inert until manually killed, rather than exiting promptly.
Severity context: comparable *in kind* to the pre-existing "Ctrl-C skips cleanup" gap (no
signal handling exists today either), but broader in the set of conditions affected, and it
specifically hits the nested-backend workflow used for routine dev/agent test runs.

If this direction is chosen anyway, worth pairing it with adding real SIGINT/SIGTERM
handling (a natural, low-cost addition once Qt owns the loop — a self-pipe/`signalfd` +
`QSocketNotifier` is a well-known pattern) as partial mitigation: it wouldn't restore
detection of the wlroots-internal fatal cases, but it would at least make a manual `kill`
result in clean teardown instead of the current abrupt exit.

### Option 3 — Option 2 + symbol interposition to reclaim `wl_display_terminate()`
Not recommended (see above) — recorded for completeness only.

## Suggested next steps for a future session

1. If leaning toward Option 1: research whether Qt exposes a narrower D-Bus-only fd hook
   before committing to the full GLib-context bridge (see "scoped-down variant" above).
2. If leaning toward Option 2: decide whether the nested-backend-loss/DRM-loss regression is
   actually acceptable given Biome's current single-user/prototype status, and whether
   pairing it with new SIGINT/SIGTERM handling is worth doing at the same time.
3. Either way: the interim adaptive-backoff fix already in `ipc/global_shortcuts_portal.cpp`
   is safe to keep indefinitely regardless of which direction is eventually chosen — it's
   not blocking anything.
4. Whichever direction is picked, plan for a real test pass afterward (nested run,
   exercising outputs / XWayland launch / session-lock / layer-shell / hotkeys /
   workspace-switch, plus — for Option 2 specifically — deliberately closing the nested
   window and unplugging/failing a backend if feasible, to confirm the accepted regression
   behaves as expected rather than worse than expected).

## Implementation plan (Option 1, chosen 2026-09-08)

The scoped-down "D-Bus-fd-only" variant mentioned under Option 1 above is **not** being
pursued: the point of this change is a real, general Qt event loop — `deleteLater()`,
`QLayout`'s deferred `LayoutRequest`, `QTimer`, future Qt consumers, all of it — not just
making `QDBusConnection` dispatch. So this is the full GLib-main-context bridge.

### Feasibility validated before writing this plan

Two things this plan depends on were checked empirically rather than assumed, since getting
either wrong would make the whole approach silently do nothing:

- **Qt6 on the target system (Debian Trixie, `libqt6core6t64` 6.8.2+dfsg-9+deb13u2) was built
  with GLib support.** `qtcore-config.h` has `#define QT_FEATURE_glib 1`. Without this, Qt
  falls back to `QEventDispatcherUNIX` (its own poll-based dispatcher, no GLib involvement),
  and nothing below would apply.
- **The actual runtime dispatcher for a main-thread `QApplication`/`QCoreApplication` is
  `QEventDispatcherGlib`, and it wraps `g_main_context_default()` specifically** (not a
  private per-instance context) — confirmed two ways:
  - Qt source (`qeventdispatcher_glib.cpp`): the constructor does
    `if (app && QThread::currentThread() == app->thread()) mainContext = g_main_context_default();`
    — main-thread case only; other threads get a private `g_main_context_new()`. Biome's
    `QApplication` is constructed on `main()`'s thread and Biome has no threading anywhere
    (`grep -rn QThread\|std::thread` across the tree: zero hits), so the main-thread branch
    always applies.
  - Verified live with a throwaway test program (`QCoreApplication` + `-platform offscreen`,
    same as Biome's setup): `QAbstractEventDispatcher::instance()->metaObject()->className()`
    prints `QEventDispatcherGlib`; a `QDBusConnection::sessionBus()` connects fine even before
    any loop runs (confirms today's "setup is synchronous, dispatch is not" finding still
    holds); and `g_main_context_acquire(g_main_context_default())` succeeds, followed by
    `g_main_context_prepare()`/`query()` returning real fds — all via plain public GLib API,
    no private Qt headers touched anywhere.

**Consequence: the bridge module needs zero Qt C++ API at all.** It only ever touches
`g_main_context_default()` through public `<glib.h>` calls. It doesn't need to know
`QDBusConnection` or `QAbstractEventDispatcher` exist — it's dispatching *the* process-wide
GLib context that Qt's dispatcher happens to also be attached to, so anything else in the
process that uses GLib's default context (now or later) rides along for free too.

### New module: `core/qt_glib_bridge.{h,cpp}`

Lives in `core/` (not `ipc/`, where the old pump lived) because it's no longer D-Bus-specific
— `decoration/` and any future Qt consumer benefit equally, and `core/main.cpp` is where it
needs to be wired in earliest. Single entry point:

```cpp
// core/qt_glib_bridge.h
struct BiomeServer;
void qt_glib_bridge_init(BiomeServer *server);
```

Called from `core/main.cpp` right after `server.display = wl_display_create();` (line 74) —
before `output_manager_init()` and everything else, and well before
`global_shortcuts_portal_init()`/`cursor_bridge_init()`/`workspace_bridge_init()`, so Qt's
dispatch is live for the entire rest of startup, not just from whenever IPC happens to init.
No explicit teardown call needed at shutdown — nothing here owns anything that outlives
process exit in a way that matters (see "acquire, never release" below).

### Core algorithm

State kept in the module (anonymous namespace, mirroring the style being replaced):

```cpp
GMainContext *g_context = g_main_context_default();
gint g_priority = 0;                                   // from the most recent prepare()
std::vector<GPollFD> g_poll_fds;                        // from the most recent query()
int g_n_fds = 0;
std::unordered_map<int, wl_event_source *> g_fd_sources; // fd -> registered wl_event_loop source
wl_event_source *g_timeout_source = nullptr;             // arms glib's requested wait
wl_event_loop *g_loop = nullptr;
```

One function drives everything, called (a) once at init and (b) every time a registered fd
fires or the timeout timer fires:

```
rearm():
    loop:
        immediate = g_main_context_prepare(g_context, &g_priority)

        # query() reports how many fds it actually needs; grow and re-ask if our
        # buffer was too small. This resync-on-every-cycle is "the fiddly part" —
        # skipping it is exactly how a D-Bus reply can silently never dispatch
        # (a new QDBusConnection, or a QTimer starting/stopping, changes the fd
        # set on the next prepare()).
        loop:
            n = g_main_context_query(g_context, g_priority, &timeout_ms,
                                      g_poll_fds.data(), g_poll_fds.size())
            if n <= g_poll_fds.size(): break
            g_poll_fds.resize(n)
        g_n_fds = n

        sync_fd_sources()      # diff g_fd_sources against the new g_poll_fds set
        arm_timeout(timeout_ms) # wl_event_source_timer_update; 0/disarm if timeout_ms == -1

        if not immediate:
            return   # nothing ready now; wait for wl_event_loop to call pump() back

        pump()       # something's ready right now — dispatch it, then loop back
                      # to re-prepare, since dispatch can change what's pending next

pump():
    g_poll(g_poll_fds.data(), g_n_fds, 0)   # non-blocking; fills real revents.
                                              # g_poll() is GLib's own wrapper (portable,
                                              # no manual struct-pollfd-layout assumptions)
    ready = g_main_context_check(g_context, g_priority, g_poll_fds.data(), g_n_fds)
    if ready:
        g_main_context_dispatch(g_context)
    # state is now stale (dispatch may have started/stopped timers, opened a new
    # D-Bus connection, etc.) — caller (rearm()'s loop) re-prepares next iteration
```

```
sync_fd_sources():
    new_fds = { fds[i].fd: mask_from_events(fds[i].events) for i in range(g_n_fds) }
    for fd, mask in new_fds:
        if fd not in g_fd_sources:
            g_fd_sources[fd] = wl_event_loop_add_fd(g_loop, fd, mask, on_fd_ready, nullptr)
        elif mask changed since last cycle:
            wl_event_source_fd_update(g_fd_sources[fd], mask)
    for fd in g_fd_sources.keys() - new_fds.keys():
        wl_event_source_remove(g_fd_sources[fd])
        erase fd

mask_from_events(events):
    (WL_EVENT_READABLE if events & G_IO_IN else 0) |
    (WL_EVENT_WRITABLE if events & G_IO_OUT else 0)
    # HANGUP/ERROR aren't requested explicitly - poll()/epoll report them in
    # revents regardless of the requested mask, and g_poll() picks them up the
    # same way, so there's nothing to opt into here.

on_fd_ready(fd, mask, data):     # wl_event_loop_fd_func_t
    (void)fd; (void)mask; (void)data
    pump()
    rearm()          # re-prepare for the next cycle; loops internally if more is
                      # immediately ready
    return 0

on_timeout(data):                # wl_event_loop_timer_func_t
    (void)data
    pump()
    rearm()
    return 0
```

`qt_glib_bridge_init()` itself:

```cpp
void qt_glib_bridge_init(BiomeServer *server) {
    g_loop = wl_display_get_event_loop(server->display);
    g_timeout_source = wl_event_loop_add_timer(g_loop, on_timeout, nullptr);

    if (!g_main_context_acquire(g_context)) {
        // Single-threaded process, called once at startup before anything else
        // touches g_main_context_default() - this should never fail. If it
        // somehow does, log loudly and bail out rather than silently running
        // degraded; there's no fallback path (see "risks" below).
        wlr_log(WLR_ERROR, "qt_glib_bridge: failed to acquire GMainContext - "
            "Qt/D-Bus dispatch will not work");
        return;
    }

    rearm();
}
```

Note `wl_event_source_timer_update(source, 0)` disarms the timer (standard libwayland
contract, same as an itimerspec of zero) — used whenever `query()` reports `timeout_ms == -1`
("block indefinitely," i.e. nothing time-based pending, only fd-driven).

### Why no explicit `g_main_context_release()`

GLib's contract is "acquire before calling prepare/query/check/dispatch yourself, to mark
this thread as the owner so a concurrent `g_main_loop_run()` elsewhere doesn't also try to
drive it." Biome is single-threaded and this bridge is the only thing that will ever drive
`g_main_context_default()` for the life of the process, so acquiring once at startup and
holding it until exit is correct and simpler than acquire/release around every cycle (which
is what `QEventDispatcherGlib::processEvents()` does today, since *it* has to coexist with
being called reentrantly/nested — the bridge doesn't have that constraint since it's the sole
owner throughout).

### CMake changes

`core/CMakeLists.txt` needs a new `pkg_check_modules` for glib (following the same
locally-scoped pattern `desktop/CMakeLists.txt` already uses for `xcb-ewmh`, rather than
hoisting it to the root `CMakeLists.txt` alongside wlroots/wayland-server/xkbcommon, since
only `core/` needs it):

```cmake
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)
```

...and add `PkgConfig::GLIB` to `target_link_libraries(biome PRIVATE ...)`, plus
`qt_glib_bridge.h`/`qt_glib_bridge.cpp` to the `add_executable(biome ...)` source list.
`glib-2.0` is already present on the target system (confirmed: `pkg-config --modversion
glib-2.0` → 2.84.4) as a transitive dependency of GTK/other desktop packages, but it isn't
currently declared as a Biome dependency anywhere — this makes it explicit, which is correct
regardless of that.

### Changes to existing files

**`ipc/global_shortcuts_portal.cpp`** — delete entirely:
- The anonymous-namespace block: `g_qt_pump_timer`, `kQtPumpActiveMs`/`kQtPumpIdleMs`/
  `kQtPumpIdleAfterTicks`, `g_qt_pump_idle_streak`, the diagnostic counters
  (`g_qt_pump_fires_since_log` etc.), and `pump_qt_events()` itself (lines ~186-237).
- The big rationale comment above that block (lines ~170-185) — the whole premise it explains
  (polling as a pragmatic stand-in for real fd integration) no longer applies.
- The `wl_event_loop_add_timer`/`wl_event_source_timer_update` calls at the end of
  `global_shortcuts_portal_init()` (lines ~267-270).
- Now-unused includes: `QAbstractEventDispatcher`, and `<ctime>` (only used by the deleted
  diagnostic logging).
- `global_shortcuts_portal_init()` goes back to doing exactly what its name says — register
  the D-Bus object/service — with dispatch being someone else's problem entirely (the bridge,
  already live before this function runs).

**`core/main.cpp`** — add `#include "core/qt_glib_bridge.h"` and one call,
`qt_glib_bridge_init(&server);`, immediately after `server.display = wl_display_create();`
(line 74), before `output_manager_init(&server);`.

**Optional, separate cleanup — `decoration/switcher.cpp`** (lines ~86-93): now that Qt's
dispatcher is genuinely live, `deleteLater()`'s `DeferredDelete` event will actually get
delivered, so the manual-delete workaround can revert:
```cpp
icon->deleteLater();
```
replacing `delete icon;` and its explanatory comment. This was already flagged in the
research above as safe under either fd-driven option and not load-bearing for the main
decision — worth doing in the same change since it's a one-line revert, but call it out as a
separate, easily-revertible hunk in the PR/commit if that's preferred.

**Not changed:** `decoration/frame_widget.cpp`'s `repolish_tree()` and
`force_activate_layouts()` — both stay, per the research above (neither is actually solved by
a live loop; see their reasoning higher up in this doc). `core/main.cpp`'s teardown sequence,
`wl_display_terminate()` handling, and `core/keybindings.cpp` are untouched — Option 1's
entire point is that `wl_display_run()` stays primary and none of that changes.

### Diagnostics to carry over

The old pump's periodic `WLR_DEBUG` summary line (fires/work-done every ~10s) was useful for
empirically confirming wakeup rates — worth keeping an equivalent for the same reason, now
tracking: `rearm()` cycles, `pump()` calls that actually dispatched vs. found nothing ready,
and current fd-source count. Same cadence/log level as before. This is also the practical
safety net for the theoretical livelock case below, not just a nice-to-have.

### Risks / edge cases

- **Livelock**: if some GLib/Qt source's `prepare()` always reports `immediate = true` (a bug
  in a source, not expected in normal Qt/QtDBus usage), `rearm()`'s inner loop spins forever
  without yielding back to `wl_event_loop_dispatch()`, starving every other Biome subsystem
  (rendering, input, XWayland). Not expected — Qt's own built-in sources don't do this in
  normal operation — but the diagnostic counters above make it visible immediately (cycle
  count spiking) rather than manifesting only as "compositor seems frozen." If this is ever
  observed in practice, the fix is bounding `rearm()`'s inner loop (e.g. cap iterations per
  call, defer the rest via `wl_event_loop_add_idle()`) rather than anything structural.
- **`g_main_context_acquire()` failing**: shouldn't happen (single-threaded, called once
  before anything else touches the context — validated above) but if it ever does, the
  fallback is "log and don't activate the bridge," same failure shape as today's "no D-Bus
  session bus available" handling in this same file. Qt/D-Bus dispatch simply doesn't happen
  in that case, same as before this change existed — not a new failure mode, just not masked
  by a working pump either.
- **New GLib-using code later**: since the bridge drives the actual process-wide
  `g_main_context_default()` rather than anything D-Bus-specific, any future code that adds
  its own `GSource`/`g_idle_add()`/etc. to the default context gets dispatched for free too —
  worth knowing as a (harmless) side effect, not just a limitation to work around.

### Testing plan

Nested run (`biome` inside an existing session, the normal dev/agent workflow per
`docs/history.md`), exercising:
- **Global shortcuts**: bind a shortcut via the portal, trigger it, confirm `Activated`/
  `Deactivated` signals actually arrive (this is the exact path that was silently broken
  without any pump at all, and polling-pumped before this change).
- **`cursor_bridge`/`workspace_bridge`**: a cursor-theme change and a workspace switch, same
  "does the D-Bus signal actually land" check.
- **Idle CPU / wakeup rate**: confirm near-zero wakeups at idle via the new diagnostic log
  (should show near-zero `rearm()`/`pump()` cycles when nothing's happening, vs. the old
  pump's guaranteed 10-40ms tick forever) and/or `top`/`ps` CPU%.
- **`deleteLater()` cleanup** (if the optional `switcher.cpp` change is included): cycle the
  window switcher through enough entries to exercise the shrink path, confirm no crash/leak.
- **Session-lock, layer-shell, XWayland launch, output handling**: unaffected by this change
  in principle (none of them go through Qt/GLib dispatch), but worth a smoke pass since
  `main.cpp`'s init ordering changed (bridge now inits before everything else).
- **`wl_display_terminate()` path still works** (the whole reason Option 2 was rejected):
  deliberately close the nested backend's host window, confirm Biome still exits cleanly —
  this path is untouched by Option 1, but it's cheap to re-confirm given it was the crux of
  the whole decision. **Already run (2026-09-08): it does not exit, but reproduces identically
  against a from-scratch pre-Option-1 baseline build — pre-existing, not a regression from
  this change.** See the "Post-implementation finding" note at the top of this doc. Don't
  re-run this as an Option-1 regression check; it's a separate, still-open bug.
