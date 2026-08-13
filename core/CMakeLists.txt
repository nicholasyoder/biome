biome_generate_protocol_header(XDG_SHELL_PROTOCOL_HEADER
    "${WAYLAND_PROTOCOLS_DATADIR}/stable/xdg-shell/xdg-shell.xml")

add_executable(biome
    main.cpp
    wlroots.hpp
    ${XDG_SHELL_PROTOCOL_HEADER}
)

add_dependencies(biome biome_wlroots_cxx_shim)

# BIOME_WLROOTS_SHIM_DIR must come before PkgConfig::WLROOTS's own include
# dir so the patched copies of wlr_scene.h/wlr_matrix.h are the ones found.
target_include_directories(biome PRIVATE
    ${BIOME_WLROOTS_SHIM_DIR}
    ${BIOME_PROTOCOL_DIR}
)
target_link_libraries(biome PRIVATE
    PkgConfig::WLROOTS
    PkgConfig::WAYLAND_SERVER
    PkgConfig::XKBCOMMON
)

install(TARGETS biome RUNTIME DESTINATION bin)
