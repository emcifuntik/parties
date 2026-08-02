find_package(RapidJSON CONFIG REQUIRED)
find_package(RmlUi CONFIG REQUIRED)

set(DESIGNER_SOURCES
    src/main.cpp
    src/designer_app.cpp
    src/file_watcher.cpp
    src/embedded_fonts.cpp

    # Reuse from client
	${CMAKE_SOURCE_DIR}/client/src/lobby_model.cpp
	${CMAKE_SOURCE_DIR}/client/src/context_window_model.cpp
	${CMAKE_SOURCE_DIR}/client/src/server_list_model.cpp
	${CMAKE_SOURCE_DIR}/client/src/chat_model.cpp
	${CMAKE_SOURCE_DIR}/client/src/gradient_circle_element.cpp
	${CMAKE_SOURCE_DIR}/client/src/level_meter_element.cpp
	${CMAKE_SOURCE_DIR}/client/src/video_element.cpp
)

add_executable(rmlui_designer ${DESIGNER_SOURCES})

target_include_directories(rmlui_designer PRIVATE
    ${CMAKE_SOURCE_DIR}/client/include
    ${CMAKE_SOURCE_DIR}/client/src          # RmlUi_RenderInterface_Extended.h
    ${CMAKE_SOURCE_DIR}/common/include      # parties/profiler.h (no-op stubs)
)

target_link_libraries(rmlui_designer PRIVATE
    RmlUi::RmlUi
    RmlUi::Debugger
    parties_rmlui_dx12_backend
    d3d12 dxgi d3dcompiler dcomp dwmapi
)

target_link_options(rmlui_designer PRIVATE /MANIFEST:NO)

# #embed dependencies — recompile when embedded font files change
file(GLOB DESIGNER_EMBED_FONTS ${CMAKE_SOURCE_DIR}/client/ui/fonts/*.ttf)
set_source_files_properties(src/embedded_fonts.cpp PROPERTIES
    OBJECT_DEPENDS "${DESIGNER_EMBED_FONTS}"
)

if(BUILD_TESTING)
    add_test(
        NAME parties_ui_designer_launcher
        COMMAND rmlui_designer
            "${CMAKE_SOURCE_DIR}/client/ui/lobby.rml"
            --asset-dir "${CMAKE_SOURCE_DIR}/client/ui"
            --fixture launcher
            --size 1440x900
            --frames 5
            --screenshot "${CMAKE_BINARY_DIR}/ui-designer-launcher.bmp"
    )
    add_test(
        NAME parties_ui_designer_party_modal
        COMMAND rmlui_designer
            "${CMAKE_SOURCE_DIR}/client/ui/lobby.rml"
            --asset-dir "${CMAKE_SOURCE_DIR}/client/ui"
            --fixture party-modal
            --size 1440x900
            --frames 5
            --screenshot "${CMAKE_BINARY_DIR}/ui-designer-party-modal.bmp"
    )
    add_test(
        NAME parties_ui_designer_onboarding
        COMMAND rmlui_designer
            "${CMAKE_SOURCE_DIR}/client/ui/lobby.rml"
            --asset-dir "${CMAKE_SOURCE_DIR}/client/ui"
            --fixture onboarding
            --size 1440x900
            --frames 5
            --screenshot "${CMAKE_BINARY_DIR}/ui-designer-onboarding.bmp"
    )
    add_test(
        NAME parties_ui_designer_recovery
        COMMAND rmlui_designer
            "${CMAKE_SOURCE_DIR}/client/ui/lobby.rml"
            --asset-dir "${CMAKE_SOURCE_DIR}/client/ui"
            --fixture recovery
            --size 1440x900
            --frames 5
            --screenshot "${CMAKE_BINARY_DIR}/ui-designer-recovery.bmp"
    )
    add_test(
        NAME parties_ui_designer_member
        COMMAND rmlui_designer
            "${CMAKE_SOURCE_DIR}/client/ui/lobby.rml"
            --asset-dir "${CMAKE_SOURCE_DIR}/client/ui"
            --fixture member
            --size 1440x900
            --frames 5
            --screenshot "${CMAKE_BINARY_DIR}/ui-designer-member.bmp"
    )
    add_test(
        NAME parties_ui_designer_smoke
        COMMAND rmlui_designer
            "${CMAKE_SOURCE_DIR}/client/ui/lobby.rml"
            --asset-dir "${CMAKE_SOURCE_DIR}/client/ui"
            --fixture room
            --size 1440x900
            --frames 5
            --screenshot "${CMAKE_BINARY_DIR}/ui-designer-smoke.bmp"
    )
    add_test(
        NAME parties_ui_designer_share
        COMMAND rmlui_designer
            "${CMAKE_SOURCE_DIR}/client/ui/lobby.rml"
            --asset-dir "${CMAKE_SOURCE_DIR}/client/ui"
            --fixture share
            --size 1440x900
            --frames 5
            --screenshot "${CMAKE_BINARY_DIR}/ui-designer-share.bmp"
    )
    add_test(
        NAME parties_ui_designer_audio_share
        COMMAND rmlui_designer
            "${CMAKE_SOURCE_DIR}/client/ui/lobby.rml"
            --asset-dir "${CMAKE_SOURCE_DIR}/client/ui"
            --fixture audio-share
            --size 1440x900
            --frames 5
            --screenshot "${CMAKE_BINARY_DIR}/ui-designer-audio-share.bmp"
    )
    add_test(
        NAME parties_ui_designer_context_user
        COMMAND rmlui_designer
            "${CMAKE_SOURCE_DIR}/client/ui/context_window.rml"
            --asset-dir "${CMAKE_SOURCE_DIR}/client/ui"
            --fixture context-user
            --size 420x520
            --frames 5
            --screenshot "${CMAKE_BINARY_DIR}/ui-designer-context-user.bmp"
    )
    add_test(
        NAME parties_ui_designer_context_channel
        COMMAND rmlui_designer
            "${CMAKE_SOURCE_DIR}/client/ui/context_window.rml"
            --asset-dir "${CMAKE_SOURCE_DIR}/client/ui"
            --fixture context-channel
            --size 310x310
            --frames 5
            --screenshot "${CMAKE_BINARY_DIR}/ui-designer-context-channel.bmp"
    )
    add_test(
        NAME parties_ui_designer_settings
        COMMAND rmlui_designer
            "${CMAKE_SOURCE_DIR}/client/ui/lobby.rml"
            --asset-dir "${CMAKE_SOURCE_DIR}/client/ui"
            --fixture settings
            --size 1440x900
            --frames 5
            --screenshot "${CMAKE_BINARY_DIR}/ui-designer-settings.bmp"
    )
    add_test(
        NAME parties_ui_designer_settings_select_open
        COMMAND rmlui_designer
            "${CMAKE_SOURCE_DIR}/client/ui/lobby.rml"
            --asset-dir "${CMAKE_SOURCE_DIR}/client/ui"
            --fixture settings-select-open
            --size 1440x900
            --frames 5
            --screenshot "${CMAKE_BINARY_DIR}/ui-designer-settings-select-open.bmp"
    )
    add_test(
        NAME parties_ui_designer_settings_screen_share
        COMMAND rmlui_designer
            "${CMAKE_SOURCE_DIR}/client/ui/lobby.rml"
            --asset-dir "${CMAKE_SOURCE_DIR}/client/ui"
            --fixture settings-screen-share
            --size 1440x900
            --frames 5
            --screenshot "${CMAKE_BINARY_DIR}/ui-designer-settings-screen-share.bmp"
    )
    add_test(
        NAME parties_ui_designer_settings_account
        COMMAND rmlui_designer
            "${CMAKE_SOURCE_DIR}/client/ui/lobby.rml"
            --asset-dir "${CMAKE_SOURCE_DIR}/client/ui"
            --fixture settings-account
            --size 1440x900
            --frames 5
            --screenshot "${CMAKE_BINARY_DIR}/ui-designer-settings-account.bmp"
    )
    add_test(
        NAME parties_ui_designer_settings_hotkeys
        COMMAND rmlui_designer
            "${CMAKE_SOURCE_DIR}/client/ui/lobby.rml"
            --asset-dir "${CMAKE_SOURCE_DIR}/client/ui"
            --fixture settings-hotkeys
            --size 1440x900
            --frames 5
            --screenshot "${CMAKE_BINARY_DIR}/ui-designer-settings-hotkeys.bmp"
    )
    add_test(
        NAME parties_ui_designer_chat_route
        COMMAND rmlui_designer
            "${CMAKE_SOURCE_DIR}/client/ui/lobby.rml"
            --asset-dir "${CMAKE_SOURCE_DIR}/client/ui"
            --fixture chat
            --size 1440x900
            --frames 5
            --screenshot "${CMAKE_BINARY_DIR}/ui-designer-chat.bmp"
    )
    add_test(
        NAME parties_ui_designer_chat_segment_churn
        COMMAND rmlui_designer
            "${CMAKE_SOURCE_DIR}/client/ui/lobby.rml"
            --asset-dir "${CMAKE_SOURCE_DIR}/client/ui"
            --fixture chat-segment-churn
            --size 1440x900
            --frames 8
            --screenshot "${CMAKE_BINARY_DIR}/ui-designer-chat-segment-churn.bmp"
    )
    add_test(
        NAME parties_ui_designer_stream_single
        COMMAND rmlui_designer
            "${CMAKE_SOURCE_DIR}/client/ui/lobby.rml"
            --asset-dir "${CMAKE_SOURCE_DIR}/client/ui"
            --fixture stream-single
            --size 1440x900
            --frames 5
            --screenshot "${CMAKE_BINARY_DIR}/ui-designer-stream-single.bmp"
    )
    add_test(
        NAME parties_ui_designer_stream_wide
        COMMAND rmlui_designer
            "${CMAKE_SOURCE_DIR}/client/ui/lobby.rml"
            --asset-dir "${CMAKE_SOURCE_DIR}/client/ui"
            --fixture streams
            --size 2048x1118
            --frames 5
            --screenshot "${CMAKE_BINARY_DIR}/ui-designer-stream-wide.bmp"
    )
    add_test(
        NAME parties_ui_designer_stream_narrow
        COMMAND rmlui_designer
            "${CMAKE_SOURCE_DIR}/client/ui/lobby.rml"
            --asset-dir "${CMAKE_SOURCE_DIR}/client/ui"
            --fixture streams
            --size 1190x1746
            --frames 5
            --screenshot "${CMAKE_BINARY_DIR}/ui-designer-stream-narrow.bmp"
    )
    add_test(
        NAME parties_ui_designer_stream_fps_overflow
        COMMAND rmlui_designer
            "${CMAKE_SOURCE_DIR}/client/ui/lobby.rml"
            --asset-dir "${CMAKE_SOURCE_DIR}/client/ui"
            --fixture stream-fps-overflow
            --size 1440x900
            --frames 5
            --screenshot "${CMAKE_BINARY_DIR}/ui-designer-stream-fps-overflow.bmp"
    )
endif()
