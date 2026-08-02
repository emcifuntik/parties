find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(toml11 CONFIG REQUIRED)
find_package(RapidJSON CONFIG REQUIRED)
if(BUILD_CLIENT)
    find_package(RmlUi CONFIG REQUIRED)
endif()
set_target_properties(toml11::toml11 PROPERTIES INTERFACE_COMPILE_OPTIONS "")

add_executable(parties_integration_test
    integration_test.cpp
    ../server/src/server.cpp
    ../server/src/quic_server.cpp
    ../server/src/database.cpp
    ../server/src/config.cpp
    ../client/src/net_client_msquic.cpp
)

target_include_directories(parties_integration_test
    PRIVATE
        ../server/include
        ../client/include
)

target_link_libraries(parties_integration_test
    PRIVATE
        parties_common
        unofficial::sqlite3::sqlite3
        toml11::toml11
)

if(WIN32)
    target_link_libraries(parties_integration_test PRIVATE ncrypt.lib crypt32.lib)
endif()

# ── Voice packet-loss resilience (offline, deterministic) ─────────────────────
find_package(Opus CONFIG REQUIRED)
find_path(SPEEXDSP_INCLUDE_DIRS "speex/speex_echo.h")
find_library(SPEEXDSP_LIBRARY speexdsp)

add_executable(parties_voice_loss_test
    voice_loss_test.cpp
    ../client/src/voice_mixer.cpp
)

target_include_directories(parties_voice_loss_test
    PRIVATE
        ../client/include
)

target_link_libraries(parties_voice_loss_test
    PRIVATE
        parties_common
        Opus::opus
)

# ── Secondary/auxiliary audio stream (offline, deterministic) ─────────────────
add_executable(parties_aux_stream_test
    aux_stream_test.cpp
    ../client/src/voice_mixer.cpp
)

target_include_directories(parties_aux_stream_test
    PRIVATE
        ../client/include
)

target_link_libraries(parties_aux_stream_test
    PRIVATE
        parties_common
        Opus::opus
)

# ── RmlUi client tests (offline, deterministic) ─────────────────────────────
if(BUILD_CLIENT)
    add_executable(parties_ui_stylesheet_test ui_stylesheet_test.cpp)
    target_link_libraries(parties_ui_stylesheet_test PRIVATE RmlUi::RmlUi)

    add_executable(parties_document_router_test document_router_test.cpp)
    target_include_directories(parties_document_router_test PRIVATE ../client/include)
    target_link_libraries(parties_document_router_test PRIVATE RmlUi::RmlUi)

    add_executable(parties_context_render_manager_lifecycle_test
        context_render_manager_lifecycle_test.cpp)
    target_link_libraries(parties_context_render_manager_lifecycle_test PRIVATE RmlUi::RmlUi)
endif()

add_executable(parties_server_query_wake_test server_query_wake_test.cpp)
target_include_directories(parties_server_query_wake_test PRIVATE ../client/include)

add_executable(parties_video_decode_gate_test video_decode_gate_test.cpp)
target_include_directories(parties_video_decode_gate_test PRIVATE ../client/include)

add_executable(parties_video_rate_control_test video_rate_control_test.cpp)
target_include_directories(parties_video_rate_control_test PRIVATE ../encdec/include)

add_executable(parties_application_audio_test
    application_audio_test.cpp
    ../client/src/application_audio.cpp
)
target_include_directories(parties_application_audio_test PRIVATE ../client/include)

add_executable(parties_echo_cancellation_test
    echo_cancellation_test.cpp
    ../client/src/echo_canceller.cpp
)
target_include_directories(parties_echo_cancellation_test PRIVATE
    ../client/include
    ${SPEEXDSP_INCLUDE_DIRS})
target_link_libraries(parties_echo_cancellation_test PRIVATE
    parties_common
    ${SPEEXDSP_LIBRARY})

if(WIN32)
    find_path(AMD_AMF_INCLUDE_DIRS "AMF/core/Result.h" REQUIRED)
    add_executable(parties_amf_decode_state_test amf_decode_state_test.cpp)
    target_include_directories(parties_amf_decode_state_test PRIVATE
        ../encdec/src
        ${AMD_AMF_INCLUDE_DIRS})

    add_executable(parties_amf_decoder_hardware_test amf_decoder_hardware_test.cpp)
    target_include_directories(parties_amf_decoder_hardware_test PRIVATE
        ../encdec/src
        ../common/include
        ${AMD_AMF_INCLUDE_DIRS})
    target_link_libraries(parties_amf_decoder_hardware_test PRIVATE
        parties_encdec
        d3d11
        d3d12
        dxgi)

    add_executable(parties_nvdec_d3d12_interop_test nvdec_d3d12_interop_test.cpp)
    target_include_directories(parties_nvdec_d3d12_interop_test PRIVATE
        ../encdec/src
        ../common/include)
    target_link_libraries(parties_nvdec_d3d12_interop_test PRIVATE
        parties_encdec d3d11 d3d12 dxgi)

    add_executable(parties_nvdec_sequence_policy_test nvdec_sequence_policy_test.cpp)
    target_include_directories(parties_nvdec_sequence_policy_test PRIVATE ../encdec/src)

    add_executable(parties_dx12_video_converter_test dx12_video_converter_test.cpp)
    target_include_directories(parties_dx12_video_converter_test PRIVATE ../client/src)
    target_link_libraries(parties_dx12_video_converter_test PRIVATE
        parties_rmlui_dx12_backend d3d12 dxgi)

    add_executable(parties_context_window_lifecycle_test context_window_lifecycle_test.cpp)
    target_include_directories(parties_context_window_lifecycle_test PRIVATE ../client/include)
    target_link_libraries(parties_context_window_lifecycle_test PRIVATE user32)

    add_executable(parties_window_thumbnail_test
        window_thumbnail_test.cpp
        ../client/src/windows/screen_capture.cpp
    )
    target_include_directories(parties_window_thumbnail_test PRIVATE ../client/include)
    target_link_libraries(parties_window_thumbnail_test PRIVATE
        parties_common d3d11 dxgi windowsapp dwmapi gdi32 user32)

    add_executable(parties_rmlui_dx12_multiwindow_test
        rmlui_dx12_multiwindow_test.cpp)
    target_link_libraries(parties_rmlui_dx12_multiwindow_test PRIVATE
        parties_rmlui_dx12_backend user32)
endif()

add_test(NAME parties_voice_loss COMMAND parties_voice_loss_test)
add_test(NAME parties_aux_stream COMMAND parties_aux_stream_test)
if(BUILD_CLIENT)
    add_test(NAME parties_ui_stylesheet_variables COMMAND parties_ui_stylesheet_test)
    add_test(NAME parties_document_router COMMAND parties_document_router_test)
    add_test(NAME parties_context_render_manager_lifecycle COMMAND parties_context_render_manager_lifecycle_test)
endif()
add_test(NAME parties_server_query_wake COMMAND parties_server_query_wake_test)
add_test(NAME parties_video_decode_gate COMMAND parties_video_decode_gate_test)
add_test(NAME parties_video_rate_control COMMAND parties_video_rate_control_test)
add_test(NAME parties_application_audio_downmix COMMAND parties_application_audio_test)
add_test(NAME parties_echo_cancellation COMMAND parties_echo_cancellation_test)
if(WIN32)
    add_test(NAME parties_amf_decode_state COMMAND parties_amf_decode_state_test)
    add_test(NAME parties_amf_decoder_hardware COMMAND parties_amf_decoder_hardware_test)
    set_tests_properties(parties_amf_decoder_hardware PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME parties_nvdec_d3d12_interop COMMAND parties_nvdec_d3d12_interop_test)
    set_tests_properties(parties_nvdec_d3d12_interop PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_test(NAME parties_nvdec_d3d12_first_frame COMMAND parties_nvdec_d3d12_interop_test 1)
    set_tests_properties(parties_nvdec_d3d12_first_frame PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_test(NAME parties_nvdec_sequence_policy COMMAND parties_nvdec_sequence_policy_test)
    add_test(NAME parties_dx12_video_converter COMMAND parties_dx12_video_converter_test)
    set_tests_properties(parties_dx12_video_converter PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_test(NAME parties_context_window_lifecycle COMMAND parties_context_window_lifecycle_test)
    add_test(NAME parties_window_thumbnail COMMAND parties_window_thumbnail_test)
    add_test(NAME parties_rmlui_dx12_multiwindow COMMAND parties_rmlui_dx12_multiwindow_test)
    set_tests_properties(parties_rmlui_dx12_multiwindow PROPERTIES TIMEOUT 30)
endif()
