vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO mikke89/RmlUi
    REF 0ae381e00d7426762bb5ed897973366358b16642
    SHA512 e796b00f2212287b7ad5c73a2fffa2112850e78fb0ee9fb690e61b707497156ff61c2297ba337f578fd54ca3c413775c59412b20dd3b8ff4ad53b0fdf8ed8439
    HEAD_REF master
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        svg             RMLUI_SVG_PLUGIN
        lottie          RMLUI_LOTTIE_PLUGIN
)

if("freetype" IN_LIST FEATURES)
    set(RMLUI_FONT_ENGINE "freetype")
else()
    set(RMLUI_FONT_ENGINE "none")
endif()

# The shell target exists only to build upstream samples. Parties consumes the
# RmlUi libraries directly and supplies its own Metal backend on Apple. Keep the
# pinned Win32/DX12 shell on Windows, where its backend sources are packaged for
# the Parties DX12 renderer, but do not cross-compile it for Apple platforms.
if(VCPKG_TARGET_IS_WINDOWS)
    set(RMLUI_SHELL_OPTIONS
        "-DRMLUI_SHELL=ON"
        "-DRMLUI_BACKEND=Win32_DX12"
    )
else()
    set(RMLUI_SHELL_OPTIONS
        "-DRMLUI_SHELL=OFF"
    )
endif()

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS
        ${FEATURE_OPTIONS}
        "-DRMLUI_FONT_ENGINE=${RMLUI_FONT_ENGINE}"
        "-DRMLUI_COMPILER_OPTIONS=OFF"
        ${RMLUI_SHELL_OPTIONS}
        "-DRMLUI_SAMPLES=OFF"
        "-DRMLUI_TESTS=OFF"
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/RmlUi)
vcpkg_copy_pdbs()

# Upstream installs its backend source bundle to the global share/Backends
# directory. Keep the package self-contained so consumers can locate the exact
# backend that belongs to this pinned RmlUi revision without name collisions.
if(EXISTS "${CURRENT_PACKAGES_DIR}/share/Backends")
    file(RENAME
        "${CURRENT_PACKAGES_DIR}/share/Backends"
        "${CURRENT_PACKAGES_DIR}/share/${PORT}/Backends"
    )
endif()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/RmlUi/Core/Header.h"
        "#if !defined RMLUI_STATIC_LIB"
        "#if 0"
    )
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/RmlUi/Debugger/Header.h"
        "#if !defined RMLUI_STATIC_LIB"
        "#if 0"
    )
endif()

configure_file("${CMAKE_CURRENT_LIST_DIR}/usage" "${CURRENT_PACKAGES_DIR}/share/${PORT}/usage" COPYONLY)
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")
