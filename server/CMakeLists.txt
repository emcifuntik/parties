find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(toml11 CONFIG REQUIRED)
# toml11 exports MSVC-specific /utf-8 /Zc:preprocessor flags; strip them for clang-cl
set_target_properties(toml11::toml11 PROPERTIES INTERFACE_COMPILE_OPTIONS "")

add_executable(parties_server
    src/main.cpp
    src/config.cpp
    src/quic_server.cpp
    src/database.cpp
    src/server.cpp
)

target_include_directories(parties_server
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(parties_server
    PRIVATE
        parties_common
        unofficial::sqlite3::sqlite3
        toml11::toml11
        ncrypt.lib
)
