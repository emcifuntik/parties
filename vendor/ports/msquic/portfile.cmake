vcpkg_from_github(
    OUT_SOURCE_PATH QUIC_SOURCE_PATH
    REPO microsoft/msquic
    REF 7bd862e49d9fcee754cf97397b1c69ff613afcee
    SHA512 5d803f30d0e0f530974f12259aa387a9d8e59c6e98fc71131c2bff6bbc3b3a83a92232e90338bce58363ca73cc7ab92dd6b9eaa65e8574466d4e82086f8eb86c
    HEAD_REF main
)

string(COMPARE EQUAL "${VCPKG_CRT_LINKAGE}" "static" STATIC_CRT)

# ── Windows-only patches ──────────────────────────────────────
if(VCPKG_TARGET_IS_WINDOWS)

    # XDP headers are needed for the Windows build even if XDP isn't used at runtime
    vcpkg_from_github(
        OUT_SOURCE_PATH XDP_WINDOWS
        REPO microsoft/xdp-for-windows
        REF 793dc2a0d7e6f8a86dae06c84de7d8fd6eacd205
        SHA512 28a6ab43602998991dcf0485f34100ae5f031ec5219ba7d78fc2bc652730697a829fc12bd7ed2281b42ff8f91c006b4f947d3b0cc69c8caabc030ecc1ce9a00c
        HEAD_REF release/1.1
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
    [[    if(NOT QUIC_SANITIZER_ACTIVE)
        check_c_compiler_flag(/Qspectre HAS_SPECTRE)
    endif()]]
    [[    if(NOT QUIC_SANITIZER_ACTIVE AND NOT (CMAKE_C_COMPILER_ID STREQUAL "Clang"))
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
    [[if (MSVC AND (QUIC_TLS_LIB STREQUAL "quictls" OR QUIC_TLS_LIB STREQUAL "schannel") AND NOT QUIC_SANITIZER_ACTIVE)]]
    [[if (MSVC AND NOT (CMAKE_C_COMPILER_ID STREQUAL "Clang") AND (QUIC_TLS_LIB STREQUAL "quictls" OR QUIC_TLS_LIB STREQUAL "schannel" OR QUIC_TLS_LIB STREQUAL "openssl") AND NOT QUIC_SANITIZER_ACTIVE)]]
    )
    vcpkg_replace_string("${QUIC_SOURCE_PATH}/src/core/CMakeLists.txt"
    [[if (MSVC AND NOT QUIC_SANITIZER_ACTIVE)]]
    [[if (MSVC AND NOT (CMAKE_C_COMPILER_ID STREQUAL "Clang") AND NOT QUIC_SANITIZER_ACTIVE)]]
    )

endif() # VCPKG_TARGET_IS_WINDOWS

# Allow QUIC_PARAM_CONN_DISABLE_1RTT_ENCRYPTION to be set even after State.Started.
# The QUIC_CONN_BAD_START_STATE check prevents setting it from the NEW_CONNECTION
# listener callback because MsQuic's API dispatch queues the SetParam to the
# connection worker, where ConnectionSetConfiguration (also queued) may run first.
# Remove both the BAD_START_STATE and the PeerTransportParameter checks from
# DISABLE_1RTT_ENCRYPTION SetParam. The SetParam is dispatched async to the
# connection worker, so by the time it runs: (1) State.Started may be true,
# and (2) PeerTransportParameterValid may be true without the flag because
# both sides are trying to set the flag simultaneously. The handshake
# negotiation handles the actual agreement via transport parameters.
vcpkg_replace_string("${QUIC_SOURCE_PATH}/src/core/connection.c"
    [[    case QUIC_PARAM_CONN_DISABLE_1RTT_ENCRYPTION:

        if (BufferLength != sizeof(BOOLEAN)) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        if (QUIC_CONN_BAD_START_STATE(Connection)) {
            Status = QUIC_STATUS_INVALID_STATE;
            break;
        }

        if (Connection->State.PeerTransportParameterValid &&
            (!(Connection->PeerTransportParams.Flags & QUIC_TP_FLAG_DISABLE_1RTT_ENCRYPTION))) {
            //
            // The peer did't negotiate the feature.
            //
            Status = QUIC_STATUS_INVALID_STATE;
            break;
        }

        Connection->State.Disable1RttEncrytion = *(BOOLEAN*)Buffer;]]
    [[    case QUIC_PARAM_CONN_DISABLE_1RTT_ENCRYPTION:

        if (BufferLength != sizeof(BOOLEAN)) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        Connection->State.Disable1RttEncrytion = *(BOOLEAN*)Buffer;]]
)

# MsQuic's flatten_link_dependencies tries to merge system libs into the
# monolithic archive. Exclude them.
vcpkg_replace_string("${QUIC_SOURCE_PATH}/src/bin/CMakeLists.txt"
    [[set(EXCLUDE_LIST "inc")]]
    [[set(EXCLUDE_LIST "inc" "m" "pthread" "rt" "atomic" "OpenSSL::SSL" "OpenSSL::Crypto")]]
)

vcpkg_cmake_configure(
    SOURCE_PATH "${QUIC_SOURCE_PATH}"
    OPTIONS
        -DQUIC_TLS_LIB=openssl
        -DQUIC_USE_EXTERNAL_OPENSSL=ON
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
