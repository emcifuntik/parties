find_package(Opus CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(msquic CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_library(parties_common STATIC
    src/protocol.cpp
    src/serialization.cpp
    src/crypto.cpp
    src/codec.cpp
    src/net_common.cpp
    src/permissions.cpp
    src/log.cpp
    src/crash_reporter.cpp
)
set_target_properties(parties_common PROPERTIES XCODE_ATTRIBUTE_SKIP_INSTALL "YES")

target_sources(parties_common PRIVATE src/quic_common.cpp)

target_include_directories(parties_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(ENABLE_TRACY)
    find_package(Tracy CONFIG REQUIRED)
endif()

# Sentry (optional — enabled when ENABLE_SENTRY is ON)
# iOS uses sentry-cocoa (xcframework linked in client/CMakeLists.txt), not sentry-native.
if(ENABLE_SENTRY AND NOT APPLE)
    find_package(sentry CONFIG REQUIRED)
    set(_SENTRY_LIB sentry::sentry)
else()
    set(_SENTRY_LIB "")
endif()

target_link_libraries(parties_common
    PUBLIC
        Opus::opus
        OpenSSL::Crypto
        msquic::msquic
        spdlog::spdlog
        ${_SENTRY_LIB}
        $<$<BOOL:${ENABLE_TRACY}>:Tracy::TracyClient>
)

target_compile_definitions(parties_common PUBLIC
    SPDLOG_USE_STD_FORMAT
    $<$<BOOL:${BUILD_IN_RETAIL}>:PARTIES_RETAIL=1>
    $<$<CONFIG:Debug>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG>
    $<$<NOT:$<CONFIG:Debug>>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO>
)

if(ENABLE_SENTRY AND NOT APPLE)
    target_compile_definitions(parties_common PUBLIC SENTRY_ENABLED)
endif()

if(WIN32)
    target_link_libraries(parties_common PUBLIC ws2_32 winmm)
endif()
