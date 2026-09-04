// macOS VideoToolbox encoder.
//
// Produces Annex-B H264 or H265 frames.  Keyframes include SPS/PPS/VPS
// in the same output buffer so the server and receiver can parse them.

#include <encdec/apple/video_encoder_macos.h>
#include <parties/video_common.h>

#import <Foundation/Foundation.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace parties::client {

// ── Annex-B helpers ───────────────────────────────────────────────────────────

// Convert a CMSampleBuffer to the wire format.
// H264/H265: AVCC/HVCC → Annex-B with parameter sets prepended on keyframes.
// AV1: raw OBU output from VideoToolbox, passed through as-is.
static std::vector<uint8_t> sample_to_wire(CMSampleBufferRef sample,
                                            bool              is_keyframe,
                                            MacVideoCodec     codec)
{
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    if (!block) return {};

    size_t total = CMBlockBufferGetDataLength(block);

    // AV1: VideoToolbox outputs raw OBUs — no length-prefix conversion needed
    if (codec == MacVideoCodec::AV1) {
        std::vector<uint8_t> out(total);
        CMBlockBufferCopyDataBytes(block, 0, total, out.data());
        return out;
    }

    // H264/H265: convert AVCC/HVCC (4-byte length prefix) → Annex-B
    std::vector<uint8_t> out;
    CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sample);

    // Prepend parameter sets on keyframes
    if (is_keyframe && fmt) {
        if (codec == MacVideoCodec::H264) {
            size_t idx = 0;
            while (true) {
                const uint8_t* ps = nullptr; size_t ps_sz = 0;
                if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                        fmt, idx, &ps, &ps_sz, nullptr, nullptr) != noErr) break;
                out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(1);
                out.insert(out.end(), ps, ps + ps_sz);
                ++idx;
            }
        } else {
            size_t idx = 0;
            while (true) {
                const uint8_t* ps = nullptr; size_t ps_sz = 0;
                if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                        fmt, idx, &ps, &ps_sz, nullptr, nullptr) != noErr) break;
                out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(1);
                out.insert(out.end(), ps, ps + ps_sz);
                ++idx;
            }
        }
    }

    // NAL units: 4-byte length prefix → Annex-B start code
    size_t offset = 0;
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
    fps_    = fps > 0 ? fps : 30;
    pts_    = 0;

    CMVideoCodecType vt_codec;
    if (codec == MacVideoCodec::AV1) {
        if (@available(macOS 14.0, *))
            vt_codec = kCMVideoCodecType_AV1;
        else
            vt_codec = kCMVideoCodecType_HEVC; // fallback on older macOS
    } else if (codec == MacVideoCodec::H265) {
        vt_codec = kCMVideoCodecType_HEVC;
    } else {
        vt_codec = kCMVideoCodecType_H264;
    }

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

    // Apple Silicon (through M4) has no AV1 hardware encoder and VideoToolbox
    // ships no software one, so an AV1 compression session fails with
    // kVTCouldNotFindVideoEncoderErr (-12908). Fall back to HEVC, which every
    // Apple Silicon GPU encodes in hardware. codec_ is updated so the emitted
    // NAL parsing (handle_encoded_sample) and the wire codec (actual_codec(),
    // read by the caller) match what we actually produce.
    if (status != noErr && codec == MacVideoCodec::AV1) {
        fprintf(stderr, "[VideoEncoderMac] AV1 encode unavailable (%d); falling back to HEVC\n",
                (int)status);
        codec_   = MacVideoCodec::H265;
        vt_codec = kCMVideoCodecType_HEVC;
        status = VTCompressionSessionCreate(
            kCFAllocatorDefault,
            (int32_t)width, (int32_t)height,
            vt_codec,
            props,
            pb_attrs,
            nullptr,
            compress_callback,
            this,
            &session_);
    }

    CFRelease(props);
    CFRelease(pb_attrs);

    if (status != noErr) {
        fprintf(stderr, "[VideoEncoderMac] VTCompressionSessionCreate failed: %d\n",
                (int)status);
        return false;
    }

    // Bitrate — average target + hard data-rate window (shared with set_bitrate)
    apply_rate_limits(bitrate_bps);

    // Frame rate
    int32_t fps_val = (int32_t)fps;
    CFNumberRef fps_num = CFNumberCreate(nullptr, kCFNumberSInt32Type, &fps_val);
    VTSessionSetProperty(session_,
                         kVTCompressionPropertyKey_ExpectedFrameRate,
                         fps_num);
    CFRelease(fps_num);

    // Periodic keyframes at the shared VIDEO_KEYFRAME_INTERVAL_MS cadence, the
    // same GOP length the Windows encoders use, so late joiners never wait
    // longer than that for a sync point.
    int32_t kf_interval = (int32_t)(fps_ * (parties::VIDEO_KEYFRAME_INTERVAL_MS / 1000));
    CFNumberRef kf_num = CFNumberCreate(nullptr, kCFNumberSInt32Type, &kf_interval);
    VTSessionSetProperty(session_,
                         kVTCompressionPropertyKey_MaxKeyFrameInterval,
                         kf_num);
    CFRelease(kf_num);

    // Real-time encoding
    VTSessionSetProperty(session_,
                         kVTCompressionPropertyKey_RealTime,
                         kCFBooleanTrue);

    // Do not let the encoder hold frames back for lookahead: every submitted
    // frame is emitted as soon as it is encoded (one-in, one-out).
    int32_t max_frame_delay = 0;
    CFNumberRef delay_num = CFNumberCreate(nullptr, kCFNumberSInt32Type, &max_frame_delay);
    VTSessionSetProperty(session_,
                         kVTCompressionPropertyKey_MaxFrameDelayCount,
                         delay_num);
    CFRelease(delay_num);

    // Profile / level (H264: High, H265: Main)
    if (codec == MacVideoCodec::H264) {
        VTSessionSetProperty(session_,
                             kVTCompressionPropertyKey_ProfileLevel,
                             kVTProfileLevel_H264_High_AutoLevel);
        // High profile supports CABAC — set it explicitly for ~10% better
        // compression efficiency than CAVLC at the same bitrate.
        VTSessionSetProperty(session_,
                             kVTCompressionPropertyKey_H264EntropyMode,
                             kVTH264EntropyMode_CABAC);
    } else {
        VTSessionSetProperty(session_,
                             kVTCompressionPropertyKey_ProfileLevel,
                             kVTProfileLevel_HEVC_Main_AutoLevel);
    }

    // Low-latency: no B-frames / reordering. RealTime already implies this,
    // but set it explicitly so the decoder never waits on out-of-order frames.
    VTSessionSetProperty(session_,
                         kVTCompressionPropertyKey_AllowFrameReordering,
                         kCFBooleanFalse);

    // Colour signalling (BT.709). The source is sRGB BGRA; without tagging the
    // bitstream the decoder guesses the matrix/range and colours come out
    // washed or over-saturated — a big part of the "looks bad" report. We tag
    // 709 (the HD standard) so VideoToolbox uses the 709 RGB→YCbCr matrix on
    // encode and the receiver inverts it correctly.
    VTSessionSetProperty(session_, kVTCompressionPropertyKey_ColorPrimaries,
                         kCVImageBufferColorPrimaries_ITU_R_709_2);
    VTSessionSetProperty(session_, kVTCompressionPropertyKey_TransferFunction,
                         kCVImageBufferTransferFunction_ITU_R_709_2);
    VTSessionSetProperty(session_, kVTCompressionPropertyKey_YCbCrMatrix,
                         kCVImageBufferYCbCrMatrix_ITU_R_709_2);

    VTCompressionSessionPrepareToEncodeFrames(session_);
    const char* codec_name = codec_ == MacVideoCodec::AV1  ? "AV1"
                           : codec_ == MacVideoCodec::H265 ? "H265" : "H264";
    fprintf(stderr, "[VideoEncoderMac] Initialized %s %ux%u @ %u bps %u fps\n",
            codec_name, width, height, bitrate_bps, fps);
    return true;
}

void VideoEncoderMac::apply_rate_limits(uint32_t bitrate_bps)
{
    if (!session_ || bitrate_bps == 0) return;

    // Average target
    int32_t bps = (int32_t)bitrate_bps;
    CFNumberRef bps_num = CFNumberCreate(nullptr, kCFNumberSInt32Type, &bps);
    VTSessionSetProperty(session_,
                         kVTCompressionPropertyKey_AverageBitRate,
                         bps_num);
    CFRelease(bps_num);

    // Hard data rate limit: 1.5x the target (the stream peak) over a 0.5 s
    // window. Without this, AverageBitRate is just a soft hint that VT freely
    // exceeds. The window is real-time only because we feed real host-clock
    // PTS (see encode()). A short window at peak keeps a keyframe from turning
    // into a multi-second burst on a link close to the average, which every
    // viewer experiences as a stall followed by catch-up. The window length
    // must be a double-typed CFNumber (seconds); an integer 0 would disable it.
    const double  window_seconds = 0.5;
    const SInt32  byte_limit     = (SInt32)(bitrate_bps * 1.5 / 8 * window_seconds);
    CFNumberRef limit_bytes = CFNumberCreate(nullptr, kCFNumberSInt32Type, &byte_limit);
    CFNumberRef limit_time  = CFNumberCreate(nullptr, kCFNumberDoubleType, &window_seconds);
    CFTypeRef limits[] = { limit_bytes, limit_time };
    CFArrayRef limit_array = CFArrayCreate(nullptr, limits, 2, &kCFTypeArrayCallBacks);
    VTSessionSetProperty(session_,
                         kVTCompressionPropertyKey_DataRateLimits,
                         limit_array);
    CFRelease(limit_array);
    CFRelease(limit_bytes);
    CFRelease(limit_time);
}

void VideoEncoderMac::set_bitrate(uint32_t bitrate_bps)
{
    std::lock_guard lock(mutex_);
    apply_rate_limits(bitrate_bps);
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

    // Real-time presentation timestamp from the host clock. The previous scheme
    // (pts_++ at a 1000-tick timebase) advanced PTS by exactly 1 ms per frame,
    // telling VideoToolbox the stream ran at ~1000 fps regardless of the real
    // rate. That mismatch with ExpectedFrameRate corrupted rate-control pacing
    // and made the DataRateLimits window meaningless. Host-clock PTS gives VT
    // accurate inter-frame timing so it allocates bits per real second.
    CMTime pts = CMClockGetTime(CMClockGetHostTimeClock());
    CMTime dur = fps_ > 0 ? CMTimeMake(1, (int32_t)fps_) : kCMTimeInvalid;
    pts_++;  // retained only for diagnostics

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
        pts, dur,
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

    std::vector<uint8_t> wire = sample_to_wire(sample, is_keyframe, codec_);
    if (!wire.empty())
        on_encoded(wire.data(), wire.size(), is_keyframe);
}

} // namespace parties::client
