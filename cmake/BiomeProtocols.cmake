# SPDX-License-Identifier: LGPL-3.0-or-later
#
# wlroots does not generate Wayland protocol glue for you; each compositor
# has to run wayland-scanner over the protocol XML itself. This module wires
# that up via CMake custom commands instead of the Makefile recipe upstream's
# tinywl example uses.

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_check_modules(WAYLAND_SCANNER REQUIRED wayland-scanner)

pkg_get_variable(WAYLAND_PROTOCOLS_DATADIR wayland-protocols pkgdatadir)
pkg_get_variable(WAYLAND_SCANNER_EXECUTABLE wayland-scanner wayland_scanner)

set(BIOME_PROTOCOL_DIR "${CMAKE_BINARY_DIR}/protocol")
file(MAKE_DIRECTORY ${BIOME_PROTOCOL_DIR})

# Generates a server-side protocol header from a protocol XML file, stores
# its path in ${out_var} and a target that depends on it in ${target_out_var}
# (add_dependencies(<consumer> ${target_out_var}) from any directory to make
# sure it's generated first - a bare add_custom_command's OUTPUT rule isn't
# reliably picked up by a target in a *different* directory than the one
# that declared it, across CMake generators/versions, so callers here need
# the wrapping custom target rather than listing the header itself as a
# source). Compositor code includes "<name>-protocol.h" for the protocols it
# needs (currently just xdg-shell); wlroots links the protocol implementation
# itself, this header only provides the generated interface/enum definitions
# wlroots' own headers assume are available.
function(biome_generate_protocol_header out_var target_out_var xml_path)
    get_filename_component(name ${xml_path} NAME_WE)
    set(output "${BIOME_PROTOCOL_DIR}/${name}-protocol.h")
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${WAYLAND_SCANNER_EXECUTABLE} server-header ${xml_path} ${output}
        DEPENDS ${xml_path}
        COMMENT "Generating ${name}-protocol.h"
        VERBATIM
    )
    set(target_name "${name}_protocol_header")
    add_custom_target(${target_name} DEPENDS ${output})
    set(${out_var} ${output} PARENT_SCOPE)
    set(${target_out_var} ${target_name} PARENT_SCOPE)
endfunction()

# Generates both the server-side header AND the private-code source (the
# wl_interface/wl_message marshalling tables) from a protocol XML file.
# Needed only for protocols with no wlroots-provided C type wrapping them -
# biome_generate_protocol_header() above is enough when wlroots implements
# the protocol itself (xdg-shell, layer-shell) and Biome only needs the
# generated interface/enum symbols its headers reference. ext-workspace-v1
# (desktop/ext_workspace.cpp) has no such wlroots type, so Biome's own code
# has to call wl_global_create()/wl_resource_create() directly, which needs
# the private-code translation unit linked in as well as the header.
# Returns the header path, the source path, and a target depending on both.
function(biome_generate_protocol_source header_out_var source_out_var target_out_var xml_path)
    get_filename_component(name ${xml_path} NAME_WE)
    set(header "${BIOME_PROTOCOL_DIR}/${name}-protocol.h")
    set(source "${BIOME_PROTOCOL_DIR}/${name}-protocol.c")
    add_custom_command(
        OUTPUT ${header}
        COMMAND ${WAYLAND_SCANNER_EXECUTABLE} server-header ${xml_path} ${header}
        DEPENDS ${xml_path}
        COMMENT "Generating ${name}-protocol.h"
        VERBATIM
    )
    add_custom_command(
        OUTPUT ${source}
        COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code ${xml_path} ${source}
        DEPENDS ${xml_path}
        COMMENT "Generating ${name}-protocol.c"
        VERBATIM
    )
    set(target_name "${name}_protocol_source")
    add_custom_target(${target_name} DEPENDS ${header} ${source})
    set(${header_out_var} ${header} PARENT_SCOPE)
    set(${source_out_var} ${source} PARENT_SCOPE)
    set(${target_out_var} ${target_name} PARENT_SCOPE)
endfunction()
