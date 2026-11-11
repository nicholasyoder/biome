Name:           biome
Version:        0.2.0
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
