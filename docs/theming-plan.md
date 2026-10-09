# External decoration themes — Plan

Let a shell theme Biome's decorations and Alt-Tab switcher from its own
layered theme files, so one theme (e.g. Forest's Round-Dark) can style the
shell and the compositor from one place. Status: **proposed, not started**.

## Shape

Biome learns one generic feature: "load a layered QSS theme named `name`
from directory `dir`". It knows nothing about Forest. Forest ships
`biome.css` files in its existing theme layers and points Biome at them via
config. Coupling runs one way (Forest → Biome's documented config and
selectors), which the decoupling goal allows.

Appearance is not policy, so this doesn't conflict with "fixed-policy, not
user-configurable"; Biome already reads output and fade config the same way.

## Config

Two keys in Biome's merged config (`core/config.h`: `/etc/biome/conf.d/*.conf`,
then `~/.config/Biome/Biome.conf`):

```ini
[Theme]
dir=/usr/share/forest/themes
name=Round-Dark
```

- `Theme/dir` — directory holding theme subdirectories.
- `Theme/name` — subdirectory to load.

Either key unset → built-in theme.

## Loading

Same algorithm as Forest's `fstyleloader::loadstyle("biome")`:

1. Read `<dir>/<name>/theme.conf` (INI). Its `parent_themes` key (comma
   list, `[General]` group) names the parent layers, in order.
2. Layers = `parent_themes` + `name` itself.
3. Concatenate `<dir>/<layer>/biome.css` for each layer, in order. A layer
   without a `biome.css` is skipped silently; an unreadable one is logged
   and skipped.
4. Empty result (theme missing, or no layer has `biome.css`) → built-in theme.

An external theme **replaces** the built-in one, it isn't layered on top of
it. That way built-in changes never leak into a shell's theme, and a theme
must style every selector it cares about.

This `theme.conf` format becomes a format Biome documents and reads.
Document it in `architecture-notes.md` once implemented; don't change it
without changing Forest's loader too.

## Built-in theme

Stays compiled in (`decoration/theme/theme.qrc`), as a single fallback that
looks sane with no shell installed. It doesn't need the layered format. Its
content is today's `biome-dark.qss`; rename to something neutral (e.g.
`builtin.qss`) since it's no longer "the dark one".

## Selector contract

What external themes target, so it becomes a stable API: renaming any of
these silently breaks shell themes. List it in `architecture-notes.md` and
treat changes as breaking.

| Selector | Widget |
|---|---|
| `biome_decoration--DecorationFrame#biomeFrame` | Whole frame; props `focused`, `biomeMaximized` |
| `QWidget#biomeTitlebar` | Titlebar |
| `QLabel#biomeTitle` | Title text |
| `QToolButton#biomeTitleIcon` | App icon |
| `biome_decoration--DecorationBorder`, `#biomeBorderLeft/Right/Bottom` | Borders |
| `biome_decoration--DecorationButton#biomeButtonMinimize/Maximize/Close` | Buttons (`qproperty-icon`, `:hover`, `:pressed`) |
| `#biomeContent` | Client-area spacer |
| `QWidget#biomeSwitcherRoot`, `QFrame#biomeSwitcherPanel` | Switcher |
| `QToolButton#biomeSwitcherIcon` | Switcher entry; prop `selected` |
| `QLabel#biomeSwitcherTitleLabel` | Switcher title |
| `QWidget#biomeSwitcherHighlightRoot`, `QFrame#biomeSwitcherHighlight` | Switcher highlight |

The `biome_decoration--` type selectors tie the C++ namespace and class
names into the contract too.

Icon `url()`s: Qt resolves relative paths against the process's working
directory, not the `.css` file, so themes use absolute paths. A theme may
still use Biome's built-in `:/biome/decoration/icons/*.svg`, but those are
dark-theme-coloured, so a light theme needs its own.

## Live reload

`biome_config()` is cached at startup. Theme keys need a fresh read:

- Watch `~/.config/Biome/` (directory, not the file: `QSettings` writes by
  atomic rename) with `QFileSystemWatcher`, like `ipc/cursor_theme_watcher`.
- On change, re-read only `Theme/*` (fresh `QSettings` merge, not the cached
  `BiomeConfig`). Unchanged → no-op; Biome's own `[Outputs]` writes land in
  the same file.
- Changed → rebuild the stylesheet, `qApp->setStyleSheet()` (re-polishes
  existing widgets), then for every toplevel re-run layout and re-render its
  frame. Border width and titlebar height can change, so content geometry
  and the toplevel's configure size must be re-derived too
  (`desktop/decoration_bridge`), not just the image.
- Drop-in changes (package upgrades) don't need to apply live.

The blurry-on-scaled-outputs fix (roadmap 0.2.0) needs the same
"re-render every frame" path; build it once.

## Forest side

- Add `biome.css` to the theme layers: `base` (shapes/sizes shared by all),
  `base-dark` / `base-light` (colours, button icons), `base-rounded` /
  `base-circle` (radii). Start by splitting today's `biome-dark.qss` across
  `base` + `base-dark` + `base-rounded`, then write the light and circle
  layers. Button SVGs ship under the colour layers, referenced by absolute
  path.
- `etc/biome/conf.d/50-forest.conf` gains `[Theme] dir=/usr/share/forest/themes`
  and `name=` Forest's default theme (`Round-Dark`, matching
  `fstyleloader`'s default). Fresh installs then match with no user config
  and no upgrade step.
- `ForestThemeSettings::set_desktop_theme()` also writes `Theme/name` to
  `QSettings("Biome", "Biome")` alongside Forest's own `theme`. Biome picks
  it up via its watcher; no D-Bus call into Biome. `QSettings` merges on
  sync under a lock file, so this doesn't clobber Biome's `[Outputs]` writes.
- Forest never reads Biome's files; Biome never reads Forest's beyond the
  directory it's pointed at.

## Testing

- No `[Theme]` keys (Biome alone) → built-in theme, unchanged from today.
- Bad `name`, or a theme with no `biome.css` layers → built-in, one log line.
- Forest session → decorations match the selected theme; switching theme in
  settings restyles open windows and the switcher live, with titlebar/border
  size changes reflowing window content correctly.
