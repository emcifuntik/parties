vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO wolfssl/wolfssl
    REF "v${VERSION}-stable"
    SHA512 6f191c218b270bd4dc90d6f07a80416e6bc8d049f3f49ea84c38a2af40ae9588a4fe306860fbb8696c5af15c4ca359818e3955069389d33269eee0101c270439
    HEAD_REF master
    PATCHES
    )

if ("asio" IN_LIST FEATURES)
    set(ENABLE_ASIO yes)
else()
    set(ENABLE_ASIO no)
endif()

if ("dtls" IN_LIST FEATURES)
    set(ENABLE_DTLS yes)
else()
    set(ENABLE_DTLS no)
endif()

if ("quic" IN_LIST FEATURES)
    set(ENABLE_QUIC yes)
else()
    set(ENABLE_QUIC no)
endif()

if ("curve25519" IN_LIST FEATURES)
    set(ENABLE_CURVE25519 yes)
else()
    set(ENABLE_CURVE25519 no)
endif()

if ("ed25519" IN_LIST FEATURES)
    set(ENABLE_ED25519 yes)
else()
    set(ENABLE_ED25519 no)
endif()

vcpkg_cmake_get_vars(cmake_vars_file)
include("${cmake_vars_file}")

foreach(config RELEASE DEBUG)
  # Suppress clang-cl errors for upstream wolfSSL code (InetPtonW narrow/wide mismatch)
  string(APPEND VCPKG_COMBINED_C_FLAGS_${config} " -Wno-incompatible-pointer-types")
  string(APPEND VCPKG_COMBINED_C_FLAGS_${config} " -DWOLFSSL_CUSTOM_OID -DHAVE_OID_ENCODING -DWOLFSSL_ASN_TEMPLATE")
  # Force PEM-to-DER support and ensure NO_CERTS is not set
  string(APPEND VCPKG_COMBINED_C_FLAGS_${config} " -DWOLFSSL_PEM_TO_DER")
  # Enable 0-RTT early data (wolfSSL CMake doesn't have an option for this yet)
  string(APPEND VCPKG_COMBINED_C_FLAGS_${config} " -DWOLFSSL_EARLY_DATA")
  # Ed25519 defines for ABI consistency (must match what wolfSSL compiles with)
  if ("ed25519" IN_LIST FEATURES)
      string(APPEND VCPKG_COMBINED_C_FLAGS_${config} " -DHAVE_ED25519 -DHAVE_ED25519_SIGN -DHAVE_ED25519_VERIFY -DHAVE_ED25519_KEY_IMPORT -DHAVE_ED25519_KEY_EXPORT")
  endif()
  if ("secret-callback" IN_LIST FEATURES)
      string(APPEND VCPKG_COMBINED_C_FLAGS_${config} " -DHAVE_SECRET_CALLBACK")
  endif()
endforeach()

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS
      -DWOLFSSL_BUILD_OUT_OF_TREE=yes
      -DWOLFSSL_EXAMPLES=no
      -DWOLFSSL_CRYPT_TESTS=no
      -DWOLFSSL_OPENSSLALL=yes
      -DWOLFSSL_TLSX=yes
      -DWOLFSSL_ASN=yes
      -DWOLFSSL_KEYGEN=yes
      -DWOLFSSL_CERTGEN=yes
      -DWOLFSSL_ASIO=${ENABLE_ASIO}
      -DWOLFSSL_DTLS=${ENABLE_DTLS}
      -DWOLFSSL_DTLS13=${ENABLE_DTLS}
      -DWOLFSSL_DTLS_CID=${ENABLE_DTLS}
      -DWOLFSSL_QUIC=${ENABLE_QUIC}
      -DWOLFSSL_SESSION_TICKET=${ENABLE_QUIC}
      -DWOLFSSL_CURVE25519=${ENABLE_CURVE25519}
      -DWOLFSSL_ED25519=${ENABLE_ED25519}
    OPTIONS_RELEASE
      -DCMAKE_C_FLAGS=${VCPKG_COMBINED_C_FLAGS_RELEASE}
    OPTIONS_DEBUG
      -DCMAKE_C_FLAGS=${VCPKG_COMBINED_C_FLAGS_DEBUG}
      -DWOLFSSL_DEBUG=yes)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/wolfssl)

if(VCPKG_TARGET_IS_IOS OR VCPKG_TARGET_IS_OSX)
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/lib/pkgconfig/wolfssl.pc" "Libs.private: " "Libs.private: -framework CoreFoundation -framework Security ")
    if(NOT VCPKG_BUILD_TYPE)
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/wolfssl.pc" "Libs.private: " "Libs.private: -framework CoreFoundation -framework Security ")
    endif()
endif()
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
