find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(toml11 CONFIG REQUIRED)
set_target_properties(toml11::toml11 PROPERTIES INTERFACE_COMPILE_OPTIONS "")

add_executable(parties_integration_test
    integration_test.cpp
    ../server/src/server.cpp
    ../server/src/plugin_manager.cpp
    ../server/src/quic_server.cpp
    ../server/src/database.cpp
    ../server/src/config.cpp
    ../client/src/net_client_msquic.cpp
)

target_include_directories(parties_integration_test
    PRIVATE
        ../server/include
        ../client/include
        ../client/src
)

target_link_libraries(parties_integration_test
    PRIVATE
        parties_plugin_sdk
        parties_common
        unofficial::sqlite3::sqlite3
        toml11::toml11
        ${CMAKE_DL_LIBS}
)

if(TARGET parties_plugin_bot_echo)
    add_dependencies(parties_integration_test parties_plugin_bot_echo)
endif()

add_executable(parties_plugin_policy_test
    plugin_policy_test.cpp
    ../server/src/plugin_manager.cpp
)

target_include_directories(parties_plugin_policy_test
    PRIVATE
        ../server/include
)

target_link_libraries(parties_plugin_policy_test
    PRIVATE
        parties_plugin_sdk
        parties_common
        toml11::toml11
        ${CMAKE_DL_LIBS}
)

if(TARGET parties_plugin_bot_echo)
    add_dependencies(parties_plugin_policy_test parties_plugin_bot_echo)
endif()

target_compile_definitions(parties_plugin_policy_test
    PRIVATE
        "PARTIES_TEST_PLUGIN_DIR=\"$<TARGET_FILE_DIR:parties_server>/plugins\""
)

target_compile_definitions(parties_integration_test
    PRIVATE
        "PARTIES_TEST_PLUGIN_DIR=\"$<TARGET_FILE_DIR:parties_server>/plugins\""
)

if(WIN32)
    target_link_libraries(parties_integration_test PRIVATE ncrypt.lib crypt32.lib)
endif()
