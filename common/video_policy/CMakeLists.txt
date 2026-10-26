add_library(parties_video_policy STATIC
    src/video_common.cpp
    src/video_frame_reorder.cpp
    src/video_send_controller.cpp
)
set_target_properties(parties_video_policy PROPERTIES XCODE_ATTRIBUTE_SKIP_INSTALL "YES")

target_include_directories(parties_video_policy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
