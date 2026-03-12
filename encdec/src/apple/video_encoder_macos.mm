// macOS VideoToolbox encoder.
//
// Produces Annex-B H264 or H265 frames.  Keyframes include SPS/PPS/VPS
// in the same output buffer so the server and receiver can parse them.

#include <encdec/apple/video_encoder_macos.h>

#import <Foundation/Foundation.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace parties::client {

// ── Annex-B helpers ───────────────────────────────────────────────────────────

// Convert a CMSampleBuffer in AVCC/HVCC format to Annex-B.
// Parameter sets (SPS, PPS, VPS) from the format description are prepended
// before the first IDR NAL unit so the decoder always has them.
static std::vector<uint8_t> sample_to_annexb(CMSampleBufferRef sample,
                                              bool              is_keyframe,
                                              MacVideoCodec     codec)
{
    std::vector<uint8_t> out;

    CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sample);

    // Prepend parameter sets on keyframes.
    if (is_keyframe && fmt) {
        if (codec == MacVideoCodec::H264) {
            size_t offset = 0;
            while (true) {
                const uint8_t* ps    = nullptr;
                size_t         ps_sz = 0;
                OSStatus st = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    fmt, offset, &ps, &ps_sz, nullptr, nullptr);
                if (st != noErr) break;
                // Annex-B start code
                out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(1);
                out.insert(out.end(), ps, ps + ps_sz);
                ++offset;
            }
        } else { // H265
            size_t offset = 0;
            while (true) {
                const uint8_t* ps    = nullptr;
                size_t         ps_sz = 0;
                OSStatus st = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                    fmt, offset, &ps, &ps_sz, nullptr, nullptr);
                if (st != noErr) break;
                out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(1);
                out.insert(out.end(), ps, ps + ps_sz);
                ++offset;
            }
        }
    }

    // Convert AVCC/HVCC NAL units (4-byte length prefix) → Annex-B.
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    if (!block) return out;

    size_t   total   = CMBlockBufferGetDataLength(block);
    size_t   offset  = 0;
    while (offset + 4 <= total) {
        uint8_t len_bytes[4];
        CMBlockBufferCopyDataBytes(block, offset, 4, len_bytes);
        uint32_t nal_len = ((uint32_t)len_bytes[0] << 24)
                         | ((uint32_t)len_bytes[1] << 16)
                         | ((uint32_t)len_bytes[2] <<  8)
                         |  (uint32_t)len_bytes[3];
        offset += 4;
        if (offset + nal_len > total) break;

        out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(1);
        size_t base = out.size();
        out.resize(base + nal_len);
        CMBlockBufferCopyDataBytes(block, offset, nal_len, out.data() + base);
        offset += nal_len;
    }
    return out;
}

// ── VideoEncoderMac ───────────────────────────────────────────────────────────

VideoEncoderMac::VideoEncoderMac()  = default;
VideoEncoderMac::~VideoEncoderMac() { shutdown(); }

bool VideoEncoderMac::init(MacVideoCodec codec, uint32_t width, uint32_t height,
                            uint32_t bitrate_bps, uint32_t fps)
{
    std::lock_guard lock(mutex_);
    codec_  = codec;
    width_  = width;
    height_ = height;
    pts_    = 0;

    CMVideoCodecType vt_codec =
        (codec == MacVideoCodec::H265) ? kCMVideoCodecType_HEVC
                                       : kCMVideoCodecType_H264;

    // Encoder properties
    CFMutableDictionaryRef props =
        CFDictionaryCreateMutable(nullptr, 0,
                                  &kCFTypeDictionaryKeyCallBacks,
                                  &kCFTypeDictionaryValueCallBacks);

    // Hardware-accelerated on Apple Silicon
    CFDictionarySetValue(props,
                         kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
                         kCFBooleanTrue);

    // Pixel buffer format — BGRA is what ScreenCaptureKit delivers
    int32_t fmt_val = kCVPixelFormatType_32BGRA;
    CFNumberRef fmt_num = CFNumberCreate(nullptr, kCFNumberSInt32Type, &fmt_val);

    CFStringRef pb_keys[]   = { kCVPixelBufferPixelFormatTypeKey };
    CFTypeRef   pb_values[] = { fmt_num };
    CFDictionaryRef pb_attrs = CFDictionaryCreate(
        nullptr,
        (const void**)pb_keys, (const void**)pb_values, 1,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFRelease(fmt_num);

    OSStatus status = VTCompressionSessionCreate(
        kCFAllocatorDefault,
        (int32_t)width, (int32_t)height,
        vt_codec,
        props,
        pb_attrs,
        nullptr,
        compress_callback,
        this,
        &session_);

    CFRelease(props);
    CFRelease(pb_attrs);

    if (status != noErr) {
        fprintf(stderr, "[VideoEncoderMac] VTCompressionSessionCreate failed: %d\n",
                (int)status);
        return false;
    }

    // Bitrate — average target
    int32_t bps = (int32_t)bitrate_bps;
    CFNumberRef bps_num = CFNumberCreate(nullptr, kCFNumberSInt32Type, &bps);
    VTSessionSetProperty(session_,
                         kVTCompressionPropertyKey_AverageBitRate,
                         bps_num);
    CFRelease(bps_num);

    // Hard data rate limit: allow 1.5x the target over a 1-second window.
    // Without this, AverageBitRate is just a soft hint that VT freely exceeds.
    int32_t byte_limit = (int32_t)(bitrate_bps * 1.5 / 8);
    int32_t time_limit = 1; // seconds
    CFNumberRef limit_bytes = CFNumberCreate(nullptr, kCFNumberSInt32Type, &byte_limit);
    CFNumberRef limit_time  = CFNumberCreate(nullptr, kCFNumberSInt32Type, &time_limit);
    CFTypeRef limits[] = { limit_bytes, limit_time };
    CFArrayRef limit_array = CFArrayCreate(nullptr, limits, 2, &kCFTypeArrayCallBacks);
    VTSessionSetProperty(session_,
                         kVTCompressionPropertyKey_DataRateLimits,
                         limit_array);
    CFRelease(limit_array);
    CFRelease(limit_bytes);
    CFRelease(limit_time);

    // Frame rate
    int32_t fps_val = (int32_t)fps;
    CFNumberRef fps_num = CFNumberCreate(nullptr, kCFNumberSInt32Type, &fps_val);
    VTSessionSetProperty(session_,
                         kVTCompressionPropertyKey_ExpectedFrameRate,
                         fps_num);
    CFRelease(fps_num);

    // Keyframe every 3 seconds — limits P-frame quality degradation
    int32_t kf_interval = (int32_t)(fps * 3);
    CFNumberRef kf_num = CFNumberCreate(nullptr, kCFNumberSInt32Type, &kf_interval);
    VTSessionSetProperty(session_,
                         kVTCompressionPropertyKey_MaxKeyFrameInterval,
                         kf_num);
    CFRelease(kf_num);

    // Real-time encoding
    VTSessionSetProperty(session_,
                         kVTCompressionPropertyKey_RealTime,
                         kCFBooleanTrue);

    // Profile / level (H264: Baseline, H265: Main)
    if (codec == MacVideoCodec::H264) {
        VTSessionSetProperty(session_,
                             kVTCompressionPropertyKey_ProfileLevel,
                             kVTProfileLevel_H264_High_AutoLevel);
    } else {
        VTSessionSetProperty(session_,
                             kVTCompressionPropertyKey_ProfileLevel,
                             kVTProfileLevel_HEVC_Main_AutoLevel);
    }

    VTCompressionSessionPrepareToEncodeFrames(session_);
    fprintf(stderr, "[VideoEncoderMac] Initialized %s %ux%u @ %u bps %u fps\n",
            codec == MacVideoCodec::H265 ? "H265" : "H264",
            width, height, bitrate_bps, fps);
    return true;
}

void VideoEncoderMac::shutdown()
{
    std::lock_guard lock(mutex_);
    if (session_) {
        VTCompressionSessionInvalidate(session_);
        CFRelease(session_);
        session_ = nullptr;
    }
}

void VideoEncoderMac::encode(CVPixelBufferRef pixel_buffer, bool force_keyframe)
{
    std::lock_guard lock(mutex_);
    if (!session_ || !pixel_buffer) return;

    CMTime pts = CMTimeMake(pts_++, 1000);  // millisecond timebase

    CFMutableDictionaryRef frame_props = nullptr;
    if (force_keyframe) {
        frame_props = CFDictionaryCreateMutable(
            nullptr, 1,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(frame_props,
                             kVTEncodeFrameOptionKey_ForceKeyFrame,
                             kCFBooleanTrue);
    }

    VTEncodeInfoFlags info = 0;
    VTCompressionSessionEncodeFrame(
        session_, pixel_buffer,
        pts, kCMTimeInvalid,
        frame_props, nullptr, &info);

    if (frame_props) CFRelease(frame_props);
}

// ── VT callback ───────────────────────────────────────────────────────────────

void VideoEncoderMac::compress_callback(void*             refcon,
                                         void*             /*frameRefcon*/,
                                         OSStatus          status,
                                         VTEncodeInfoFlags /*flags*/,
                                         CMSampleBufferRef sample)
{
    if (status != noErr || !sample) return;
    static_cast<VideoEncoderMac*>(refcon)->handle_encoded_sample(sample);
}

void VideoEncoderMac::handle_encoded_sample(CMSampleBufferRef sample)
{
    if (!on_encoded) return;

    // Check if this is a keyframe.
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    bool is_keyframe = true;
    if (attachments && CFArrayGetCount(attachments) > 0) {
        CFDictionaryRef att = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        CFBooleanRef not_sync =
            (CFBooleanRef)CFDictionaryGetValue(att,
                kCMSampleAttachmentKey_NotSync);
        if (not_sync && CFBooleanGetValue(not_sync))
            is_keyframe = false;
    }

    std::vector<uint8_t> annexb = sample_to_annexb(sample, is_keyframe, codec_);
    if (!annexb.empty())
        on_encoded(annexb.data(), annexb.size(), is_keyframe);
}

} // namespace parties::client
