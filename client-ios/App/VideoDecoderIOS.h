#pragma once

#import <CoreMedia/CoreMedia.h>
#import <VideoToolbox/VideoToolbox.h>

#include <parties/video_common.h>

#include <functional>
#include <mutex>
#include <vector>

// VideoToolbox-based decoder for the screen share stream.
// Primary codec is AV1 (kCMVideoCodecType_AV1, iOS 16+, A14+ hardware).
// H265 and H264 are supported as fallbacks.
//
// Decoded frames arrive as CVPixelBufferRef in NV12
// (kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) on an internal VT thread.
// The caller must CFRetain/CFRelease the buffer if it needs to hold it.

class VideoDecoderIOS {
public:
    VideoDecoderIOS();
    ~VideoDecoderIOS();

    // Call once with codec + dimensions (known from SCREEN_SHARE_STARTED).
    // Returns false if VideoToolbox can't create a session (e.g. AV1 on <iOS16).
    bool init(parties::VideoCodecId codec, uint32_t width, uint32_t height);

    void shutdown();

    // Feed one encoded frame. is_keyframe should be set from VIDEO_FLAG_KEYFRAME.
    // For H264/H265 the session is created lazily on the first keyframe so that
    // parameter sets (SPS/PPS/VPS) can be extracted.
    void decode(const uint8_t* data, size_t len, bool is_keyframe);

    // Flush any buffered frames from the VT session.
    void flush();

    // Called on a VideoToolbox internal thread with a retained pixel buffer.
    // Receiver must CFRelease the buffer when done.
    std::function<void(CVPixelBufferRef)> on_decoded;

private:
    // AV1: format desc created from the first keyframe's Sequence Header OBU.
    bool setup_av1(const uint8_t* data, size_t len);

    // H264: parse SPS+PPS from an Annex-B keyframe, build format desc.
    bool setup_h264(const uint8_t* data, size_t len);

    // H265: parse VPS+SPS+PPS from an Annex-B keyframe, build format desc.
    bool setup_h265(const uint8_t* data, size_t len);

    // Create VTDecompressionSession from fmt_desc_.
    bool create_session();

    // Convert Annex-B stream to length-prefixed AVCC/HVCC, skipping param sets.
    static void annexb_to_avcc(const uint8_t* data, size_t len,
                                std::vector<uint8_t>& out,
                                parties::VideoCodecId codec);

    // VT output callback (static, dispatched back via on_decoded).
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

    std::mutex mutex_;
};
