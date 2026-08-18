cmake_minimum_required(VERSION 3.16)
project(biome VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Custom cmake modules
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(BiomeProtocols)
include(BiomeWlrootsShim)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WLROOTS REQUIRED IMPORTED_TARGET wlroots-0.18)
pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)

# Generated once here (not per-subdirectory) since both desktop/ and core/
# need it - wlroots' own headers assume xdg-shell-protocol.h is on the
# include path, and CMake only allows one add_custom_command per OUTPUT. See
# BiomeProtocols.cmake for why consumers need XDG_SHELL_PROTOCOL_HEADER_TARGET
# rather than just listing XDG_SHELL_PROTOCOL_HEADER as a source.
biome_generate_protocol_header(XDG_SHELL_PROTOCOL_HEADER XDG_SHELL_PROTOCOL_HEADER_TARGET
    "${WAYLAND_PROTOCOLS_DATADIR}/stable/xdg-shell/xdg-shell.xml")

add_subdirectory(decoration)
add_subdirectory(desktop)
add_subdirectory(core)
