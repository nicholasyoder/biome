find_package(Qt6 REQUIRED COMPONENTS Core DBus)

set(CMAKE_AUTOMOC ON)

add_library(biome_ipc STATIC
    global_shortcuts_portal.h
    global_shortcuts_portal.cpp
)

add_dependencies(biome_ipc biome_wlroots_cxx_shim
    ${XDG_SHELL_PROTOCOL_HEADER_TARGET} ${LAYER_SHELL_PROTOCOL_HEADER_TARGET})

# BIOME_WLROOTS_SHIM_DIR must come before PkgConfig::WLROOTS's own include
# dir so the patched copies of wlr_scene.h/wlr_matrix.h are the ones found.
target_include_directories(biome_ipc PUBLIC
    ${BIOME_WLROOTS_SHIM_DIR}
    ${BIOME_PROTOCOL_DIR}
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(biome_ipc PUBLIC
    Qt6::Core
    Qt6::DBus
    PkgConfig::WLROOTS
)
