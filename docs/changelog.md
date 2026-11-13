Changelog
============

* Unreleased
  - Add content-type-v1 and alpha-modifier-v1 (clients can now set per-surface opacity)
  - Read the theme's [Palette]: it sets the app palette, and the window-snap and Alt-Tab highlight color now follows the theme's highlight color. Theme loading now comes from Forest's forest-theme library (new build dependency: forest-theme-devel)
  - Add a compositor-side window menu (restore/maximize, minimize, move, resize, always on top, move to workspace, close), opened by right-clicking the titlebar, clicking the titlebar icon, or a client-side-decorated app's own titlebar menu; styled by the theme's shared.css, shared with Forest
  - Add always-on-top windows
  - Add window snapping: drag a titlebar to a screen edge or corner (with a preview) to tile to a half, quarter or maximize, or right-click / press-and-hold the maximize button for a snap menu
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
