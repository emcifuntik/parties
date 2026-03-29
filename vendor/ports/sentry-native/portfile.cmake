vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/getsentry/sentry-native/releases/download/${VERSION}/sentry-native.zip"
    FILENAME "sentry-native-${VERSION}.zip"
    SHA512 a5b9fbfdfbd2cbe218d77cb2385a7ba2d7cacbd78b4265b0138dfbe9a9ad6a6dfd9cb9c795251741828fef6330877d1fecd8aed81b202a903ef2e4bd8811091b
)

vcpkg_extract_source_archive(
    SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    NO_REMOVE_ONE_LEVEL
    # No patches needed for inproc backend (crashpad patches don't apply).
)

vcpkg_list(SET options)

if(NOT "backend" IN_LIST FEATURES)
    vcpkg_list(APPEND options "-DSENTRY_BACKEND=none")
else()
    # Use inproc backend: captures crashes in-process, no separate
    # crashpad_handler.exe needed. Single-exe deployment.
    vcpkg_list(APPEND options "-DSENTRY_BACKEND=inproc")
endif()

if(NOT "transport" IN_LIST FEATURES)
    vcpkg_list(APPEND options "-DSENTRY_TRANSPORT=none")
endif()

if("compression" IN_LIST FEATURES)
    vcpkg_list(APPEND options "-DSENTRY_TRANSPORT_COMPRESSION=ON")
endif()

if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
    set(VCPKG_CXX_FLAGS "/D_CRT_DECLARE_NONSTDC_NAMES ${VCPKG_CXX_FLAGS}")
    set(VCPKG_C_FLAGS "/D_CRT_DECLARE_NONSTDC_NAMES ${VCPKG_C_FLAGS}")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${options}
        -DSENTRY_BUILD_TESTS=OFF
        -DSENTRY_BUILD_EXAMPLES=OFF
        -DCRASHPAD_ZLIB_SYSTEM=ON
    MAYBE_UNUSED_VARIABLES
        CRASHPAD_ZLIB_SYSTEM
)

vcpkg_cmake_install()

vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_cmake_config_fixup(PACKAGE_NAME sentry CONFIG_PATH lib/cmake/sentry)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
