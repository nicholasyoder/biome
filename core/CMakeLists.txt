find_package(Qt6 REQUIRED COMPONENTS Core DBus)

find_package(PkgConfig REQUIRED)
# Needed by qt_glib_bridge.cpp to dispatch g_main_context_default() directly
# (see docs/qt-event-loop-integration-research.md) - already present on the
# target system as a transitive dependency of GTK/other desktop packages, but
# wasn't declared as a Biome dependency anywhere until now.
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)

add_executable(biome
    main.cpp
    server.h
    layers.h
    layers.cpp
    output.h
    output.cpp
    output_config.h
    output_config.cpp
    fade_config.h
    fade_config.cpp
    input.h
    input.cpp
    keybindings.h
    keybindings.cpp
    cursor.h
    cursor.cpp
    idle_blank.h # STOPGAP(idle-blank)
    idle_blank.cpp # STOPGAP(idle-blank)
    qt_glib_bridge.h
    qt_glib_bridge.cpp
    wlroots.hpp
)

add_dependencies(biome biome_wlroots_cxx_shim
    ${XDG_SHELL_PROTOCOL_HEADER_TARGET} ${LAYER_SHELL_PROTOCOL_HEADER_TARGET}
    ${CURSOR_SHAPE_PROTOCOL_HEADER_TARGET})

# BIOME_WLROOTS_SHIM_DIR must come before PkgConfig::WLROOTS's own include
# dir so the patched copies of wlr_scene.h/wlr_matrix.h are the ones found.
target_include_directories(biome PRIVATE
    ${BIOME_WLROOTS_SHIM_DIR}
    ${BIOME_PROTOCOL_DIR}
    ${CMAKE_SOURCE_DIR}
)
target_link_libraries(biome PRIVATE
    biome_desktop
    biome_decoration
    biome_ipc
    PkgConfig::WLROOTS
    PkgConfig::WAYLAND_SERVER
    PkgConfig::XKBCOMMON
    PkgConfig::GLIB
    Qt6::Core
    Qt6::DBus
)

install(TARGETS biome RUNTIME DESTINATION bin)
