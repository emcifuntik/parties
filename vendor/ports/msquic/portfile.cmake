vcpkg_from_github(
    OUT_SOURCE_PATH QUIC_SOURCE_PATH
    REPO microsoft/msquic
    REF "v${VERSION}"
    SHA512 a16ec3de6a0a68256a4688586d6205ef9ddc4ea22a3ea2b208d4b953faf4369a0dd573b9e27bb933344f37dbfb421fa6f819a70e87a7d02a0b4971adde60dfdb
    HEAD_REF main
)

string(COMPARE EQUAL "${VCPKG_CRT_LINKAGE}" "static" STATIC_CRT)

# ── Windows-only patches ──────────────────────────────────────
if(VCPKG_TARGET_IS_WINDOWS)

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

    # clang-cl: /Qspectre and /guard:cf are MSVC-only.
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

    # Disable /analyze for clang-cl (MSVC is TRUE for clang-cl but /analyze is MSVC-only)
    vcpkg_replace_string("${QUIC_SOURCE_PATH}/src/platform/CMakeLists.txt"
    [[if (MSVC AND (QUIC_TLS_LIB STREQUAL "quictls" OR QUIC_TLS_LIB STREQUAL "schannel") AND NOT QUIC_ENABLE_SANITIZERS)]]
    [[if (MSVC AND NOT (CMAKE_C_COMPILER_ID STREQUAL "Clang") AND (QUIC_TLS_LIB STREQUAL "quictls" OR QUIC_TLS_LIB STREQUAL "schannel") AND NOT QUIC_ENABLE_SANITIZERS)]]
    )
    vcpkg_replace_string("${QUIC_SOURCE_PATH}/src/core/CMakeLists.txt"
    [[if (MSVC AND NOT QUIC_ENABLE_SANITIZERS)]]
    [[if (MSVC AND NOT (CMAKE_C_COMPILER_ID STREQUAL "Clang") AND NOT QUIC_ENABLE_SANITIZERS)]]
    )

endif() # VCPKG_TARGET_IS_WINDOWS

# ── Cross-platform: quictls (OpenSSL fork with QUIC API) ─────────
# MsQuic's FetchContent expects quictls source in submodules/openssl/
vcpkg_from_github(
    OUT_SOURCE_PATH QUICTLS_SOURCE
    REPO quictls/openssl
    REF openssl-3.1.7+quic
    SHA512 152824320d988c87bc6848b558a2fd472ce8d564d36373ad3e9f7a1f862f74bd48e052d5934ea9d9732158aa534fe21825fdb1572569622373a457a8c0c78c36
    HEAD_REF openssl-3.1.7+quic
)

file(REMOVE_RECURSE "${QUIC_SOURCE_PATH}/submodules/quictls")
file(RENAME "${QUICTLS_SOURCE}" "${QUIC_SOURCE_PATH}/submodules/quictls")

# MsQuic's flatten_link_dependencies tries to merge system libs like -lm
# into the monolithic archive. Exclude them.
vcpkg_replace_string("${QUIC_SOURCE_PATH}/src/bin/CMakeLists.txt"
    [[set(EXCLUDE_LIST "inc")]]
    [[set(EXCLUDE_LIST "inc" "m" "pthread" "rt" "atomic")]]
)

# quictls build requires Perl and NASM (called via cmd.exe from ninja custom commands).
# vcpkg isolates the build environment and strips PATH, so we must explicitly inject them.
if(VCPKG_TARGET_IS_WINDOWS)
    find_program(PERL_EXECUTABLE perl
        PATHS "C:/Strawberry/perl/bin" ENV PATH
        NO_DEFAULT_PATH
    )
    if(NOT PERL_EXECUTABLE)
        find_program(PERL_EXECUTABLE perl)
    endif()
    if(PERL_EXECUTABLE)
        cmake_path(GET PERL_EXECUTABLE PARENT_PATH _perl_dir)
    else()
        set(_perl_dir "C:/Strawberry/perl/bin")
    endif()
    vcpkg_host_path_list(PREPEND ENV{PATH} "${_perl_dir}")
    # NASM for OpenSSL assembly optimizations
    vcpkg_host_path_list(PREPEND ENV{PATH} "${CMAKE_CURRENT_LIST_DIR}/../../../vendor/nasm")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${QUIC_SOURCE_PATH}"
    OPTIONS
        -DQUIC_TLS_LIB=quictls
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
