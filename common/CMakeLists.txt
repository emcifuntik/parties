find_package(Opus CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

find_package(msquic CONFIG REQUIRED)

add_library(parties_common STATIC
    src/protocol.cpp
    src/serialization.cpp
    src/crypto.cpp
    src/codec.cpp
    src/net_common.cpp
    src/permissions.cpp
)

target_sources(parties_common PRIVATE src/quic_common.cpp)

target_include_directories(parties_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(ENABLE_TRACY)
    find_package(Tracy CONFIG REQUIRED)
endif()

target_link_libraries(parties_common
    PUBLIC
        Opus::opus
        OpenSSL::Crypto
        msquic::msquic
        $<$<BOOL:${ENABLE_TRACY}>:Tracy::TracyClient>
)

if(WIN32)
    target_link_libraries(parties_common PUBLIC ws2_32 winmm)
endif()
