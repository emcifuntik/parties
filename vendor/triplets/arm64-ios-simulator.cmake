set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME iOS)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET "16.3")

# Keep dependencies on the same SDK and deployment target as the app. Without
# this, vcpkg may use the host SDK version and produce linker warnings for every
# static library when the app targets an older iOS release.
execute_process(COMMAND xcrun --sdk iphonesimulator --show-sdk-path
    OUTPUT_VARIABLE _sdk_path OUTPUT_STRIP_TRAILING_WHITESPACE)
set(VCPKG_CMAKE_CONFIGURE_OPTIONS
    "-DCMAKE_OSX_SYSROOT=${_sdk_path}"
    "-DCMAKE_OSX_DEPLOYMENT_TARGET=16.3")
