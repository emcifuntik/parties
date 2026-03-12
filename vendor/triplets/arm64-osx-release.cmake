set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET "13.0")

# Ensure SDK headers (Security.framework etc.) are found during vcpkg port builds
execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
    OUTPUT_VARIABLE _sdk_path OUTPUT_STRIP_TRAILING_WHITESPACE)
set(VCPKG_CMAKE_CONFIGURE_OPTIONS "-DCMAKE_OSX_SYSROOT=${_sdk_path}")
