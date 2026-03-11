#pragma once

// macOS video encoder — VideoToolbox VTCompressionSession.
// Encodes BGRA CVPixelBuffers (from ScreenCaptureKit) to H264 or H265
// bitstream (Annex-B, keyframe-prefixed with SPS/PPS).
//
// Replacement for the Windows NVENC / AMF / MFT encoders on macOS ARM.

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <cstdint>
#include <functional>
#include <mutex>

namespace parties::client {

enum class MacVideoCodec { H264, H265 };

class VideoEncoderMac {
public:
    VideoEncoderMac();
    ~VideoEncoderMac();

    // Initialize for the given codec, dimensions, and target bitrate (bits/s).
    bool init(MacVideoCodec codec, uint32_t width, uint32_t height,
              uint32_t bitrate_bps, uint32_t fps);

    void shutdown();

    // Encode one frame.  pixel_buffer must be BGRA or NV12.
    // is_keyframe = true forces an IDR (e.g. after a PLI request).
    void encode(CVPixelBufferRef pixel_buffer, bool force_keyframe = false);

    // Called from a VT internal thread with one complete NAL unit (Annex-B).
    // is_keyframe = true for IDR frames (prepended with SPS/PPS).
    std::function<void(const uint8_t* data, size_t len, bool is_keyframe)> on_encoded;

private:
    static void compress_callback(void*                refcon,
                                  void*                frameRefcon,
                                  OSStatus             status,
                                  VTEncodeInfoFlags    flags,
                                  CMSampleBufferRef    sample);

    void handle_encoded_sample(CMSampleBufferRef sample);

    VTCompressionSessionRef session_ = nullptr;
    MacVideoCodec           codec_   = MacVideoCodec::H264;
    uint32_t                width_   = 0;
    uint32_t                height_  = 0;
    int64_t                 pts_     = 0;
    std::mutex              mutex_;
};

} // namespace parties::client
