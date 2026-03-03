cmake_minimum_required(VERSION 3.21)

# ── vcpkg toolchain (before project()) ──────────────────────────────────────
# Defaults to the parties repo's own vcpkg submodule.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain")
endif()

if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "arm64-ios" CACHE STRING "vcpkg target triplet")
endif()

# Overlay ports: parties repo ships patched ports for rnnoise, wolfssl, enet, etc.
if(NOT DEFINED VCPKG_OVERLAY_PORTS)
    set(VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_SOURCE_DIR}/../ports"
        CACHE STRING "vcpkg overlay ports")
endif()

# Overlay triplets: inherit custom triplets if defined in the parties repo.
if(NOT DEFINED VCPKG_OVERLAY_TRIPLETS AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../triplets")
    set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_SOURCE_DIR}/../triplets"
        CACHE STRING "vcpkg overlay triplets")
endif()

# ── Apple signing ────────────────────────────────────────────────────────────
# Put  set(APPLE_DEVELOPMENT_TEAM "XXXXXXXXXX")  in local.cmake (gitignored).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/local.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/local.cmake")
endif()
if(NOT DEFINED APPLE_DEVELOPMENT_TEAM AND DEFINED ENV{APPLE_DEVELOPMENT_TEAM})
    set(APPLE_DEVELOPMENT_TEAM "$ENV{APPLE_DEVELOPMENT_TEAM}")
endif()
if(NOT DEFINED APPLE_DEVELOPMENT_TEAM)
    set(APPLE_DEVELOPMENT_TEAM "")
endif()

project(PartiesIOS VERSION 0.1 LANGUAGES CXX OBJCXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── parties_common (protocol / crypto / codec / serialization) ───────────────
add_subdirectory(../common common_build)

# ── RmlUi — use the submodule from the parent rmlui-iphone repo ──────────────
# Override with -DRMLUI_DIR=<path> to point elsewhere.
if(NOT DEFINED RMLUI_DIR)
    set(RMLUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../RmlUi"
        CACHE PATH "Path to RmlUi source tree")
endif()
set(BUILD_SHARED_LIBS      OFF CACHE BOOL "" FORCE)
set(RMLUI_SAMPLES          OFF CACHE BOOL "" FORCE)
set(RMLUI_FONT_ENGINE      "freetype" CACHE STRING "" FORCE)
add_subdirectory(${RMLUI_DIR} rmlui_build EXCLUDE_FROM_ALL)

# ── Other third-party deps via vcpkg ─────────────────────────────────────────
find_package(rnnoise CONFIG REQUIRED)
find_path(MINIAUDIO_INCLUDE_DIRS "miniaudio.h")

# ── Sources ───────────────────────────────────────────────────────────────────

# Portable client code shared with the Windows client (no platform deps).
set(SHARED_CLIENT_SOURCES
    ../client/src/net_client.cpp
    ../client/src/audio_engine.cpp
    ../client/src/voice_mixer.cpp
    ../client/src/miniaudio_impl.cpp
)

set(APP_SOURCES
    App/main.mm
    App/AppDelegate.mm
    App/PartiesViewController.mm
    App/app_model.mm
    Backends/RmlUi_Renderer_Metal.mm
    Backends/RmlUi_Platform_iOS.mm
    Backends/RmlUi_Backend_iOS_Metal.mm
)

set(APP_HEADERS
    App/AppDelegate.h
    App/PartiesViewController.h
    App/app_model.h
    Backends/RmlUi_Renderer_Metal.h
    Backends/RmlUi_Platform_iOS.h
    Backends/RmlUi_Backend_iOS_Metal.h
)

set(APP_RESOURCES
    App/Assets/lobby.rml
    App/Assets/style.rcss
    App/Assets/LatoLatin-Regular.ttf
)

add_executable(PartiesIOS MACOSX_BUNDLE
    ${SHARED_CLIENT_SOURCES}
    ${APP_SOURCES}
    ${APP_HEADERS}
    ${APP_RESOURCES}
)

set_source_files_properties(${APP_RESOURCES} PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources)

target_include_directories(PartiesIOS PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/App
    ${CMAKE_CURRENT_SOURCE_DIR}/Backends
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/include     # parties:: headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../client/include     # shared client headers
    ${MINIAUDIO_INCLUDE_DIRS}
)

target_link_libraries(PartiesIOS PRIVATE
    parties_common
    rnnoise::rnnoise
    RmlUi::Core
    "-framework Metal"
    "-framework MetalKit"
    "-framework UIKit"
    "-framework QuartzCore"
    "-framework Foundation"
    "-framework CoreGraphics"
    "-framework CoreText"
    "-framework AVFoundation"
    "-framework AudioToolbox"
    "-framework CoreAudio"
)

set_target_properties(PartiesIOS PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER      "com.parties.ios"
    MACOSX_BUNDLE_BUNDLE_VERSION      "1"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
    MACOSX_BUNDLE_INFO_PLIST          "${CMAKE_CURRENT_SOURCE_DIR}/App/Info.plist"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.parties.ios"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY    "1"    # iPhone
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "16.0"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY         "iPhone Developer"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM           "${APPLE_DEVELOPMENT_TEAM}"
    XCODE_ATTRIBUTE_ENABLE_BITCODE             "NO"
    # Microphone access is required for voice chat
    XCODE_ATTRIBUTE_NSMicrophoneUsageDescription "Parties needs your microphone for voice chat."
)
