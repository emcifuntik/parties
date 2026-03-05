vcpkg_from_github(
    OUT_SOURCE_PATH QUIC_SOURCE_PATH
    REPO microsoft/msquic
    REF "v${VERSION}"
    SHA512 a16ec3de6a0a68256a4688586d6205ef9ddc4ea22a3ea2b208d4b953faf4369a0dd573b9e27bb933344f37dbfb421fa6f819a70e87a7d02a0b4971adde60dfdb
    HEAD_REF main
)

# XDP headers are needed for the Windows build even if XDP isn't used at runtime
vcpkg_from_github(
    OUT_SOURCE_PATH XDP_WINDOWS
    REPO microsoft/xdp-for-windows
    REF v1.1.3
    SHA512 8bf38182bf3c2da490e6e4df9420bacc3839e19d7cea6ca1c1420d1fd349e87a1f80992b52524eaab70a84ff1ac4e1681974211871117847fba92334350dcf13
    HEAD_REF main
)

# Place XDP headers where MsQuic expects them
if(NOT EXISTS "${QUIC_SOURCE_PATH}/submodules/xdp-for-windows/published/external")
    file(REMOVE_RECURSE "${QUIC_SOURCE_PATH}/submodules/xdp-for-windows")
    file(COPY "${XDP_WINDOWS}/published/external" DESTINATION "${QUIC_SOURCE_PATH}/submodules/xdp-for-windows/published")
endif()

string(COMPARE EQUAL "${VCPKG_CRT_LINKAGE}" "static" STATIC_CRT)

# MsQuic unconditionally appends /GL (LTCG) to release flags.
# lld-link (clang-cl) cannot consume LTCG bitcode objects. Strip /GL and /LTCG.
vcpkg_replace_string("${QUIC_SOURCE_PATH}/CMakeLists.txt"
    [[set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /GL /Zi")]]
    [[set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /Zi")]]
)
vcpkg_replace_string("${QUIC_SOURCE_PATH}/CMakeLists.txt"
    [[set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /GL /Zi")]]
    [[set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")]]
)
vcpkg_replace_string("${QUIC_SOURCE_PATH}/CMakeLists.txt"
    [[${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /LTCG /IGNORE:4075]]
    [[${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /IGNORE:4075]]
)
vcpkg_replace_string("${QUIC_SOURCE_PATH}/CMakeLists.txt"
    [[${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG /IGNORE:4075]]
    [[${CMAKE_EXE_LINKER_FLAGS_RELEASE} /IGNORE:4075]]
)

# clang-cl: disable /WX — upstream MsQuic code has warnings that clang diagnoses
# but MSVC doesn't (unused-value, microsoft-anon-tag, etc.)
vcpkg_replace_string("${QUIC_SOURCE_PATH}/CMakeLists.txt"
[[    set(QUIC_WARNING_FLAGS /WX /W4 /sdl /wd4206 CACHE INTERNAL "")]]
[[    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        # clang-cl: drop /WX /sdl, downgrade Clang-15+ default-error diagnostics
        set(QUIC_WARNING_FLAGS /W4 /wd4206
            -Wno-error=incompatible-pointer-types
            -Wno-error=int-conversion
            -Wno-error=unused-value
            -Wno-error=microsoft-anon-tag
            CACHE INTERNAL "")
    else()
        set(QUIC_WARNING_FLAGS /WX /W4 /sdl /wd4206 CACHE INTERNAL "")
    endif()]]
)

# clang-cl: /Qspectre and /guard:cf are MSVC-only.  check_c_compiler_flag lets
# them through (clang only warns), but MsQuic's /WX promotes the warning to error.
vcpkg_replace_string("${QUIC_SOURCE_PATH}/CMakeLists.txt"
[[    if(NOT QUIC_ENABLE_SANITIZERS)
        check_c_compiler_flag(/Qspectre HAS_SPECTRE)
    endif()]]
[[    if(NOT QUIC_ENABLE_SANITIZERS AND NOT (CMAKE_C_COMPILER_ID STREQUAL "Clang"))
        check_c_compiler_flag(/Qspectre HAS_SPECTRE)
    endif()]]
)
vcpkg_replace_string("${QUIC_SOURCE_PATH}/CMakeLists.txt"
[[    check_c_compiler_flag(/guard:cf HAS_GUARDCF)]]
[[    if(NOT (CMAKE_C_COMPILER_ID STREQUAL "Clang"))
        check_c_compiler_flag(/guard:cf HAS_GUARDCF)
    endif()]]
)

# --- wolfSSL TLS backend ---
#
# Instead of building QuicTLS (OpenSSL fork), we use wolfSSL with OPENSSLALL
# as MsQuic's TLS provider. wolfSSL provides OpenSSL-compatible QUIC API
# (SSL_CTX_set_quic_method, SSL_provide_quic_data, etc.) via its compat layer.
#
# Strategy:
#   - Use tls_quictls.c compiled against wolfSSL's OpenSSL-compat headers
#   - Use crypt_bcrypt.c for QUIC crypto (Windows CNG, no OpenSSL 3.x APIs)
#   - Use cert_capi.c + selfsign_capi.c for cert operations (Windows CAPI)
#   - Install wolfssl_compat.h to stub missing OpenSSL 3.x APIs

# Install the wolfSSL compatibility header into MsQuic's source
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/wolfssl_compat.h"
     DESTINATION "${QUIC_SOURCE_PATH}/src/platform")

# -- Patch top-level CMakeLists.txt: add wolfssl TLS backend option --

# Add wolfssl option alongside quictls (after the quictls FetchContent block)
vcpkg_replace_string("${QUIC_SOURCE_PATH}/CMakeLists.txt"
[[if(QUIC_TLS_LIB STREQUAL "quictls")
    add_library(OpenSSL INTERFACE)

    include(FetchContent)

    FetchContent_Declare(
        OpenSSLQuic
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/submodules
        CMAKE_ARGS "-DQUIC_USE_SYSTEM_LIBCRYPTO=${QUIC_USE_SYSTEM_LIBCRYPTO}"
    )
    FetchContent_MakeAvailable(OpenSSLQuic)

    target_link_libraries(OpenSSL
        INTERFACE
        OpenSSLQuic::OpenSSLQuic
    )
endif()]]
[[if(QUIC_TLS_LIB STREQUAL "wolfssl")
    add_library(OpenSSL INTERFACE)
    find_package(wolfssl CONFIG REQUIRED)
    get_target_property(_wolfssl_inc wolfssl::wolfssl INTERFACE_INCLUDE_DIRECTORIES)
    # wolfSSL OpenSSL-compat headers live under wolfssl/openssl/ —
    # add wolfssl/ subdir so #include "openssl/ssl.h" resolves
    target_include_directories(OpenSSL INTERFACE ${_wolfssl_inc}/wolfssl)
    target_link_libraries(OpenSSL INTERFACE wolfssl::wolfssl)
elseif(QUIC_TLS_LIB STREQUAL "quictls")
    add_library(OpenSSL INTERFACE)

    include(FetchContent)

    FetchContent_Declare(
        OpenSSLQuic
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/submodules
        CMAKE_ARGS "-DQUIC_USE_SYSTEM_LIBCRYPTO=${QUIC_USE_SYSTEM_LIBCRYPTO}"
    )
    FetchContent_MakeAvailable(OpenSSLQuic)

    target_link_libraries(OpenSSL
        INTERFACE
        OpenSSLQuic::OpenSSLQuic
    )
endif()]]
)

# Add wolfssl test config defines (skip PFX/0-RTT tests like schannel)
vcpkg_replace_string("${QUIC_SOURCE_PATH}/CMakeLists.txt"
[[if(QUIC_TLS_LIB STREQUAL "quictls")
    message(STATUS "Enabling quictls/openssl configuration tests")]]
[[if(QUIC_TLS_LIB STREQUAL "wolfssl")
    message(STATUS "Enabling wolfSSL configuration tests")
    list(APPEND QUIC_COMMON_DEFINES QUIC_TEST_OPENSSL_FLAGS=1)
elseif(QUIC_TLS_LIB STREQUAL "quictls")
    message(STATUS "Enabling quictls/openssl configuration tests")]]
)

# -- Patch platform CMakeLists.txt: add wolfssl source/link rules --

vcpkg_replace_string("${QUIC_SOURCE_PATH}/src/platform/CMakeLists.txt"
[[if (QUIC_TLS_LIB STREQUAL "schannel")
    message(STATUS "Configuring for Schannel")
    set(SOURCES ${SOURCES} cert_capi.c crypt_bcrypt.c selfsign_capi.c tls_schannel.c)
elseif(QUIC_TLS_LIB STREQUAL "quictls")]]
[[if (QUIC_TLS_LIB STREQUAL "wolfssl")
    message(STATUS "Configuring for wolfSSL")
    set(SOURCES ${SOURCES} tls_quictls.c crypt_bcrypt.c certificates_capi.c cert_capi.c selfsign_capi.c)
    # wolfSSL's options.h isn't needed — all defines come from CMake target.
    # Without this, settings.h emits #warning which MSVC C compiler doesn't support.
    add_compile_definitions(WOLFSSL_NO_OPTIONS_H WOLFSSL_EARLY_DATA)
elseif(QUIC_TLS_LIB STREQUAL "schannel")
    message(STATUS "Configuring for Schannel")
    set(SOURCES ${SOURCES} cert_capi.c crypt_bcrypt.c selfsign_capi.c tls_schannel.c)
elseif(QUIC_TLS_LIB STREQUAL "quictls")]]
)

# Add wolfssl link rule
vcpkg_replace_string("${QUIC_SOURCE_PATH}/src/platform/CMakeLists.txt"
[[if(QUIC_TLS_LIB STREQUAL "quictls")
    target_link_libraries(msquic_platform PUBLIC OpenSSL)]]
[[if(QUIC_TLS_LIB STREQUAL "wolfssl")
    target_link_libraries(msquic_platform PUBLIC OpenSSL secur32 onecore)
elseif(QUIC_TLS_LIB STREQUAL "quictls")
    target_link_libraries(msquic_platform PUBLIC OpenSSL)]]
)

# Disable /analyze for clang-cl (MSVC is TRUE for clang-cl but /analyze is MSVC-only)
vcpkg_replace_string("${QUIC_SOURCE_PATH}/src/platform/CMakeLists.txt"
[[if (MSVC AND (QUIC_TLS_LIB STREQUAL "quictls" OR QUIC_TLS_LIB STREQUAL "schannel") AND NOT QUIC_ENABLE_SANITIZERS)]]
[[if (MSVC AND NOT (CMAKE_C_COMPILER_ID STREQUAL "Clang") AND (QUIC_TLS_LIB STREQUAL "wolfssl" OR QUIC_TLS_LIB STREQUAL "quictls" OR QUIC_TLS_LIB STREQUAL "schannel") AND NOT QUIC_ENABLE_SANITIZERS)]]
)
vcpkg_replace_string("${QUIC_SOURCE_PATH}/src/core/CMakeLists.txt"
[[if (MSVC AND NOT QUIC_ENABLE_SANITIZERS)]]
[[if (MSVC AND NOT (CMAKE_C_COMPILER_ID STREQUAL "Clang") AND NOT QUIC_ENABLE_SANITIZERS)]]
)

# -- Patch tls_quictls.c: add wolfSSL compatibility --

# Include wolfssl_compat.h at the top (after platform_internal.h include)
vcpkg_replace_string("${QUIC_SOURCE_PATH}/src/platform/tls_quictls.c"
[=[#include "platform_internal.h"

#include "openssl/opensslv.h"

#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable:4100) // Unreferenced parameter errcode in inline function
#endif
#include "openssl/bio.h"
#include "openssl/core_names.h"
#include "openssl/err.h"
#include "openssl/kdf.h"
#include "openssl/pem.h"
#include "openssl/pkcs12.h"
#include "openssl/pkcs7.h"
#include "openssl/rsa.h"
#include "openssl/ssl.h"
#include "openssl/x509.h"
#ifdef _WIN32
#pragma warning(pop)
#endif]=]
[=[#include "platform_internal.h"

#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable:4100) // Unreferenced parameter errcode in inline function
#endif
#include "openssl/bio.h"
#include "openssl/err.h"
#include "openssl/pem.h"
#include "openssl/pkcs12.h"
#include "openssl/rsa.h"
#include "openssl/ssl.h"
#include "openssl/x509.h"
#ifdef _WIN32
#pragma warning(pop)
#endif

/* wolfSSL compat shim — stubs for OpenSSL 3.x APIs not in wolfSSL */
#include "wolfssl_compat.h"]=]
)

# Change extern AES handle to definition (normally in crypt_openssl.c which we don't use;
# the ticket key callback referencing it is never registered with wolfSSL)
vcpkg_replace_string("${QUIC_SOURCE_PATH}/src/platform/tls_quictls.c"
    [[extern EVP_CIPHER *CXPLAT_AES_256_CBC_ALG_HANDLE;]]
    [[EVP_CIPHER *CXPLAT_AES_256_CBC_ALG_HANDLE = NULL;]]
)

vcpkg_cmake_configure(
    SOURCE_PATH "${QUIC_SOURCE_PATH}"
    OPTIONS
        -DQUIC_TLS_LIB=wolfssl
        -DQUIC_BUILD_SHARED=OFF
        -DQUIC_SOURCE_LINK=OFF
        -DQUIC_BUILD_PERF=OFF
        -DQUIC_BUILD_TEST=OFF
        -DQUIC_BUILD_TOOLS=OFF
        -DQUIC_ENABLE_LOGGING=OFF
        "-DQUIC_STATIC_LINK_CRT=${STATIC_CRT}"
        "-DQUIC_STATIC_LINK_PARTIAL_CRT=${STATIC_CRT}"
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

# MsQuic's static build doesn't generate proper CMake export targets.
# Replace the broken auto-generated config with our custom one.
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/share/msquic"
)
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/msquic-config.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/msquic")

vcpkg_install_copyright(FILE_LIST "${QUIC_SOURCE_PATH}/LICENSE")
