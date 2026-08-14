find_package(Qt6 REQUIRED COMPONENTS Gui Widgets)

add_library(biome_decoration STATIC
    layout.h
    layout.cpp
    theme_colors.h
    theme_colors.cpp
    renderer.h
    renderer.cpp
    switcher.h
    switcher.cpp
)

target_include_directories(biome_decoration PUBLIC ${CMAKE_SOURCE_DIR})

target_link_libraries(biome_decoration PUBLIC
    Qt6::Gui
    Qt6::Widgets
)
