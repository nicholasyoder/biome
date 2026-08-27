# Biome

Wayland compositor on wlroots 0.18. See `docs/plan.md` for architecture and roadmap.

## Building

An out-of-tree `build/` directory is already configured (Unix Makefiles). To rebuild after making changes:

```bash
cd build
cmake --build . -j$(nproc)
```

The `biome` executable is produced at `build/core/biome`. (`-j$(nproc)` runs compile jobs in parallel, one per CPU core.)

To reconfigure from scratch (e.g. after editing top-level `CMakeLists.txt` or deleting `build/`):

```bash
cmake -B build -S .
cmake --build build -j$(nproc)
```

### Dependencies

- `wlroots-0.18` (pkg-config)
- `wayland-server`
- `xkbcommon`
