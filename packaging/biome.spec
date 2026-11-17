Name:           biome
Version:        0.3.1
Release:        1%{?dist}
Summary:        Fixed-policy Wayland compositor for Forest

License:        LGPL-3.0-or-later
URL:            https://github.com/nicholasyoder/biome
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  cmake
BuildRequires:  pkgconfig(wlroots-0.20)
BuildRequires:  pkgconfig(wayland-server)
BuildRequires:  pkgconfig(wayland-protocols)
BuildRequires:  pkgconfig(xkbcommon)
BuildRequires:  pkgconfig(pixman-1)
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(xcb-ewmh)
BuildRequires:  cmake(Qt6Widgets)
BuildRequires:  cmake(ForestTheme)

Requires:       xorg-x11-server-Xwayland
Recommends:     xdg-desktop-portal

%description
Biome is an opinionated, single-policy Wayland compositor built on
wlroots, used as Forest's compositor. It is not user-configurable
like sway or river; policy is fixed in code.

%prep
%autosetup

%build
%cmake -DCMAKE_BUILD_TYPE=Release -DBIOME_DEV_BUILD=OFF
%cmake_build

%install
%cmake_install
install -Dpm 0644 debian/man/biome.1 %{buildroot}%{_mandir}/man1/biome.1

%check
%ctest

%files
%license LICENSE
%{_bindir}/biome
%{_mandir}/man1/biome.1*
%dir %{_sysconfdir}/biome
%dir %{_sysconfdir}/biome/conf.d
%{_datadir}/xdg-desktop-portal/biome-portals.conf
%{_datadir}/xdg-desktop-portal/portals/biome.portal

%changelog
* Tue Nov 17 2026 Nicholas Yoder <nicholasyoder@proton.me> - 0.3.1-1
- Handle SIGTERM/SIGINT with a clean shutdown (logout now runs the normal
  teardown and stops the `-s` command), and fix the Qt/GLib bridge losing an
  event source when a file descriptor number was reused
- Fix Meta/Ctrl/Alt shortcuts (e.g. Meta+L to lock) not working while the
  window menu is open, and Meta then opening the main menu on release
- Session lock: the Alt-Tab switcher, window menu and drag icon now sit below
  the lock screen instead of above it; window captures (ext-foreign-toplevel
  image capture) are blank while locked; a monitor plugged in during an
  abandoned lock shows the red rect; the pointer picks up the surface under it
  on unlock; new popups are refused while locked
- Fix a monitor whose first modeset failed being treated as live: a rejected
  configured mode now retries with the monitor's preferred mode, and if that
  fails too the monitor stays disabled (and is reported so) instead of holding
  windows and layout space. The pointer now picks up the surface under it
  after being pulled back onto a remaining monitor
- Fix layer-shell clients (e.g. another shell's panel) hanging when they unmap
  and re-map a surface, and stop reconfiguring every surface on the output
  twice whenever a new one appears (a notification popping up no longer makes
  every panel recommit). ext-foreign-toplevel-list now reports real titles and
  app ids from the start. Scanout fades only apply to full-output surfaces,
  others use an opacity fade
- Fix X11 windows losing their frame (and shifting content) after leaving
  fullscreen; X11 windows no longer move or resize themselves while maximized,
  fullscreen or snapped, interactive resize respects their min/max size hints,
  and a title change that reused the old text's memory no longer skips the
  title-bar redraw; X11 clients changing their decoration hints now update
  live
- Fix windows staying on the other monitor after a monitor dropped out and
  returned (e.g. a DPMS blank bounce): they now go back to it, unless you
  moved them meanwhile. Fix maximized windows staying a panel-height short
  when the usable area briefly shrank and recovered before the client acked
  the resize
- Fix two windows showing as active in the taskbar after the last window was
  minimized or the screen locked; maximizing a fullscreen window no longer
  shrinks it while fullscreen; dragging a window that mapped maximized no
  longer jumps; Ctrl-Alt-Shift-arrow moves the active window, not an unseen
  one; `MoveToplevelToWorkspace` ignores out-of-range workspaces
- Fix a crash opening the snap menu on a window that had just closed or hidden
  itself, a crash when the pointer left every monitor while dwelling at a snap
  zone, a ~36 KB icon leak per closed window, and a window mapped onto another
  workspace stealing keyboard focus while invisible (it is marked urgent
  instead); a window that hides and reappears now comes back as a fresh one
  (not maximized/fullscreen/snapped/minimized)
- Fix global shortcuts with Shift+letter never firing and letter shortcuts
  dying under Caps Lock; `SHIFT+1` and `SHIFT+exclam` both match now
- GlobalShortcuts portal: refuse bare-key shortcuts (no non-Shift modifier,
  other than F-keys, XF86 keys, Print/Pause) and triggers with empty parts
  like `CTRL+`; fix the `ListShortcuts`/`ConfigureShortcuts` signatures and
  export the interface `version`
- Match global-shortcut key names case-insensitively, so lowercase triggers
  like `CTRL+escape` and `xf86audiomute` bind (only exact-case keysym names
  did before)
- Fix a shortcut staying "held" when its keyboard was unplugged mid-press, and
  the seat still advertising a keyboard after the last one was removed
- Fix an abort on exit (every time the greeter's biome hands off to the
  session): wlroots' listener asserts on teardown
- Fix a crash when a window that hid itself without closing (unmapped) then
  asked to be minimized or maximized, and stop such hidden windows leaving
  their title bar on screen or staying in Alt-Tab
- Fix a crash when a monitor was unplugged (or dropped out on wake) while the
  screen was locked, and a black lock screen when a disabled monitor's blank
  lock rect landed over another monitor
- Fix the screen staying locked after a correct password when the lock started
  with every monitor off (Forest's lock-on-display-off): the `locked` event
  was never sent
- Fix keystrokes typed on the lock screen going to a background app when one
  of its menus was open as the lock started, or to the panel once that menu
  closed; the lock screen now always gets the keyboard, including when its
  monitor is unplugged. The Alt-Tab switcher closes on lock
- Fix a crash when an app bound a global shortcut without a description
  through the GlobalShortcuts portal

* Fri Nov 13 2026 Nicholas Yoder <nicholasyoder@proton.me> - 0.3.0-1
- Add content-type-v1 and alpha-modifier-v1 (clients can now set per-surface
  opacity)
- Read the theme's [Palette]: it sets the app palette, and the window-snap and
  Alt-Tab highlight color now follows the theme's highlight color. Theme
  loading now comes from Forest's forest-theme library (new build dependency:
  forest-theme-devel)
- Add a compositor-side window menu (restore/maximize, minimize, move, resize,
  always on top, move to workspace, close), opened by right-clicking the
  titlebar, clicking the titlebar icon, or a client-side-decorated app's own
  titlebar menu; styled by the theme's shared.css, shared with Forest
- Add always-on-top windows
- Add window snapping: drag a titlebar to a screen edge or corner (with a
  preview) to tile to a half, quarter or maximize, or right-click /
  press-and-hold the maximize button for a snap menu with layout icons
- Add ext-image-copy-capture-v1 with per-window and per-output capture
  sources; hidden (minimized or other-workspace) windows capture too

* Wed Nov 11 2026 Nicholas Yoder <nicholasyoder@proton.me> - 0.2.1-1
- Port to Fedora 45 and wlroots 0.20
- Fix a crash when a layer-shell popup was created (xdg_popup unconstraining
  now deferred to the initial commit)
- Force DRM Broadcast RGB to Full; per-output rgb_range override in Biome.conf

* Mon Nov 09 2026 Nicholas Yoder <nicholasyoder@proton.me> - 0.2.0-1
- Protocols: xdg-activation-v1, wlr-data-control, pointer-constraints,
  relative-pointer, pointer-gestures, wlr-gamma-control, single-pixel-buffer,
  xdg-shell v6
- Decorations: load external layered QSS themes with live reload, render at
  the output's scale
- GlobalShortcuts: Deactivated on key release, idempotent BindShortcuts,
  reject colliding bindings
- Enforce layer-shell keyboard_interactivity; many output, Xwayland and
  fullscreen fixes (see debian/changelog)

* Mon Oct 05 2026 Nicholas Yoder <nicholasyoder@proton.me> - 0.1.0-1
- First release: a fixed-policy floating Wayland compositor on wlroots,
  built as the compositor for the Forest desktop
