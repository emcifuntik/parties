cmake_minimum_required(VERSION 3.20)
project(parties VERSION 0.1.0 LANGUAGES CXX C)

# ── Version from git tag ─────────────────────────────────────────────────────
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --match "v[0-9]*" --abbrev=0
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_TAG_RESULT
    )
    if(GIT_TAG_RESULT EQUAL 0 AND GIT_TAG MATCHES "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
        set(PARTIES_VERSION_MAJOR "${CMAKE_MATCH_1}")
        set(PARTIES_VERSION_MINOR "${CMAKE_MATCH_2}")
        set(PARTIES_VERSION_PATCH "${CMAKE_MATCH_3}")
        set(PARTIES_VERSION_BUILD "${CMAKE_MATCH_4}")
    elseif(GIT_TAG_RESULT EQUAL 0 AND GIT_TAG MATCHES "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
        set(PARTIES_VERSION_MAJOR "${CMAKE_MATCH_1}")
        set(PARTIES_VERSION_MINOR "${CMAKE_MATCH_2}")
        set(PARTIES_VERSION_PATCH "${CMAKE_MATCH_3}")
        set(PARTIES_VERSION_BUILD "0")
    endif()
endif()
if(NOT DEFINED PARTIES_VERSION_MAJOR)
    set(PARTIES_VERSION_MAJOR 0)
    set(PARTIES_VERSION_MINOR 0)
    set(PARTIES_VERSION_PATCH 0)
    set(PARTIES_VERSION_BUILD 0)
endif()
set(PARTIES_VERSION_STR "${PARTIES_VERSION_MAJOR}.${PARTIES_VERSION_MINOR}.${PARTIES_VERSION_PATCH}.${PARTIES_VERSION_BUILD}")
message(STATUS "Parties version: ${PARTIES_VERSION_STR}")

configure_file(
    "${CMAKE_SOURCE_DIR}/common/include/parties/version.h.in"
    "${CMAKE_BINARY_DIR}/generated/parties/version_gen.h"
    @ONLY
)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
option(BUILD_CLIENT "Build the client application" ON)

if(MSVC OR CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(ENABLE_TRACY "Enable Tracy profiler" ON)
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(BUILD_IN_RETAIL "Build for retail distribution (no debugger, no console)" OFF)
if(BUILD_IN_RETAIL)
    set(ENABLE_TRACY OFF CACHE BOOL "" FORCE)
endif()
option(ENABLE_SENTRY "Enable Sentry crash reporting" OFF)
set(SENTRY_DSN_CLIENT "" CACHE STRING "Sentry DSN for the client (per-platform)")
set(SENTRY_DSN_SERVER "" CACHE STRING "Sentry DSN for the server")
if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address)
    # Disable STL container annotations — vcpkg libs aren't ASAN-instrumented
    add_compile_definitions(_DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
    # Link clang ASAN runtime — lld-link doesn't auto-find it
    get_filename_component(_CLANG_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_CLANG_RT "${_CLANG_BIN}/../lib/clang/20/lib/windows")
    cmake_path(NORMAL_PATH _CLANG_RT)
    link_libraries(
        "${_CLANG_RT}/clang_rt.asan_dynamic-x86_64.lib"
        "${_CLANG_RT}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib"
    )
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    add_compile_options(-Wno-unused-command-line-argument -Wno-c23-extensions)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # Full LTO + PDB in Release (Windows/MSVC-style flags, not for Apple Clang)
    add_compile_options($<$<CONFIG:Release>:-flto=full>)
    add_compile_options($<$<CONFIG:Release>:/Zi>)
    add_link_options($<$<CONFIG:Release>:-flto=full>)
    add_link_options($<$<CONFIG:Release>:/DEBUG:FULL>)
endif()

# Generate dSYM bundles for Xcode archives (required for App Store symbol upload)
if(APPLE)
    set(CMAKE_XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT "dwarf-with-dsym")
endif()

add_subdirectory(common)
if(BUILD_CLIENT)
    add_subdirectory(encdec)
    add_subdirectory(client)
endif()
if(NOT APPLE)
    add_subdirectory(server)
endif()
if(NOT APPLE)
    add_subdirectory(tests)
endif()

option(BUILD_DESIGNER "Build the RmlUI Designer tool" OFF)
if(BUILD_DESIGNER AND NOT APPLE)
    add_subdirectory(designer)
endif()
