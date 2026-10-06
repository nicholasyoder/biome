# Biome

Wayland compositor on wlroots 0.18. See `docs/roadmap.md` for current status
and planned work, or `docs/history.md` for the original architecture/design
narrative.

## Building

```bash
cmake -B build -S .
cmake --build build -j$(nproc)
```

The `biome` executable is produced at `build/core/biome`. (`-j$(nproc)` runs compile jobs in parallel, one per CPU core.) Re-running this after making changes reconfigures only if needed and rebuilds incrementally, so it's safe to use both for the initial build and for rebuilds.

Builds default to `BIOME_DEV_BUILD=ON` (ALT+Escape quits the compositor, debug-level logging). The Debian package builds with it `OFF`; set `BIOME_DEBUG=1` at runtime to get debug logging there.

### Dependencies

- `wlroots-0.18` (pkg-config)
- `wayland-server`
- `xkbcommon`

## Packaging

A Debian package can be built from `debian/` with:

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
