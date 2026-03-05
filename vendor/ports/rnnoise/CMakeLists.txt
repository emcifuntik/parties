cmake_minimum_required(VERSION 3.20)
project(rnnoise VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)

set(RNNOISE_SRC
    src/denoise.c
    src/rnn.c
    src/pitch.c
    src/kiss_fft.c
    src/celt_lpc.c
    src/nnet.c
    src/nnet_default.c
    src/parse_lpcnet_weights.c
    src/rnnoise_data.c
    src/rnnoise_tables.c
)

add_library(rnnoise ${RNNOISE_SRC})

target_include_directories(rnnoise
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(rnnoise
    PRIVATE
        RNNOISE_BUILD
        DISABLE_DEBUG_FLOAT
        $<$<BOOL:${BUILD_SHARED_LIBS}>:DLL_EXPORT>
)

if(MSVC)
    target_compile_options(rnnoise PRIVATE /W3)
    target_compile_definitions(rnnoise PRIVATE _USE_MATH_DEFINES)
else()
    target_compile_options(rnnoise PRIVATE -Wall -Wextra -Wno-sign-compare)
    target_link_libraries(rnnoise PRIVATE m)
endif()

# --- Install ---
include(GNUInstallDirs)

install(TARGETS rnnoise
    EXPORT rnnoise-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES include/rnnoise.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT rnnoise-targets
    FILE rnnoise-targets.cmake
    NAMESPACE rnnoise::
    DESTINATION lib/cmake/rnnoise
)

include(CMakePackageConfigHelpers)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rnnoise-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/rnnoise-config.cmake"
    INSTALL_DESTINATION lib/cmake/rnnoise
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/rnnoise-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/rnnoise-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/rnnoise-config-version.cmake"
    DESTINATION lib/cmake/rnnoise
)
