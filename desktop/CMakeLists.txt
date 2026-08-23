find_package(PkgConfig REQUIRED)
# Not a transitive Requires of wlroots-0.18's own .pc, despite wlr/xwayland.h
# pulling in <xcb/xcb_ewmh.h> - needed explicitly for _NET_WM_ICON lookup
# (app_icon.cpp).
pkg_check_modules(XCB_EWMH REQUIRED IMPORTED_TARGET xcb-ewmh)

add_library(biome_desktop STATIC
    app_icon.h
    app_icon.cpp
    toplevel.h
    toplevel.cpp
    workspace.h
    workspace.cpp
    xdg_shell.h
    xdg_shell.cpp
    xwayland_shell.h
    xwayland_shell.cpp
    decoration_bridge.h
    decoration_bridge.cpp
    session_lock.h
    session_lock.cpp
    layer_shell.h
    layer_shell.cpp
)

add_dependencies(biome_desktop biome_wlroots_cxx_shim
    ${XDG_SHELL_PROTOCOL_HEADER_TARGET} ${LAYER_SHELL_PROTOCOL_HEADER_TARGET})

# BIOME_WLROOTS_SHIM_DIR must come before PkgConfig::WLROOTS's own include
# dir so the patched copies of wlr_scene.h/wlr_matrix.h are the ones found.
target_include_directories(biome_desktop PUBLIC
    ${BIOME_WLROOTS_SHIM_DIR}
    ${BIOME_PROTOCOL_DIR}
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(biome_desktop PUBLIC
    biome_decoration
    PkgConfig::WLROOTS
    PkgConfig::WAYLAND_SERVER
    PkgConfig::XCB_EWMH
)
