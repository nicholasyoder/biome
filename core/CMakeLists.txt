add_executable(biome
    main.cpp
    server.h
    output.h
    output.cpp
    input.h
    input.cpp
    cursor.h
    cursor.cpp
    wlroots.hpp
)

add_dependencies(biome biome_wlroots_cxx_shim ${XDG_SHELL_PROTOCOL_HEADER_TARGET})

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
    PkgConfig::WLROOTS
    PkgConfig::WAYLAND_SERVER
    PkgConfig::XKBCOMMON
)

install(TARGETS biome RUNTIME DESTINATION bin)
