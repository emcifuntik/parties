#pragma once

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <cstdint>
#include <functional>
#include <mutex>

namespace parties::client {

enum class MacVideoCodec { H264, H265, AV1 };

class VideoEncoderMac {
public:
    VideoEncoderMac();
    ~VideoEncoderMac();

    bool init(MacVideoCodec codec, uint32_t width, uint32_t height,
              uint32_t bitrate_bps, uint32_t fps);

    void shutdown();

    void set_bitrate(uint32_t bitrate_bps);

    void encode(CVPixelBufferRef pixel_buffer, bool force_keyframe = false);

    std::function<void(const uint8_t* data, size_t len, bool is_keyframe)> on_encoded;

    // The codec actually in use after init(). May differ from the requested
    // codec: Apple Silicon has no AV1 encoder, so AV1 falls back to H265.
    // Callers must read this (not the requested codec) when tagging the wire
    // format, or the receiver will try to decode with the wrong codec.
    MacVideoCodec actual_codec() const { return codec_; }

private:
    static void compress_callback(void*                refcon,
                                  void*                frameRefcon,
                                  OSStatus             status,
                                  VTEncodeInfoFlags    flags,
                                  CMSampleBufferRef    sample);

    void handle_encoded_sample(CMSampleBufferRef sample);

    void apply_rate_limits(uint32_t bitrate_bps);

    VTCompressionSessionRef session_ = nullptr;
    MacVideoCodec           codec_   = MacVideoCodec::H264;
    uint32_t                width_   = 0;
    uint32_t                height_  = 0;
    uint32_t                fps_     = 30;
    int64_t                 pts_     = 0;
    std::mutex              mutex_;
};

}
