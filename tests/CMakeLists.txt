find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(toml11 CONFIG REQUIRED)
set_target_properties(toml11::toml11 PROPERTIES INTERFACE_COMPILE_OPTIONS "")

add_executable(parties_integration_test
    integration_test.cpp
    ../server/src/server.cpp
    ../server/src/quic_server.cpp
    ../server/src/database.cpp
    ../server/src/config.cpp
    ../client/src/net_client.cpp
)

target_include_directories(parties_integration_test
    PRIVATE
        ../server/include
        ../client/include
)

target_link_libraries(parties_integration_test
    PRIVATE
        parties_common
        unofficial::sqlite3::sqlite3
        toml11::toml11
)

if(WIN32)
    target_link_libraries(parties_integration_test PRIVATE ncrypt.lib crypt32.lib)
endif()
