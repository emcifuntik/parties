vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO xiph/rnnoise
    REF 70f1d256acd4b34a572f999a05c87bf00b67730d
    SHA512 2799c4c392abebb37b6509a041766451307b6b6edfdc6f9f9db62ebe38a9deb7cf67adcc7e5a243e818bf0b680dd7b2f2aada514c0a396190bac82a5507571ec
    HEAD_REF main
)

# Download pre-trained model weights
vcpkg_download_distfile(
    MODEL_ARCHIVE
    URLS "https://media.xiph.org/rnnoise/models/rnnoise_data-0a8755f8e2d834eff6a54714ecc7d75f9932e845df35f8b59bc52a7cfe6e8b37.tar.gz"
    FILENAME "rnnoise_data-0a8755f8e2d834eff6a54714ecc7d75f9932e845df35f8b59bc52a7cfe6e8b37.tar.gz"
    SHA512 b327d2fc5095be9ed66c5246a86b1a1ce180e9de875c4e5e8778f975560d1f035da40a8686dc1c3fd91c8e709be65d2638eccaa9f866b6f3d85f8d0d16bd2184
)

# Extract model data into the source tree
vcpkg_extract_source_archive(
    MODEL_PATH
    ARCHIVE "${MODEL_ARCHIVE}"
    NO_REMOVE_ONE_LEVEL
)
file(COPY "${MODEL_PATH}/src/rnnoise_data.c" DESTINATION "${SOURCE_PATH}/src/")
file(COPY "${MODEL_PATH}/src/rnnoise_data.h" DESTINATION "${SOURCE_PATH}/src/")

# Copy our CMake build files into the source tree
file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")
file(MAKE_DIRECTORY "${SOURCE_PATH}/cmake")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/rnnoise-config.cmake.in" DESTINATION "${SOURCE_PATH}/cmake")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(
    PACKAGE_NAME rnnoise
    CONFIG_PATH lib/cmake/rnnoise
)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
