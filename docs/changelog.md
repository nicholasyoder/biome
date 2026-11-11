Changelog
============

* Unreleased
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
