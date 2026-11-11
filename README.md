# Biome

Wayland compositor on wlroots 0.20. See `docs/roadmap.md` for current status
and planned work, or `docs/history.md` for the original architecture/design
narrative.

## Building

```bash
cmake -B build -S .
cmake --build build -j$(nproc)
```

The `biome` executable is produced at `build/core/biome`. (`-j$(nproc)` runs compile jobs in parallel, one per CPU core.) Re-running this after making changes reconfigures only if needed and rebuilds incrementally, so it's safe to use both for the initial build and for rebuilds.

Builds default to `BIOME_DEV_BUILD=ON` (ALT+Escape quits the compositor, debug-level logging). Distro packages build with it `OFF`; set `BIOME_DEBUG=1` at runtime to get debug logging there.

### Dependencies

- `wlroots-0.20` (pkg-config)
- `wayland-server`, `wayland-protocols`, `xkbcommon`, `pixman`, `glib-2.0`
- `xcb-ewmh`, Qt 6 Widgets

On Fedora:

```bash
sudo dnf install gcc-c++ cmake pkgconf-pkg-config wlroots-devel wayland-devel \
  wayland-protocols-devel libxkbcommon-devel pixman-devel glib2-devel \
  qt6-qtbase-devel xcb-util-wm-devel
```

## Packaging

### RPM (Fedora)

```bash
sudo dnf install rpm-build rpmdevtools           # once; plus the build deps above
git archive --prefix=biome-0.2.1/ -o ~/rpmbuild/SOURCES/biome-0.2.1.tar.gz HEAD   # Source0 is the GitHub tag tarball
rpmbuild -ba packaging/biome.spec
```

Output lands in `~/rpmbuild/RPMS/`. For a clean-chroot build, run `mock -r fedora-45-x86_64`
on the resulting `.src.rpm`. Keep the spec's `Version` and `%changelog` in step with
`CMakeLists.txt` at each release.

### Debian

`debian/` is kept for now but targets wlroots 0.18 and no longer builds on Trixie. A Debian package can be built from `debian/` with:

```bash
sudo apt build-dep .            # once: installs debian/control's Build-Depends
dpkg-buildpackage -us -uc -b    # or `debuild -us -uc` (devscripts) to also run lintian
```

This produces `../biome_<version>_amd64.deb` (plus a `-dbgsym` package); it doesn't install anything locally. The version comes from the top entry of `debian/changelog`.

It builds the working tree as-is, uncommitted changes included.

It leaves build byproducts in the tree (`obj-*-linux-gnu/`, `debian/biome/`, etc., all gitignored). Remove them with:

```bash
dpkg-buildpackage -Tclean
```
