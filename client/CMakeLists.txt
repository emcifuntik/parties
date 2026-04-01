find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(rnnoise CONFIG REQUIRED)
find_package(RmlUi CONFIG REQUIRED)
find_path(MINIAUDIO_INCLUDE_DIRS "miniaudio.h")


find_path(SPEEXDSP_INCLUDE_DIRS "speex/speex_echo.h")
find_library(SPEEXDSP_LIBRARY speexdsp)

if(NOT APPLE)
    find_package(D3D12MemoryAllocator CONFIG REQUIRED)

    # Generate .rc with version info and icon
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/res/logo.ico" ICON_PATH)
    set(RC_VERSION_MAJOR ${PARTIES_VERSION_MAJOR})
    set(RC_VERSION_MINOR ${PARTIES_VERSION_MINOR})
    set(RC_VERSION_PATCH ${PARTIES_VERSION_PATCH})
    set(RC_VERSION_BUILD ${PARTIES_VERSION_BUILD})
    set(RC_VERSION_STR "${PARTIES_VERSION_STR}")
    configure_file(res/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
    configure_file(res/app.manifest "${CMAKE_CURRENT_BINARY_DIR}/app.manifest" COPYONLY)
endif()

# ── Cross-platform sources ────────────────────────────────────────────────────
# Platform-specific files live in CLIENT_PLATFORM_SOURCES below.
set(CLIENT_COMMON_SOURCES
    src/lobby_model.cpp
    src/server_list_model.cpp
    src/settings.cpp
    src/audio_engine.cpp
    src/voice_mixer.cpp
    src/miniaudio_impl.cpp
    src/RmlUI_Backend.cpp
    src/video_element.cpp
    src/level_meter_element.cpp
    src/gradient_circle_element.cpp
    src/stream_audio_player.cpp
    src/app_core.cpp
    src/chat_model.cpp
    src/sound_player.cpp
    src/net_client_msquic.cpp
    src/slug_glyph_cache.cpp
    src/slug_font_engine.cpp
)

# ── Platform-specific sources ─────────────────────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(CLIENT_PLATFORM_SOURCES
        # Entry point & app (UIKit / Metal)
        src/ios/main.mm
        src/ios/AppDelegate.mm
        src/ios/app_ios.mm

        # RmlUI Metal backend (renderer shared with macOS)
        src/metal/RmlUi_Renderer_Metal.mm
        src/ios/RmlUi_Backend_iOS_Metal.mm
        src/ios/RmlUi_Platform_iOS.mm
    )
elseif(APPLE)
    set(CLIENT_PLATFORM_SOURCES
        # Entry point & window (AppKit / Metal)
        src/macos/main.mm
        src/macos/app_macos.mm

        # RmlUI Metal backend (shared with iOS)
        src/metal/RmlUi_Renderer_Metal.mm
        src/metal/RmlUi_Backend_macOS_Metal.mm
        src/macos/RmlUi_Platform_macOS.mm

        # Screen capture — ScreenCaptureKit (macOS 12.3+)
        src/macos/screen_capture_macos.mm

        # Auto-updater (Sparkle)
        src/macos/auto_updater_macos.mm
    )
else()
    set(CLIENT_PLATFORM_SOURCES
        # Platform specific
        src/windows/main.cpp
        src/windows/app.cpp
        src/windows/stream_audio_capture.cpp
        src/windows/RmlUi_Platform_Win32.cpp
        src/windows/ui_manager.cpp
        src/windows/context_menu.cpp

        # RmlUI rendering backends
        src/dx11/RmlUi_Renderer_DX11.cpp
        src/dx12/RmlUi_Renderer_DX12.cpp
        src/dx12/RmlUi_Renderer_DX12WL.cpp
        src/dx12/offsetAllocator.cpp
        src/vulkan/RmlUi_Renderer_VK.cpp

        # Auto-updater (WinHTTP)
        src/windows/auto_updater.cpp

        # Screen capture — DXGI
        src/windows/screen_capture.cpp

        # Video — Media Foundation / hardware encoders
        src/windows/video_encoder.cpp
        src/windows/video_decoder.cpp

        # Windows resource file
        "${CMAKE_CURRENT_BINARY_DIR}/app.rc"
    )
endif()

# miniaudio CoreAudio backend requires Objective-C++ on Apple platforms
if(APPLE)
    set_source_files_properties(
        src/audio_engine.cpp
        src/miniaudio_impl.cpp
        PROPERTIES LANGUAGE OBJCXX
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(IOS_ASSET_CATALOG "${CMAKE_CURRENT_SOURCE_DIR}/src/ios/Assets.xcassets")
    set(IOS_PRIVACY_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/src/ios/PrivacyInfo.xcprivacy")

    add_executable(parties_client MACOSX_BUNDLE
        ${CLIENT_COMMON_SOURCES}
        ${CLIENT_PLATFORM_SOURCES}
        ${IOS_ASSET_CATALOG}
        ${IOS_PRIVACY_MANIFEST}
    )

    set_source_files_properties(${IOS_ASSET_CATALOG} PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
    set_source_files_properties(${IOS_PRIVACY_MANIFEST} PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
else()
    add_executable(parties_client
        ${CLIENT_COMMON_SOURCES}
        ${CLIENT_PLATFORM_SOURCES}
    )
endif()

# Disable CMake's auto-generated manifest — we embed our own via app.rc
if(NOT APPLE)
    set_target_properties(parties_client PROPERTIES LINK_FLAGS "/MANIFEST:NO")
endif()

# #embed dependencies — recompile when embedded files change
file(GLOB EMBED_UI_RESOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/*.rml
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/*.rcss
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/*.svg
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/fonts/*.ttf
)
file(GLOB EMBED_SOUND_RESOURCES
    ${CMAKE_SOURCE_DIR}/sounds/*.wav
)
set_source_files_properties(src/rmlui_backend.cpp PROPERTIES
    OBJECT_DEPENDS "${EMBED_UI_RESOURCES}"
)
set_source_files_properties(src/sound_player.cpp PROPERTIES
    OBJECT_DEPENDS "${EMBED_SOUND_RESOURCES}"
)

target_include_directories(parties_client
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        $<$<STREQUAL:${CMAKE_SYSTEM_NAME},iOS>:${CMAKE_CURRENT_SOURCE_DIR}/src/ios>
        $<$<AND:$<BOOL:${APPLE}>,$<NOT:$<STREQUAL:${CMAKE_SYSTEM_NAME},iOS>>>:${CMAKE_CURRENT_SOURCE_DIR}/src/macos>
        ${MINIAUDIO_INCLUDE_DIRS}
        $<$<NOT:$<BOOL:${APPLE}>>:${CMAKE_CURRENT_SOURCE_DIR}/src/dx12>
        $<$<NOT:$<BOOL:${APPLE}>>:${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan>
        ${SPEEXDSP_INCLUDE_DIRS}
)

# Retail: no console window on Windows
if(NOT APPLE AND BUILD_IN_RETAIL)
    target_link_options(parties_client PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

# ── Apple bundle properties (shared + platform-specific) ─────────────────────
if(APPLE)
    set_target_properties(parties_client PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Parties"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "MIT License"
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${APPLE_DEVELOPMENT_TEAM}"
        XCODE_ATTRIBUTE_ENABLE_BITCODE "NO"
        XCODE_ATTRIBUTE_GENERATE_INFOPLIST_FILE "NO"
    )

    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set_target_properties(parties_client PROPERTIES
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.parties.ios"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/src/ios/Info.plist"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.parties.ios"
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1"    # iPhone only
            XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "16.3"
            XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
            XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/src/ios/Parties.entitlements"
            XCODE_ATTRIBUTE_SKIP_INSTALL "NO"
            XCODE_ATTRIBUTE_INSTALL_PATH "$(LOCAL_APPS_DIR)"
        )
    else()
        set_target_properties(parties_client PROPERTIES
            MACOSX_BUNDLE_GUI_IDENTIFIER "org.parties.client"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/res/Info.plist.in"
            MACOSX_BUNDLE_ICON_FILE "AppIcon"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "org.parties.client"
        )
        # Copy .icns into the app bundle Resources folder
        set(MACOS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/AppIcon.icns")
        set_source_files_properties(${MACOS_ICON} PROPERTIES
            MACOSX_PACKAGE_LOCATION Resources)
        target_sources(parties_client PRIVATE ${MACOS_ICON})
    endif()
endif()

target_link_libraries(parties_client
    PRIVATE
        parties_common
        parties_encdec
        rnnoise::rnnoise
        unofficial::sqlite3::sqlite3
        RmlUi::RmlUi
        $<$<NOT:$<BOOL:${BUILD_IN_RETAIL}>>:RmlUi::Debugger>
        ${SPEEXDSP_LIBRARY}
)

if(APPLE)
    # Shared Apple frameworks
    target_link_libraries(parties_client
        PRIVATE
            "-framework Metal"
            "-framework MetalKit"
            "-framework Foundation"
            "-framework QuartzCore"
            "-framework CoreGraphics"
            "-framework CoreText"
            "-framework AVFoundation"
            "-framework AudioToolbox"
            "-framework CoreAudio"
            "-framework Security"
    )
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        target_link_libraries(parties_client PRIVATE "-framework UIKit")
    else()
        target_link_libraries(parties_client PRIVATE
            "-framework AppKit"
            "-framework ScreenCaptureKit"
        )
    endif()
else()
    target_link_libraries(parties_client
        PRIVATE
            GPUOpen::D3D12MemoryAllocator
            d3d11 d3d12 dxgi d3dcompiler
            windowsapp
            dwmapi
            winhttp wintrust crypt32
    )
endif()

if(ENABLE_SENTRY AND NOT SENTRY_DSN_CLIENT STREQUAL "")
    target_compile_definitions(parties_client PRIVATE "SENTRY_DSN_VALUE=\"${SENTRY_DSN_CLIENT}\"")
endif()

# ── Sentry Cocoa SDK for Apple platforms ──────────────────────────────────────
# Use sentry-cocoa xcframework on iOS and macOS instead of sentry-native.
if(APPLE AND ENABLE_SENTRY)
    include(FetchContent)
    set(SENTRY_COCOA_VERSION "9.7.0")
    FetchContent_Declare(sentry_cocoa
        URL "https://github.com/getsentry/sentry-cocoa/releases/download/${SENTRY_COCOA_VERSION}/Sentry.xcframework.zip"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(sentry_cocoa)

    # The zip extracts Sentry.xcframework/ contents directly into SOURCE_DIR
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(SENTRY_FW "${sentry_cocoa_SOURCE_DIR}/ios-arm64_arm64e/Sentry.framework")
    else()
        set(SENTRY_FW "${sentry_cocoa_SOURCE_DIR}/macos-arm64_arm64e_x86_64/Sentry.framework")
    endif()

    add_library(SentryCocoa STATIC IMPORTED)
    get_filename_component(SENTRY_FW_DIR "${SENTRY_FW}" DIRECTORY)
    set_target_properties(SentryCocoa PROPERTIES
        IMPORTED_LOCATION "${SENTRY_FW}/Sentry"
    )

    target_link_libraries(parties_client PRIVATE SentryCocoa)
    # Use -F for framework-style imports: #import <Sentry/Sentry.h>
    target_compile_options(parties_client PRIVATE "-F${SENTRY_FW_DIR}")
    target_compile_definitions(parties_client PRIVATE SENTRY_COCOA_ENABLED)

    # Sentry Cocoa contains Swift code — link Swift runtime libraries
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(_SWIFT_PLATFORM "iphoneos")
    else()
        set(_SWIFT_PLATFORM "macosx")
    endif()
    execute_process(
        COMMAND xcrun --sdk ${_SWIFT_PLATFORM} --show-sdk-path
        OUTPUT_VARIABLE _SWIFT_SDK_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND xcrun --sdk ${_SWIFT_PLATFORM} --find swiftc
        OUTPUT_VARIABLE _SWIFTC_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    get_filename_component(_SWIFT_TOOLCHAIN "${_SWIFTC_PATH}" DIRECTORY)
    get_filename_component(_SWIFT_TOOLCHAIN "${_SWIFT_TOOLCHAIN}" DIRECTORY)
    target_link_directories(parties_client PRIVATE
        "${_SWIFT_TOOLCHAIN}/lib/swift/${_SWIFT_PLATFORM}"
        "${_SWIFT_SDK_PATH}/usr/lib/swift"
    )
    target_link_libraries(parties_client PRIVATE "-lswiftCore")
endif()

# ── Sparkle auto-updater for macOS ───────────────────────────────────────────
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND BUILD_IN_RETAIL)
    include(FetchContent)
    set(SPARKLE_VERSION "2.9.1")
    FetchContent_Declare(sparkle
        URL "https://github.com/sparkle-project/Sparkle/releases/download/${SPARKLE_VERSION}/Sparkle-${SPARKLE_VERSION}.tar.xz"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(sparkle)

    target_compile_options(parties_client PRIVATE "-F${sparkle_SOURCE_DIR}")
    target_link_libraries(parties_client PRIVATE "-F${sparkle_SOURCE_DIR}" "-framework Sparkle")
    target_compile_definitions(parties_client PRIVATE SPARKLE_ENABLED)

    # Set rpath so the dynamic linker finds Sparkle.framework in the app bundle
    target_link_options(parties_client PRIVATE "LINKER:-rpath,@executable_path/../Frameworks")

    # Embed Sparkle.framework in the app bundle (ditto preserves symlinks, unlike cmake -E copy_directory)
    add_custom_command(TARGET parties_client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:parties_client>/Contents/Frameworks"
        COMMAND ditto "${sparkle_SOURCE_DIR}/Sparkle.framework"
            "$<TARGET_BUNDLE_DIR:parties_client>/Contents/Frameworks/Sparkle.framework"
    )
endif()

# Copy client exe to additional test folders for multi-instance testing (Windows only)
if(NOT APPLE)
    foreach(COPY_DIR client2 client3 client4)
        add_custom_command(TARGET parties_client POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/${COPY_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:parties_client>"
                "${CMAKE_BINARY_DIR}/${COPY_DIR}/parties_client.exe"
        )
    endforeach()
endif()
