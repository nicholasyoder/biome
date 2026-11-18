cmake_minimum_required(VERSION 3.16)
# C: the integration test client and shim (tests/integration).
project(biome VERSION 0.3.1 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Off for distro packages (debian/rules): drops the ALT+Escape kill chord and
# defaults logging to info instead of debug.
option(BIOME_DEV_BUILD "Developer conveniences (ALT+Escape quit, debug logging)" ON)
if(BIOME_DEV_BUILD)
    add_compile_definitions(BIOME_DEV_BUILD)
endif()

# Custom cmake modules
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(BiomeProtocols)
include(BiomeWlrootsShim)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WLROOTS REQUIRED IMPORTED_TARGET wlroots-0.20)
pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)

# Generated once here (not per-subdirectory) since both desktop/ and core/
# need it - wlroots' own headers assume xdg-shell-protocol.h is on the
# include path, and CMake only allows one add_custom_command per OUTPUT. See
# BiomeProtocols.cmake for why consumers need XDG_SHELL_PROTOCOL_HEADER_TARGET
# rather than just listing XDG_SHELL_PROTOCOL_HEADER as a source.
biome_generate_protocol_header(XDG_SHELL_PROTOCOL_HEADER XDG_SHELL_PROTOCOL_HEADER_TARGET
    "${WAYLAND_PROTOCOLS_DATADIR}/stable/xdg-shell/xdg-shell.xml")

# wlr-layer-shell-unstable-v1 isn't part of the wayland-protocols package
# (unlike xdg-shell above) - it originates in wlroots' own protocol/ dir and
# isn't installed by the wlroots-0.20-dev package either, so a copy is
# vendored here instead of assuming a system path for it. wlr/types/
# wlr_layer_shell_v1.h itself #includes the generated
# "wlr-layer-shell-unstable-v1-protocol.h" by name, so this only needs
# generating and putting on the include path (BIOME_PROTOCOL_DIR, below) -
# nothing in Biome's own code includes it directly.
biome_generate_protocol_header(LAYER_SHELL_PROTOCOL_HEADER LAYER_SHELL_PROTOCOL_HEADER_TARGET
    "${CMAKE_SOURCE_DIR}/protocol/wlr-layer-shell-unstable-v1.xml")

# cursor-shape-v1 - like xdg-shell/layer-shell above, wlroots implements this
# protocol itself (wlr/types/wlr_cursor_shape_v1.h, included via
# core/wlroots.hpp), it just needs the generated interface/enum header on the
# include path. Part of the wayland-protocols package (staging/), no
# vendoring needed.
biome_generate_protocol_header(CURSOR_SHAPE_PROTOCOL_HEADER CURSOR_SHAPE_PROTOCOL_HEADER_TARGET
    "${WAYLAND_PROTOCOLS_DATADIR}/staging/cursor-shape/cursor-shape-v1.xml")

# Vendored for the same reason as layer-shell above; included by name from
# wlr/types/wlr_output_power_management_v1.h.
biome_generate_protocol_header(OUTPUT_POWER_PROTOCOL_HEADER OUTPUT_POWER_PROTOCOL_HEADER_TARGET
    "${CMAKE_SOURCE_DIR}/protocol/wlr-output-power-management-unstable-v1.xml")

# Included by name from wlr/types/wlr_pointer_constraints_v1.h.
biome_generate_protocol_header(POINTER_CONSTRAINTS_PROTOCOL_HEADER POINTER_CONSTRAINTS_PROTOCOL_HEADER_TARGET
    "${WAYLAND_PROTOCOLS_DATADIR}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml")

add_subdirectory(decoration)
add_subdirectory(desktop)
add_subdirectory(ipc)
add_subdirectory(core)

enable_testing()
add_subdirectory(tests)

# Registers biome as an org.freedesktop.impl.portal.GlobalShortcuts backend
# with the system's xdg-desktop-portal frontend - biome itself owns the
# org.freedesktop.impl.portal.desktop.biome bus name at runtime (see
# ipc/global_shortcuts_portal.cpp), these two files just make the frontend
# aware it exists and should be preferred for that interface.
# Drop-ins for other packages (e.g. a shell); see core/config.h.
install(DIRECTORY DESTINATION /etc/biome/conf.d)

install(FILES data/xdg-desktop-portal/biome-portals.conf
    DESTINATION /usr/share/xdg-desktop-portal)
install(FILES data/xdg-desktop-portal/portals/biome.portal
    DESTINATION /usr/share/xdg-desktop-portal/portals)
