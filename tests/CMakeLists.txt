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

add_executable(trigger_test
    trigger_test.cpp
    ${CMAKE_SOURCE_DIR}/core/trigger.cpp
)
target_include_directories(trigger_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(trigger_test PRIVATE PkgConfig::WLROOTS PkgConfig::XKBCOMMON Qt6::Core)
add_test(NAME trigger COMMAND trigger_test)

# Headless integration scenarios (tests/integration/run.sh).
find_program(DBUS_RUN_SESSION dbus-run-session)
option(BIOME_INTEGRATION_TESTS "Headless biome integration tests (needs dbus-run-session)" ON)
if(BIOME_INTEGRATION_TESTS AND DBUS_RUN_SESSION)
    pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
    set(itest_protocols
        "${WAYLAND_PROTOCOLS_DATADIR}/stable/xdg-shell/xdg-shell.xml"
        "${WAYLAND_PROTOCOLS_DATADIR}/staging/ext-session-lock/ext-session-lock-v1.xml"
        "${WAYLAND_PROTOCOLS_DATADIR}/staging/ext-workspace/ext-workspace-v1.xml"
        "${WAYLAND_PROTOCOLS_DATADIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml"
        "${CMAKE_SOURCE_DIR}/protocol/wlr-layer-shell-unstable-v1.xml"
        "${CMAKE_SOURCE_DIR}/protocol/wlr-output-power-management-unstable-v1.xml")
    set(itest_dir "${CMAKE_CURRENT_BINARY_DIR}/integration-protocol")
    file(MAKE_DIRECTORY ${itest_dir})
    set(itest_sources)
    foreach(xml ${itest_protocols})
        get_filename_component(name ${xml} NAME_WE)
        add_custom_command(
            OUTPUT ${itest_dir}/${name}-client-protocol.h ${itest_dir}/${name}-protocol.c
            COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header ${xml} ${itest_dir}/${name}-client-protocol.h
            COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code ${xml} ${itest_dir}/${name}-protocol.c
            DEPENDS ${xml}
            VERBATIM)
        list(APPEND itest_sources ${itest_dir}/${name}-client-protocol.h ${itest_dir}/${name}-protocol.c)
    endforeach()

    add_executable(biome_test_client integration/client.c ${itest_sources})
    target_include_directories(biome_test_client PRIVATE ${itest_dir})
    target_link_libraries(biome_test_client PRIVATE PkgConfig::WAYLAND_CLIENT)

    add_library(biome_test_shim MODULE integration/shim.c)
    target_link_libraries(biome_test_shim PRIVATE PkgConfig::WLROOTS ${CMAKE_DL_LIBS})

    foreach(scenario startup-exit sigterm remap lock-outputs-off lock-unplug workspaces)
        add_test(NAME integration.${scenario}
            COMMAND ${DBUS_RUN_SESSION} -- ${CMAKE_CURRENT_SOURCE_DIR}/integration/run.sh
                $<TARGET_FILE:biome> $<TARGET_FILE:biome_test_client> $<TARGET_FILE:biome_test_shim> ${scenario})
        set_tests_properties(integration.${scenario} PROPERTIES TIMEOUT 60)
    endforeach()
endif()
