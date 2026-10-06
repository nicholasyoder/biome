find_package(Qt6 REQUIRED COMPONENTS Core)

add_executable(output_arrange_test
    output_arrange_test.cpp
    ${CMAKE_SOURCE_DIR}/core/output_arrange.cpp
)
target_include_directories(output_arrange_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(output_arrange_test PRIVATE PkgConfig::WLROOTS)
add_test(NAME output_arrange COMMAND output_arrange_test)

add_executable(output_config_test
    output_config_test.cpp
    ${CMAKE_SOURCE_DIR}/core/config.cpp
    ${CMAKE_SOURCE_DIR}/core/output_config.cpp
)
target_include_directories(output_config_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(output_config_test PRIVATE PkgConfig::WAYLAND_SERVER Qt6::Core)
add_test(NAME output_config COMMAND output_config_test)
