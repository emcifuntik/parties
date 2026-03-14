find_package(dav1d CONFIG REQUIRED)

if(WIN32)
    find_package(libhevc CONFIG REQUIRED)
    find_path(OPENH264_INCLUDE_DIRS "wels/codec_api.h")
    find_library(OPENH264_LIBRARY openh264)
endif()

if(WIN32)
    set(ENCDEC_PLATFORM_SOURCES
        src/factory.cpp
        src/nvidia/nvidia_loader.cpp
        src/nvidia/nvenc_encoder.cpp
        src/nvidia/nvdec_decoder.cpp
        src/amd/amf_loader.cpp
        src/amd/amf_encoder.cpp
        src/amd/amf_decoder.cpp
        src/mft/mft_encoder.cpp
        src/mft/mft_decoder.cpp
        src/dav1d/dav1d_decoder.cpp
        src/openh264/openh264_encoder.cpp
        src/openh264/openh264_decoder.cpp
        src/libhevc/libhevc_decoder.cpp
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(ENCDEC_PLATFORM_SOURCES
        src/apple/VideoDecoderIOS.mm
    )
elseif(APPLE)
    set(ENCDEC_PLATFORM_SOURCES
        src/apple/VideoDecoderIOS.mm
        src/apple/video_encoder_macos.mm
    )
endif()

add_library(parties_encdec STATIC
    ${ENCDEC_PLATFORM_SOURCES}
)
set_target_properties(parties_encdec PROPERTIES XCODE_ATTRIBUTE_SKIP_INSTALL "YES")

target_include_directories(parties_encdec
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(WIN32)
    target_include_directories(parties_encdec PRIVATE ${OPENH264_INCLUDE_DIRS})
endif()

target_link_libraries(parties_encdec
    PRIVATE
        parties_common
        dav1d::dav1d
)

if(WIN32)
    target_link_libraries(parties_encdec PRIVATE
        libhevc::decoder
        ${OPENH264_LIBRARY}
        d3d11 mfplat mfuuid mf strmiids
    )
elseif(APPLE)
    target_link_libraries(parties_encdec PRIVATE
        "-framework VideoToolbox"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework Foundation"
    )
endif()
