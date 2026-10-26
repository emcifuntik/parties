#pragma once

#import <CoreMedia/CoreMedia.h>
#import <VideoToolbox/VideoToolbox.h>

#include <parties/video_common.h>

#include <functional>
#include <mutex>
#include <vector>

struct Dav1dContext;

class VideoDecoderIOS {
public:
    VideoDecoderIOS();
    ~VideoDecoderIOS();

    bool init(parties::VideoCodecId codec, uint32_t width, uint32_t height);

    void shutdown();

    void decode(const uint8_t* data, size_t len, bool is_keyframe);

    void flush();

    // Called on a VideoToolbox internal thread with a retained pixel buffer.
    // Receiver must CFRelease the buffer when done.
    std::function<void(CVPixelBufferRef)> on_decoded;

private:
    bool setup_av1(const uint8_t* data, size_t len);

    void decode_dav1d(const uint8_t* data, size_t len);

    bool setup_h264(const uint8_t* data, size_t len);

    bool setup_h265(const uint8_t* data, size_t len);

    bool create_session();

    static void annexb_to_avcc(const uint8_t* data, size_t len,
                                std::vector<uint8_t>& out,
                                parties::VideoCodecId codec);

    static void decompress_callback(void* refcon,
                                    void* frameRefcon,
                                    OSStatus status,
                                    VTDecodeInfoFlags flags,
                                    CVImageBufferRef image_buffer,
                                    CMTime pts,
                                    CMTime dur);

    parties::VideoCodecId codec_  = parties::VideoCodecId::AV1;
    uint32_t              width_  = 0;
    uint32_t              height_ = 0;

    CMVideoFormatDescriptionRef fmt_desc_ = nullptr;
    VTDecompressionSessionRef   session_  = nullptr;
    bool                        ready_    = false;

    Dav1dContext*        dav1d_ctx_  = nullptr;
    bool                 use_dav1d_  = false;
    CVPixelBufferPoolRef dav1d_pool_ = nullptr;

    std::mutex mutex_;
};
