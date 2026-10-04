add_executable(output_arrange_test
    output_arrange_test.cpp
    ${CMAKE_SOURCE_DIR}/core/output_arrange.cpp
)
target_include_directories(output_arrange_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(output_arrange_test PRIVATE PkgConfig::WLROOTS)
add_test(NAME output_arrange COMMAND output_arrange_test)
