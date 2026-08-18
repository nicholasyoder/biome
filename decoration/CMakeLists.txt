find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

set(CMAKE_AUTOMOC ON)

qt_add_resources(BIOME_DECORATION_QRC_SOURCES theme/theme.qrc)

add_library(biome_decoration STATIC
    layout.h
    theme.h
    theme.cpp
    frame_widget.h
    frame_widget.cpp
    renderer.h
    renderer.cpp
    switcher.h
    switcher.cpp
    ${BIOME_DECORATION_QRC_SOURCES}
)

target_include_directories(biome_decoration PUBLIC ${CMAKE_SOURCE_DIR})

target_link_libraries(biome_decoration PUBLIC
    Qt6::Gui
    Qt6::Widgets
)
