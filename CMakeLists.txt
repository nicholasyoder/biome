cmake_minimum_required(VERSION 3.16)
# C is only needed for wayland-scanner's generated ext-workspace-v1-protocol.c
# (biome_ext_workspace_protocol below, desktop/ext_workspace.cpp) - without
# it enabled here, CMake silently emits no compile rule for that source at
# all rather than erroring, so the OBJECT library looks like it built but
# is actually empty.
project(biome VERSION 0.1.0 LANGUAGES CXX C)

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

# wlr-layer-shell-unstable-v1 isn't part of the wayland-protocols package
# (unlike xdg-shell above) - it originates in wlroots' own protocol/ dir and
# isn't installed by the wlroots-0.18-dev package either, so a copy is
# vendored here instead of assuming a system path for it. wlr/types/
# wlr_layer_shell_v1.h itself #includes the generated
# "wlr-layer-shell-unstable-v1-protocol.h" by name, so this only needs
# generating and putting on the include path (BIOME_PROTOCOL_DIR, below) -
# nothing in Biome's own code includes it directly.
biome_generate_protocol_header(LAYER_SHELL_PROTOCOL_HEADER LAYER_SHELL_PROTOCOL_HEADER_TARGET
    "${CMAKE_SOURCE_DIR}/protocol/wlr-layer-shell-unstable-v1.xml")

# ext-workspace-v1 (desktop/ext_workspace.cpp) has no wlroots-provided C
# type - unlike xdg-shell/layer-shell above, wlroots doesn't implement this
# protocol at all, so Biome needs the generated private-code source (the
# wl_interface/wl_message tables), not just the header. It IS part of the
# wayland-protocols package (staging/), so no vendoring needed, unlike
# layer-shell above.
biome_generate_protocol_source(EXT_WORKSPACE_PROTOCOL_HEADER EXT_WORKSPACE_PROTOCOL_SOURCE
    EXT_WORKSPACE_PROTOCOL_SOURCE_TARGET
    "${WAYLAND_PROTOCOLS_DATADIR}/staging/ext-workspace/ext-workspace-v1.xml")

# Wrapped in its own OBJECT library (rather than listing the generated .c
# directly in biome_desktop's add_library, which lives in a different
# directory scope) since a target's name is visible project-wide in CMake
# while a bare generated-file path from another directory's custom command
# is not reliably so pre-CMP0118 - see BiomeProtocols.cmake's header-target
# comment for the same underlying caveat.
add_library(biome_ext_workspace_protocol OBJECT ${EXT_WORKSPACE_PROTOCOL_SOURCE})
add_dependencies(biome_ext_workspace_protocol ${EXT_WORKSPACE_PROTOCOL_SOURCE_TARGET})
target_link_libraries(biome_ext_workspace_protocol PUBLIC PkgConfig::WAYLAND_SERVER)
target_include_directories(biome_ext_workspace_protocol PUBLIC ${BIOME_PROTOCOL_DIR})

add_subdirectory(decoration)
add_subdirectory(desktop)
add_subdirectory(ipc)
add_subdirectory(core)
