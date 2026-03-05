find_package(Opus CONFIG REQUIRED)
find_package(wolfssl CONFIG REQUIRED)
find_package(argon2 CONFIG REQUIRED)
find_package(msquic CONFIG REQUIRED)

add_library(parties_common STATIC
    src/protocol.cpp
    src/serialization.cpp
    src/crypto.cpp
    src/codec.cpp
    src/net_common.cpp
    src/quic_common.cpp
    src/permissions.cpp
)

target_include_directories(parties_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# wolfssl overlay port passes these via CFLAGS but they're not in options.h.
# WOLFSSL_CUSTOM_OID affects the Cert struct layout — without it our app sees
# a smaller struct than the library, causing heap overflow in wc_InitCert and
# wrong field offsets.
target_compile_definitions(parties_common
    PUBLIC
        WOLFSSL_CUSTOM_OID
        HAVE_OID_ENCODING
        WOLFSSL_ASN_TEMPLATE
        WOLFSSL_PEM_TO_DER
        WOLFSSL_KEY_GEN
        WOLFSSL_CERT_GEN
)

target_link_libraries(parties_common
    PUBLIC
        Opus::opus
        wolfssl::wolfssl
        argon2::argon2
        msquic::msquic
)

if(WIN32)
    target_link_libraries(parties_common PUBLIC ws2_32 winmm)
endif()
