find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(rnnoise CONFIG REQUIRED)
find_package(RmlUi CONFIG REQUIRED)
find_package(D3D12MemoryAllocator CONFIG REQUIRED)
find_package(dav1d CONFIG REQUIRED)
find_path(MINIAUDIO_INCLUDE_DIRS "miniaudio.h")
find_path(SPEEXDSP_INCLUDE_DIRS "speex/speex_echo.h")
find_library(SPEEXDSP_LIBRARY speexdsp)

# Generate .rc with absolute icon path (llvm-rc resolves relative to build dir)
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/res/logo.ico" ICON_PATH)
configure_file(res/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)

add_executable(parties_client
    src/main.cpp
    src/app.cpp
    src/lobby_model.cpp
    src/server_list_model.cpp
    src/net_client.cpp
    src/settings.cpp
    src/audio_engine.cpp
    src/voice_mixer.cpp
    src/sound_player.cpp
    src/context_menu.cpp
    src/miniaudio_impl.cpp
    src/rmlui_backend.cpp
    src/ui_manager.cpp
    src/dx12/RmlUi_Renderer_DX12.cpp
    src/dx12/RmlUi_Platform_Win32.cpp
    src/dx12/offsetAllocator.cpp
    src/screen_capture.cpp
    src/video_encoder.cpp
    src/video_decoder.cpp
    src/video_element.cpp
    src/level_meter_element.cpp
    src/stream_audio_capture.cpp
    src/stream_audio_player.cpp
    src/nvidia/nvidia_loader.cpp
    src/nvidia/nvenc_encoder.cpp
    src/nvidia/nvdec_decoder.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/app.rc"
)

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
        ${CMAKE_CURRENT_SOURCE_DIR}/src/dx12
        ${MINIAUDIO_INCLUDE_DIRS}
        ${SPEEXDSP_INCLUDE_DIRS}
)

# Release: hide console (WINDOWS subsystem), Debug: keep console
# set_target_properties(parties_client PROPERTIES
#     WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>
# )
# target_link_options(parties_client PRIVATE
#     $<$<NOT:$<CONFIG:Debug>>:/ENTRY:mainCRTStartup>
# )

target_link_libraries(parties_client
    PRIVATE
        parties_common
        rnnoise::rnnoise
        unofficial::sqlite3::sqlite3
        RmlUi::RmlUi
        RmlUi::Debugger
        GPUOpen::D3D12MemoryAllocator
        dav1d::dav1d
        ${SPEEXDSP_LIBRARY}
        d3d12 dxgi d3dcompiler
        windowsapp
        dwmapi
)

# Copy client exe to additional test folders for multi-instance testing
foreach(COPY_DIR client2 client3 client4)
    add_custom_command(TARGET parties_client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/${COPY_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:parties_client>"
            "${CMAKE_BINARY_DIR}/${COPY_DIR}/parties_client.exe"
    )
endforeach()

