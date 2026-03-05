cmake_minimum_required(VERSION 3.20)
project(argon2 VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)

set(ARGON2_SRC
    src/argon2.c
    src/core.c
    src/blake2/blake2b.c
    src/thread.c
    src/encoding.c
    src/ref.c
)

add_library(argon2 ${ARGON2_SRC})

target_include_directories(argon2
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(argon2 PRIVATE /W3)
else()
    target_compile_options(argon2 PRIVATE -Wall -Wextra)
    target_link_libraries(argon2 PRIVATE pthread)
endif()

# --- Install ---
include(GNUInstallDirs)

install(TARGETS argon2
    EXPORT argon2-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES include/argon2.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT argon2-targets
    FILE argon2-targets.cmake
    NAMESPACE argon2::
    DESTINATION lib/cmake/argon2
)

include(CMakePackageConfigHelpers)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/argon2-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/argon2-config.cmake"
    INSTALL_DESTINATION lib/cmake/argon2
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/argon2-config.cmake"
    DESTINATION lib/cmake/argon2
)
