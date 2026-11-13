# Biome

Wayland compositor on wlroots 0.20, built as the compositor for the
[Forest](https://github.com/nicholasyoder/forest) desktop. It targets Fedora 45; download the
RPM from the [releases page](https://github.com/nicholasyoder/biome/releases) and install it with
`sudo dnf install ./biome-0*.rpm`, or build it yourself (see [Packaging](#rpm-fedora)). See `docs/roadmap.md`
for current status and planned work, or `docs/history.md` for the original architecture/design
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
- `ForestTheme` CMake package (header-only theme loader from the [Forest](https://github.com/nicholasyoder/forest) repo, `forest-theme-devel` package; build-time only)

On Fedora:

```bash
sudo dnf install gcc-c++ cmake pkgconf-pkg-config wlroots-devel wayland-devel \
  wayland-protocols-devel libxkbcommon-devel pixman-devel glib2-devel \
  qt6-qtbase-devel xcb-util-wm-devel forest-theme-devel
```

If `forest-theme-devel` isn't installed system-wide (e.g. Forest is only staged
from a build tree), point CMake at it: `cmake -B build -DCMAKE_PREFIX_PATH=<prefix containing share/cmake/ForestTheme>`.
The path is cached, so later rebuilds don't need it again.

## Packaging

### RPM (Fedora)

```bash
sudo dnf install rpm-build rpmdevtools           # once; plus the build deps above
mkdir -p rpmbuild/SOURCES
git archive --prefix=biome-0.2.1/ -o rpmbuild/SOURCES/biome-0.2.1.tar.gz HEAD   # stands in for the release tarball the spec expects
rpmbuild -ba --define "_topdir $PWD/rpmbuild" packaging/biome.spec
```

Run from the repo root; output lands in `rpmbuild/RPMS/` (gitignored). To use the default
`~/rpmbuild` instead, drop the `--define`, use `~/rpmbuild/SOURCES` for the tarball, and
run `rpmdev-setuptree` once. For a clean-chroot build, run `mock -r fedora-45-x86_64`
on the resulting `.src.rpm`. Keep the spec's `Version` and `%changelog` in step with
`CMakeLists.txt` at each release.

### Debian (no longer fully supported)

`debian/` is kept for now but is untested, and no `.deb`s are published for new releases (the last was 0.2.0, for Debian 13 "Trixie"). It targets wlroots 0.20, so it no longer builds on Trixie (wlroots 0.18) and is untested on Debian sid. A Debian package can be built from `debian/` with:

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
