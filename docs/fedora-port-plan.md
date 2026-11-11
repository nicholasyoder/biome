# Fedora port plan (Debian Trixie → Fedora 45, wlroots 0.18 → 0.20.2)

Multi-session working plan; replaces the old `wlroots-port-plan.md` (patching
wlroots 0.20 onto Trixie), which is moot. Delete this file once the 0.10.1-era
release has shipped and lasting facts have moved into `history.md` and
`architecture-notes.md`. Companion plan: `forest/docs/fedora-port-plan.md`.

**Goal:** Biome builds, runs and ships as an RPM on Fedora, functionally the
same as 0.2.0, as release **0.2.1**. No new features in this pass.

## Findings (2026-11-10, Fedora 45, checked with `dnf`)

- `wlroots-devel` is **0.20.2** (pkg-config `wlroots-0.20`). Fedora also has
  `wlroots0.18-devel` (0.18.3) and `wlroots0.19-devel`, but we target 0.20:
  it is the only version with `ext-foreign-toplevel-image-capture-source-v1`
  and the one that has the xwm security fixes.
- `wayland-protocols-devel` 1.50, `wayland-devel` 1.26, `libxkbcommon-devel`
  1.13, Qt 6.11.2. Nothing needs patching or backporting.
- Nothing is installed yet (no cmake, compiler, or devel packages).
- SELinux is enforcing.

## Build dependencies

```sh
sudo dnf install gcc-c++ cmake pkgconf-pkg-config rpm-build rpmdevtools \
  wlroots-devel wayland-devel wayland-protocols-devel libxkbcommon-devel \
  pixman-devel glib2-devel qt6-qtbase-devel xcb-util-wm-devel
```

(`xcb-util-wm-devel` provides `xcb-ewmh`, used by `desktop/`.) Debian's
`libwayland-bin` (wayland-scanner) is part of `wayland-devel` here.

## Steps

Work on a branch off `develop` (e.g. `fedora-wlroots-0.20`); one PR for 1–4.

### 1. Build setup

- [ ] Install the dependencies above; `cmake -B build -S . && cmake --build build`
      against the *unchanged* tree to confirm the toolchain works and see what
      breaks (expected: wlroots 0.18 not found).
- [ ] Switch `CMakeLists.txt` and `cmake/BiomeWlrootsShim.cmake` to
      `wlroots-0.20`.
- [ ] Re-check the shim against 0.20 headers: `[static 4]` in `wlr_scene.h`,
      `char *class` in `xwayland.h`, `char *namespace` in `wlr_layer_shell_v1.h`.
      `wlr_matrix.h` may no longer have `[static 9]` or be needed at all.
      Grep new headers for further C++ keyword collisions.
- [ ] Check the vendored protocols (`protocol/`) still match what 0.20's
      headers `#include` by name, and whether wayland-protocols 1.50 now ships
      any we vendor or generate (`WAYLAND_PROTOCOLS_DATADIR` paths still exist).

### 2. wlroots 0.18 → 0.20 API port

Read the 0.19.0 and 0.20.0 release notes for intent; don't just make it compile.
Known breaks (from the old plan's source check, line numbers may have moved):

- [ ] `wlr_swapchain_acquire()` lost its age argument (`desktop/layer_shell_fade.cpp`).
- [ ] `wlr_xdg_surface_get_geometry()` removed → `xdg_surface->geometry`
      (`desktop/toplevel.cpp`).
- [ ] `wlr_xwayland_or_surface_wants_focus()` renamed
      `wlr_xwayland_surface_override_redirect_wants_focus()` (`desktop/xwayland_shell.cpp`).
- [ ] Remaining struct-field / signal changes in output, scene, seat, xwayland.
- [ ] Unit tests (`tests/`) pass.

### 3. Re-verify behaviour that depended on 0.18

Each note records 0.18 behaviour Biome relies on or works around; check against
0.20, then update or delete the note and workaround.

- [ ] `architecture-notes.md` (~110): closing the nested backend's host window
      calls `wlr_output_destroy()`, not terminate.
- [ ] `architecture-notes.md` (~298) and `desktop/idle.cpp` (~33): stale
      `active_outputs` under a disabled ancestor.
- [ ] `architecture-notes.md` (~328): screencopy stall on a disabled output
      (believed fixed in 0.19, 31f9d6bb).
- [ ] `grep -rn "wlroots 0\."` for other version-specific comments.

### 4. RPM packaging

- [ ] `packaging/biome.spec` (or `rpm/`; decide location, keep `debian/` until
      the .deb line is formally dropped). `BuildRequires` per the list above;
      configure with `-DCMAKE_BUILD_TYPE=Release -DBIOME_DEV_BUILD=OFF` via
      `%cmake`. Runtime: `Recommends: xdg-desktop-portal`, `Requires: xorg-x11-server-Xwayland`
      (check what Debian's shlibs/Xwayland handling provided).
- [ ] Install paths: `install(...)` rules use literal `/usr/share/...` and
      `/etc/biome/conf.d`; `%cmake` sets `CMAKE_INSTALL_PREFIX=/usr`, so confirm
      the binary lands in `/usr/bin` and nothing goes to `/usr/lib64` by accident.
- [ ] Man pages (`debian/biome.manpages`, `debian/man`) → `%files` `%{_mandir}`.
- [ ] Changelog: spec `%changelog` for 0.2.1. Decide whether `docs/changelog`
      stays the source (Forest regenerates `debian/changelog` from it; mirror
      that for the spec).
- [ ] `rpmbuild -ba` / `mock -r fedora-45-x86_64` clean-chroot build succeeds;
      `rpmlint` output reviewed.
- [ ] Update `README.md` (build/packaging sections, "wlroots 0.18" → 0.20)
      and `CLAUDE.md`.

### 5. Manual regression pass (user-driven, real hardware)

- [ ] DRM session start from greetd/TTY, VT switch, suspend/resume
- [ ] Multi-monitor hotplug, output management (Forest Displays), DPMS
- [ ] XWayland: focus, override-redirect menus, restacking
- [ ] Layer-shell: panel, desktop, fade animation, reconfigure storm
- [ ] Session lock, idle inhibit, screencopy (grim)
- [ ] Gestures, pointer constraints, cursor-shape, gamma
- [ ] GlobalShortcuts portal under Fedora's `xdg-desktop-portal` 1.22
      (`biome-portals.conf` / `biome.portal` still picked up)
- [ ] Nested (`BIOME_DEV_BUILD`) run
- [ ] SELinux: run under enforcing and check `ausearch -m avc -ts recent`
      (the compositor opens DRM/input devices via logind/libseat).

### 6. Release 0.2.1

- [ ] Version bump in `CMakeLists.txt`, changelog entry ("Port to Fedora and
      wlroots 0.20.2; no functional changes").
- [ ] User merges the PR, tags and builds the RPM.

## After the release (not part of the port)

- Update roadmap's "wlroots bump" entry (done once 0.20 lands) and
  `history.md`'s Trixie/0.18 target rationale.
- Remove root `CLAUDE.md`'s "Biome uses 0.18.2" note about `misc/wlroots`
  (point it at 0.20.2).
- **Capture protocols**: `ext-image-copy-capture-v1` plus output and
  foreign-toplevel capture sources, then Forest windowlist previews. Carried
  over from the old plan:
  - Create the three managers; handle `new_request` by mapping the
    `ext_foreign_toplevel_handle_v1` to its `BiomeToplevel` and accepting with
    that toplevel's scene node (check `foreign_toplevel.c`/`scene.c` in 0.20.2).
  - Policy for minimized / other-workspace windows (disabled scene node ⇒ empty
    frames; previews of minimized windows are the main use case): last frame,
    or render off-screen.
  - Same security stance as screencopy (unrestricted).
  - Forest side: raw `wayland-client` binding, single-frame shm capture →
    `QImage` in `imagepopup::get_window_image()`; optionally live previews.
