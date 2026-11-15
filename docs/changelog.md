Changelog
============

* Unreleased
  - Fix a crash opening the snap menu on a window that had just closed or hidden itself, a crash when the pointer left every monitor while dwelling at a snap zone, a ~36 KB icon leak per closed window, and a window mapped onto another workspace stealing keyboard focus while invisible (it is marked urgent instead); a window that hides and reappears now comes back as a fresh one (not maximized/fullscreen/snapped/minimized)
  - Fix global shortcuts with Shift+letter never firing and letter shortcuts dying under Caps Lock; `SHIFT+1` and `SHIFT+exclam` both match now
  - GlobalShortcuts portal: refuse bare-key shortcuts (no non-Shift modifier, other than F-keys, XF86 keys, Print/Pause) and triggers with empty parts like `CTRL+`; fix the `ListShortcuts`/`ConfigureShortcuts` signatures and export the interface `version`
  - Fix a shortcut staying "held" when its keyboard was unplugged mid-press, and the seat still advertising a keyboard after the last one was removed
  - Fix an abort on exit (every time the greeter's biome hands off to the session): wlroots' listener asserts on teardown
  - Fix a crash when a window that hid itself without closing (unmapped) then asked to be minimized or maximized, and stop such hidden windows leaving their title bar on screen or staying in Alt-Tab
  - Fix a crash when a monitor was unplugged (or dropped out on wake) while the screen was locked, and a black lock screen when a disabled monitor's blank lock rect landed over another monitor
  - Fix the screen staying locked after a correct password when the lock started with every monitor off (Forest's lock-on-display-off): the `locked` event was never sent
  - Fix keystrokes typed on the lock screen going to a background app when one of its menus was open as the lock started, or to the panel once that menu closed; the lock screen now always gets the keyboard, including when its monitor is unplugged. The Alt-Tab switcher closes on lock
  - Fix a crash when an app bound a global shortcut without a description through the GlobalShortcuts portal

* Release 0.3.0 - 2026-11-13
  - Add content-type-v1 and alpha-modifier-v1 (clients can now set per-surface opacity)
  - Read the theme's [Palette]: it sets the app palette, and the window-snap and Alt-Tab highlight color now follows the theme's highlight color. Theme loading now comes from Forest's forest-theme library (new build dependency: forest-theme-devel)
  - Add a compositor-side window menu (restore/maximize, minimize, move, resize, always on top, move to workspace, close), opened by right-clicking the titlebar, clicking the titlebar icon, or a client-side-decorated app's own titlebar menu; styled by the theme's shared.css, shared with Forest
  - Add always-on-top windows
  - Add window snapping: drag a titlebar to a screen edge or corner (with a preview) to tile to a half, quarter or maximize, or right-click / press-and-hold the maximize button for a snap menu with layout icons
  - Add ext-image-copy-capture-v1 with per-window and per-output capture sources; hidden (minimized or other-workspace) windows capture too

* Release 0.2.1 - 2026-11-11
  - Port to Fedora 45 and wlroots 0.20
  - Fix a crash when a layer-shell popup was created (xdg_popup unconstraining now deferred to the initial commit)
  - Force DRM Broadcast RGB to Full; per-output rgb_range override in Biome.conf

* Release 0.2.0 - 2026-11-09
  - Add protocols: xdg-activation-v1, wlr-data-control, pointer-constraints, relative-pointer, pointer-gestures, wlr-gamma-control, single-pixel-buffer, xdg-shell v6
  - Decorations: load external layered QSS themes with live reload, render at the output's scale
  - GlobalShortcuts: send Deactivated on key release, idempotent BindShortcuts, reject colliding bindings
  - Enforce layer-shell keyboard_interactivity; many output, Xwayland and fullscreen fixes (see debian/changelog)

* Release 0.1.0 - 2026-10-05
  - First release: a fixed-policy floating Wayland compositor on wlroots, built as the compositor for the Forest desktop
