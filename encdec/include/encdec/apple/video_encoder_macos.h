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

enum class MacVideoCodec { H264, H265, AV1 };

class VideoEncoderMac {
public:
    VideoEncoderMac();
    ~VideoEncoderMac();

    // Initialize for the given codec, dimensions, and target bitrate (bits/s).
    bool init(MacVideoCodec codec, uint32_t width, uint32_t height,
              uint32_t bitrate_bps, uint32_t fps);

    void shutdown();

    // Change the average bitrate (and the matching hard data-rate window) of a
    // running session. Driven by the sharer's congestion controller
    // (AppCore::take_video_bitrate_update) from the capture callback.
    void set_bitrate(uint32_t bitrate_bps);

    // Encode one frame.  pixel_buffer must be BGRA or NV12.
    // is_keyframe = true forces an IDR (e.g. after a PLI request).
    void encode(CVPixelBufferRef pixel_buffer, bool force_keyframe = false);

    // Called from a VT internal thread with one complete NAL unit (Annex-B).
    // is_keyframe = true for IDR frames (prepended with SPS/PPS).
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

    // Applies AverageBitRate + DataRateLimits for `bitrate_bps` to session_.
    // Caller holds mutex_.
    void apply_rate_limits(uint32_t bitrate_bps);

    VTCompressionSessionRef session_ = nullptr;
    MacVideoCodec           codec_   = MacVideoCodec::H264;
    uint32_t                width_   = 0;
    uint32_t                height_  = 0;
    uint32_t                fps_     = 30;
    int64_t                 pts_     = 0;
    std::mutex              mutex_;
};

} // namespace parties::client
