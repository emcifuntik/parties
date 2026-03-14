find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(rnnoise CONFIG REQUIRED)
find_package(RmlUi CONFIG REQUIRED)
find_path(MINIAUDIO_INCLUDE_DIRS "miniaudio.h")


find_path(SPEEXDSP_INCLUDE_DIRS "speex/speex_echo.h")
find_library(SPEEXDSP_LIBRARY speexdsp)

if(NOT APPLE)
    find_package(D3D12MemoryAllocator CONFIG REQUIRED)

    # Generate .rc with absolute icon path (llvm-rc resolves relative to build dir)
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/res/logo.ico" ICON_PATH)
    configure_file(res/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
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
    src/stream_audio_player.cpp
    src/app_core.cpp
    src/sound_player.cpp
    src/net_client_msquic.cpp
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
    # miniaudio CoreAudio backend requires Objective-C++
    set_source_files_properties(
        src/audio_engine.cpp
        src/miniaudio_impl.cpp
        PROPERTIES LANGUAGE OBJCXX
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

        # Sound player — pure miniaudio, same as Windows
    )
    # miniaudio CoreAudio backend requires Objective-C++
    set_source_files_properties(
        src/audio_engine.cpp
        src/miniaudio_impl.cpp
        PROPERTIES LANGUAGE OBJCXX
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

        # Screen capture — DXGI
        src/windows/screen_capture.cpp

        # Video — Media Foundation / hardware encoders
        src/windows/video_encoder.cpp
        src/windows/video_decoder.cpp

        # Windows resource file
        "${CMAKE_CURRENT_BINARY_DIR}/app.rc"
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # ── iOS bundle resources ──────────────────────────────────────────────
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

    set_target_properties(parties_client PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER      "com.parties.ios"
        MACOSX_BUNDLE_BUNDLE_VERSION      "1"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
        MACOSX_BUNDLE_INFO_PLIST          "${CMAKE_CURRENT_SOURCE_DIR}/src/ios/Info.plist"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.parties.ios"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY    "1"    # iPhone only
        XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "16.0"
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY         "Apple Development"
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE            "Automatic"
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM           "${APPLE_DEVELOPMENT_TEAM}"
        XCODE_ATTRIBUTE_ENABLE_BITCODE             "NO"
        XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS     "${CMAKE_CURRENT_SOURCE_DIR}/src/ios/Parties.entitlements"
        XCODE_ATTRIBUTE_GENERATE_INFOPLIST_FILE    "NO"
        # Archive must be "iOS App Archive" (not Generic) for TestFlight
        XCODE_ATTRIBUTE_SKIP_INSTALL               "NO"
        XCODE_ATTRIBUTE_INSTALL_PATH               "$(LOCAL_APPS_DIR)"
    )
else()
    add_executable(parties_client
        ${CLIENT_COMMON_SOURCES}
        ${CLIENT_PLATFORM_SOURCES}
    )
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

# Release: hide console (WINDOWS subsystem), Debug: keep console
# set_target_properties(parties_client PROPERTIES
#     WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>
# )
# target_link_options(parties_client PRIVATE
#     $<$<NOT:$<CONFIG:Debug>>:/ENTRY:mainCRTStartup>
# )

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_link_libraries(parties_client
        PRIVATE
            parties_common
            parties_encdec
            rnnoise::rnnoise
            unofficial::sqlite3::sqlite3
            RmlUi::RmlUi
            RmlUi::Debugger
            ${SPEEXDSP_LIBRARY}
            "-framework Metal"
            "-framework MetalKit"
            "-framework UIKit"
            "-framework Foundation"
            "-framework QuartzCore"
            "-framework CoreGraphics"
            "-framework CoreText"
            "-framework AVFoundation"
            "-framework AudioToolbox"
            "-framework CoreAudio"
            "-framework Security"
    )
elseif(APPLE)
    target_link_libraries(parties_client
        PRIVATE
            parties_common
            parties_encdec
            rnnoise::rnnoise
            unofficial::sqlite3::sqlite3
            RmlUi::RmlUi
            RmlUi::Debugger
            ${SPEEXDSP_LIBRARY}
            "-framework Metal"
            "-framework MetalKit"
            "-framework AppKit"
            "-framework Foundation"
            "-framework QuartzCore"
            "-framework CoreGraphics"
            "-framework CoreText"
            "-framework AVFoundation"
            "-framework AudioToolbox"
            "-framework CoreAudio"
            "-framework ScreenCaptureKit"
            "-framework Security"
    )
else()
    target_link_libraries(parties_client
        PRIVATE
            parties_common
            parties_encdec
            rnnoise::rnnoise
            unofficial::sqlite3::sqlite3
            RmlUi::RmlUi
            RmlUi::Debugger
            GPUOpen::D3D12MemoryAllocator
            ${SPEEXDSP_LIBRARY}
            d3d11 d3d12 dxgi d3dcompiler
            windowsapp
            dwmapi
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
