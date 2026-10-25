#include <encdec/apple/VideoDecoderIOS.h>

#include <parties/video_common.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>

#include <dav1d/dav1d.h>

#include <cstring>
#include <cstdio>

using parties::VideoCodecId;

static uint32_t read_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

static void write_be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8);
    p[3] = (uint8_t)(v);
}

VideoDecoderIOS::VideoDecoderIOS()  = default;

VideoDecoderIOS::~VideoDecoderIOS()
{
    shutdown();
}

bool VideoDecoderIOS::init(VideoCodecId codec, uint32_t width, uint32_t height)
{
    std::lock_guard<std::mutex> lock(mutex_);
    codec_  = codec;
    width_  = width;
    height_ = height;

    return true;
}

void VideoDecoderIOS::shutdown()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (session_) {
        VTDecompressionSessionInvalidate(session_);
        CFRelease(session_);
        session_ = nullptr;
    }
    if (fmt_desc_) {
        CFRelease(fmt_desc_);
        fmt_desc_ = nullptr;
    }
    if (dav1d_ctx_) {
        dav1d_flush(dav1d_ctx_);
        dav1d_close(&dav1d_ctx_);
        dav1d_ctx_ = nullptr;
    }
    use_dav1d_ = false;
    if (dav1d_pool_) {
        CVPixelBufferPoolFlush(dav1d_pool_, kCVPixelBufferPoolFlushExcessBuffers);
        CFRelease(dav1d_pool_);
        dav1d_pool_ = nullptr;
    }
    ready_ = false;
}

static std::pair<const uint8_t*, size_t>
find_obu(const uint8_t* data, size_t len, uint8_t target_type)
{
    size_t i = 0;
    while (i < len) {
        const uint8_t* obu_start = data + i;
        uint8_t header = data[i++];
        if (header >> 7) continue;
        uint8_t obu_type       = (header >> 3) & 0x0F;
        uint8_t extension_flag = (header >> 2) & 0x01;
        uint8_t has_size       = (header >> 1) & 0x01;
        if (extension_flag) { if (i >= len) break; i++; }

        size_t payload_size = 0;
        if (has_size) {
            for (int b = 0; b < 8 && i < len; b++) {
                uint8_t leb = data[i++];
                payload_size |= ((size_t)(leb & 0x7F)) << (b * 7);
                if (!(leb & 0x80)) break;
            }
        } else {
            payload_size = len - i;
        }

        if (obu_type == target_type) {
            size_t total = (size_t)(data + i + payload_size - obu_start);
            size_t avail = len - (size_t)(obu_start - data);
            return { obu_start, total < avail ? total : avail };
        }
        if (i + payload_size > len) break;
        i += payload_size;
    }
    return { nullptr, 0 };
}

bool VideoDecoderIOS::setup_av1(const uint8_t* data, size_t len)
{
    auto [seq_ptr, seq_len] = find_obu(data, len, 1);
    if (!seq_ptr || seq_len == 0) {
        fprintf(stderr, "[VideoDecoderIOS] AV1 keyframe missing Sequence Header OBU\n");
        return false;
    }
    fprintf(stderr, "[VideoDecoderIOS] AV1: Sequence Header OBU found (%zu bytes)\n", seq_len);

    uint8_t seq_profile = 0;
    {
        const uint8_t* p = seq_ptr;
        uint8_t obu_hdr = *p++;
        if ((obu_hdr >> 2) & 1) p++;
        if ((obu_hdr >> 1) & 1) {
            while (p < seq_ptr + seq_len && (*p & 0x80)) p++;
            if (p < seq_ptr + seq_len) p++;
        }
        if (p < seq_ptr + seq_len)
            seq_profile = (*p >> 5) & 0x07;
    }

    const uint8_t av1c_hdr[4] = { 0x81, (uint8_t)((seq_profile << 5) | 0x1F), 0x0C, 0x00 };
    std::vector<uint8_t> av1c;
    av1c.reserve(4 + seq_len);
    av1c.insert(av1c.end(), av1c_hdr, av1c_hdr + 4);
    av1c.insert(av1c.end(), seq_ptr, seq_ptr + seq_len);

    CFDataRef   av1c_data = CFDataCreate(nullptr, av1c.data(), (CFIndex)av1c.size());
    CFStringRef atom_key  = CFSTR("av1C");
    CFDictionaryRef atoms = CFDictionaryCreate(
        nullptr,
        (const void**)&atom_key, (const void**)&av1c_data, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(av1c_data);

    CFStringRef ext_key = kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms;
    CFDictionaryRef extensions = CFDictionaryCreate(
        nullptr,
        (const void**)&ext_key, (const void**)&atoms, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(atoms);

    OSStatus status = CMVideoFormatDescriptionCreate(
        kCFAllocatorDefault,
        kCMVideoCodecType_AV1,
        (int32_t)width_, (int32_t)height_,
        extensions,
        &fmt_desc_);
    CFRelease(extensions);

    if (status != noErr) {
        fprintf(stderr,
                "[VideoDecoderIOS] CMVideoFormatDescriptionCreate(AV1+av1C) failed: %d\n",
                (int)status);
        goto try_dav1d;
    }
    if (create_session()) return true;

try_dav1d:
    {
        Dav1dSettings settings;
        dav1d_default_settings(&settings);
        settings.n_threads = 4;
        settings.max_frame_delay = 1;
        if (dav1d_open(&dav1d_ctx_, &settings) == 0) {
            use_dav1d_ = true;
            ready_     = true;
            fprintf(stderr, "[VideoDecoderIOS] AV1: using dav1d software decoder\n");

            CFStringRef pb_keys[] = {
                kCVPixelBufferWidthKey,
                kCVPixelBufferHeightKey,
                kCVPixelBufferPixelFormatTypeKey,
                kCVPixelBufferMetalCompatibilityKey,
            };
            int w_val   = (int)width_;
            int h_val   = (int)height_;
            int fmt_val = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
            CFNumberRef w_num   = CFNumberCreate(nullptr, kCFNumberIntType, &w_val);
            CFNumberRef h_num   = CFNumberCreate(nullptr, kCFNumberIntType, &h_val);
            CFNumberRef fmt_num = CFNumberCreate(nullptr, kCFNumberIntType, &fmt_val);
            CFTypeRef pb_vals[] = { w_num, h_num, fmt_num, kCFBooleanTrue };
            CFDictionaryRef pb_attrs = CFDictionaryCreate(
                nullptr, (const void**)pb_keys, (const void**)pb_vals, 4,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFRelease(w_num); CFRelease(h_num); CFRelease(fmt_num);

            CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, pb_attrs, &dav1d_pool_);
            CFRelease(pb_attrs);
            if (!dav1d_pool_)
                fprintf(stderr, "[VideoDecoderIOS] CVPixelBufferPool creation failed; "
                                "falling back to per-frame allocation\n");
            return true;
        }
        fprintf(stderr, "[VideoDecoderIOS] AV1: dav1d_open failed\n");
        return false;
    }
}

static std::vector<std::pair<const uint8_t*, size_t>>
find_nal_units(const uint8_t* data, size_t len, const std::vector<uint8_t>& types)
{
    std::vector<std::pair<const uint8_t*, size_t>> result;
    size_t i = 0;
    while (i + 3 < len) {
        if (!(data[i] == 0 && data[i+1] == 0 &&
              (data[i+2] == 1 || (data[i+2] == 0 && i+3 < len && data[i+3] == 1))))
        {
            ++i;
            continue;
        }
        size_t sc_len = (data[i+2] == 1) ? 3 : 4;
        size_t nal_start = i + sc_len;
        if (nal_start >= len) break;

        uint8_t nal_type_byte = data[nal_start];

        size_t nal_end = len;
        for (size_t j = nal_start + 1; j + 2 < len; ++j) {
            if (data[j] == 0 && data[j+1] == 0 &&
                (data[j+2] == 1 || (data[j+2] == 0 && j+3 < len && data[j+3] == 1)))
            {
                nal_end = j;
                break;
            }
        }

        for (uint8_t t : types) {
            if (nal_type_byte == t) {
                result.push_back({ data + nal_start, nal_end - nal_start });
                break;
            }
        }
        i = nal_end;
    }
    return result;
}

bool VideoDecoderIOS::setup_h264(const uint8_t* data, size_t len)
{
    auto sps_units = find_nal_units(data, len, {0x67});
    auto pps_units = find_nal_units(data, len, {0x68});
    if (sps_units.empty() || pps_units.empty()) {
        fprintf(stderr, "[VideoDecoderIOS] H264 keyframe missing SPS/PPS\n");
        return false;
    }

    const uint8_t* param_ptrs[2] = { sps_units[0].first, pps_units[0].first };
    size_t         param_sizes[2] = { sps_units[0].second, pps_units[0].second };

    OSStatus status = CMVideoFormatDescriptionCreateFromH264ParameterSets(
        kCFAllocatorDefault,
        2,
        param_ptrs,
        param_sizes,
        4,
        &fmt_desc_);

    if (status != noErr) {
        fprintf(stderr, "[VideoDecoderIOS] CMVideoFormatDescriptionCreateFromH264ParameterSets failed: %d\n",
                (int)status);
        return false;
    }
    return create_session();
}

bool VideoDecoderIOS::setup_h265(const uint8_t* data, size_t len)
{
    auto vps_units = find_nal_units(data, len, {0x40});
    auto sps_units = find_nal_units(data, len, {0x42});
    auto pps_units = find_nal_units(data, len, {0x44});
    if (vps_units.empty() || sps_units.empty() || pps_units.empty()) {
        fprintf(stderr, "[VideoDecoderIOS] H265 keyframe missing VPS/SPS/PPS\n");
        return false;
    }

    const uint8_t* param_data[3] = {
        vps_units[0].first, sps_units[0].first, pps_units[0].first
    };
    size_t param_size[3] = {
        vps_units[0].second, sps_units[0].second, pps_units[0].second
    };

    OSStatus status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
        kCFAllocatorDefault,
        3, param_data, param_size,
        4,
        nullptr,
        &fmt_desc_);

    if (status != noErr) {
        fprintf(stderr, "[VideoDecoderIOS] CMVideoFormatDescriptionCreateFromHEVCParameterSets failed: %d\n",
                (int)status);
        return false;
    }
    return create_session();
}

bool VideoDecoderIOS::create_session()
{
    CFNumberRef pixel_fmt_num;
    OSType pixel_fmt = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    pixel_fmt_num = CFNumberCreate(nullptr, kCFNumberSInt32Type, &pixel_fmt);

    CFStringRef metal_key = kCVPixelBufferMetalCompatibilityKey;
    CFBooleanRef metal_val = kCFBooleanTrue;

    CFStringRef keys[2]   = { kCVPixelBufferPixelFormatTypeKey, metal_key };
    CFTypeRef   values[2] = { pixel_fmt_num, metal_val };

    CFDictionaryRef dest_attrs = CFDictionaryCreate(
        nullptr,
        (const void**)keys, (const void**)values, 2,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFRelease(pixel_fmt_num);

    VTDecompressionOutputCallbackRecord cb;
    cb.decompressionOutputCallback = &VideoDecoderIOS::decompress_callback;
    cb.decompressionOutputRefCon   = this;

    OSStatus status = VTDecompressionSessionCreate(
        kCFAllocatorDefault,
        fmt_desc_,
        nullptr,
        dest_attrs,
        &cb,
        &session_);

    CFRelease(dest_attrs);

    if (status != noErr) {
        fprintf(stderr, "[VideoDecoderIOS] VTDecompressionSessionCreate failed: %d\n",
                (int)status);
        return false;
    }

    ready_ = true;
    return true;
}

void VideoDecoderIOS::annexb_to_avcc(const uint8_t* data, size_t len,
                                     std::vector<uint8_t>& out,
                                     VideoCodecId codec)
{
    auto is_param_nal = [&](uint8_t first_byte) -> bool {
        if (codec == VideoCodecId::H264) {
            uint8_t t = first_byte & 0x1F;
            return (t == 7 || t == 8 || t == 6 || t == 9);
        } else {
            uint8_t t = (first_byte >> 1) & 0x3F;
            return (t == 32 || t == 33 || t == 34 || t == 35);
        }
    };

    size_t i = 0;
    while (i < len) {
        if (data[i] != 0) { ++i; continue; }

        if (i + 3 >= len) break;
        bool sc3 = (data[i] == 0 && data[i+1] == 0 && data[i+2] == 1);
        bool sc4 = (data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 &&
                    i+3 < len && data[i+3] == 1);
        if (!sc3 && !sc4) { ++i; continue; }

        size_t sc_len  = sc3 ? 3 : 4;
        size_t nal_start = i + sc_len;
        if (nal_start >= len) break;

        size_t nal_end = len;
        for (size_t j = nal_start + 1; j + 2 < len; ++j) {
            if (data[j] == 0 && data[j+1] == 0 &&
                (data[j+2] == 1 ||
                 (data[j+2] == 0 && j+3 < len && data[j+3] == 1)))
            {
                nal_end = j;
                break;
            }
        }

        size_t nal_len = nal_end - nal_start;

        if (!is_param_nal(data[nal_start])) {
            size_t off = out.size();
            out.resize(off + 4 + nal_len);
            write_be32(out.data() + off, (uint32_t)nal_len);
            memcpy(out.data() + off + 4, data + nal_start, nal_len);
        }

        i = nal_end;
    }
}

void VideoDecoderIOS::decode(const uint8_t* data, size_t len, bool is_keyframe)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!ready_) {
        if (!is_keyframe) {
            fprintf(stderr, "[VideoDecoderIOS] waiting for keyframe (not ready)\n");
            return;
        }

        fprintf(stderr, "[VideoDecoderIOS] setting up session for keyframe len=%zu\n", len);
        bool ok = false;
        if      (codec_ == VideoCodecId::H264) ok = setup_h264(data, len);
        else if (codec_ == VideoCodecId::H265) ok = setup_h265(data, len);
        else if (codec_ == VideoCodecId::AV1)  ok = setup_av1(data, len);
        fprintf(stderr, "[VideoDecoderIOS] session setup: %s\n", ok ? "OK" : "FAILED");
        if (!ok) return;
    }

    if (use_dav1d_) {
        decode_dav1d(data, len);
        return;
    }

    const uint8_t* feed_data = data;
    size_t         feed_len  = len;

    std::vector<uint8_t> avcc_buf;
    if (codec_ == VideoCodecId::H264 || codec_ == VideoCodecId::H265) {
        annexb_to_avcc(data, len, avcc_buf, codec_);
        if (avcc_buf.empty()) return;
        feed_data = avcc_buf.data();
        feed_len  = avcc_buf.size();
    }

    CMBlockBufferRef block = nullptr;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault,
        (void*)feed_data,
        feed_len,
        kCFAllocatorNull,
        nullptr,
        0, feed_len,
        0,
        &block);

    if (status != noErr || !block) {
        fprintf(stderr, "[VideoDecoderIOS] CMBlockBufferCreateWithMemoryBlock failed: %d\n",
                (int)status);
        return;
    }

    CMSampleBufferRef sample = nullptr;
    status = CMSampleBufferCreateReady(
        kCFAllocatorDefault,
        block,
        fmt_desc_,
        1,
        0,
        nullptr,
        0,
        nullptr,
        &sample);
    CFRelease(block);

    if (status != noErr || !sample) {
        fprintf(stderr, "[VideoDecoderIOS] CMSampleBufferCreateReady failed: %d\n",
                (int)status);
        return;
    }

    VTDecodeFrameFlags flags = kVTDecodeFrame_EnableAsynchronousDecompression;
    VTDecodeInfoFlags  info  = 0;
    VTDecompressionSessionDecodeFrame(session_, sample, flags, nullptr, &info);
    CFRelease(sample);
}

void VideoDecoderIOS::flush()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (session_) VTDecompressionSessionFinishDelayedFrames(session_);
    if (dav1d_ctx_) dav1d_flush(dav1d_ctx_);
}

void VideoDecoderIOS::decode_dav1d(const uint8_t* data, size_t len)
{
    Dav1dData dd = {};
    uint8_t* buf = dav1d_data_create(&dd, len);
    if (!buf) return;
    std::memcpy(buf, data, len);

    while (dd.sz > 0) {
        int ret = dav1d_send_data(dav1d_ctx_, &dd);
        if (ret < 0 && ret != DAV1D_ERR(EAGAIN)) {
            fprintf(stderr, "[VideoDecoderIOS] dav1d_send_data failed: %d\n", ret);
            dav1d_data_unref(&dd);
            return;
        }

        Dav1dPicture pic = {};
        while (dav1d_get_picture(dav1d_ctx_, &pic) == 0) {
            if (on_decoded) {
                bool is_10bit = (pic.p.bpc == 10);
                OSType pix_fmt = is_10bit
                    ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
                    : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;

                CVPixelBufferRef cvbuf = nullptr;
                CVReturn cv;
                if (!is_10bit && dav1d_pool_ &&
                    (uint32_t)pic.p.w == width_ && (uint32_t)pic.p.h == height_) {
                    cv = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault,
                                                            dav1d_pool_, &cvbuf);
                } else {
                    CFStringRef attrs_keys[1]   = { kCVPixelBufferMetalCompatibilityKey };
                    CFTypeRef   attrs_values[1] = { kCFBooleanTrue };
                    CFDictionaryRef attrs = CFDictionaryCreate(
                        nullptr,
                        (const void**)attrs_keys, (const void**)attrs_values, 1,
                        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                    cv = CVPixelBufferCreate(kCFAllocatorDefault,
                                            (size_t)pic.p.w, (size_t)pic.p.h,
                                            pix_fmt, attrs, &cvbuf);
                    CFRelease(attrs);
                }

                if (cv == kCVReturnSuccess && cvbuf) {
                    CVPixelBufferLockBaseAddress(cvbuf, 0);

                    int src_byte_per_pel = is_10bit ? 2 : 1;
                    int uv_w = pic.p.w / 2;
                    int uv_h = pic.p.h / 2;

                    uint8_t* dst_y  = (uint8_t*)CVPixelBufferGetBaseAddressOfPlane(cvbuf, 0);
                    size_t   dst_ys = CVPixelBufferGetBytesPerRowOfPlane(cvbuf, 0);
                    const uint8_t* src_y = (const uint8_t*)pic.data[0];
                    for (int row = 0; row < pic.p.h; row++)
                        std::memcpy(dst_y + row * dst_ys,
                                    src_y + row * pic.stride[0],
                                    (size_t)pic.p.w * src_byte_per_pel);

                    uint8_t* dst_uv  = (uint8_t*)CVPixelBufferGetBaseAddressOfPlane(cvbuf, 1);
                    size_t   dst_uvs = CVPixelBufferGetBytesPerRowOfPlane(cvbuf, 1);
                    const uint8_t* src_u = (const uint8_t*)pic.data[1];
                    const uint8_t* src_v = (const uint8_t*)pic.data[2];
                    for (int row = 0; row < uv_h; row++) {
                        uint8_t*       d  = dst_uv + row * dst_uvs;
                        const uint8_t* su = src_u  + row * pic.stride[1];
                        const uint8_t* sv = src_v  + row * pic.stride[1];
                        for (int col = 0; col < uv_w; col++) {
                            std::memcpy(d + col * 2 * src_byte_per_pel,
                                        su + col * src_byte_per_pel, src_byte_per_pel);
                            std::memcpy(d + col * 2 * src_byte_per_pel + src_byte_per_pel,
                                        sv + col * src_byte_per_pel, src_byte_per_pel);
                        }
                    }

                    CVPixelBufferUnlockBaseAddress(cvbuf, 0);
                    on_decoded(cvbuf);
                }
            }
            dav1d_picture_unref(&pic);
        }
    }
}

void VideoDecoderIOS::decompress_callback(void*           refcon,
                                          void*,
                                          OSStatus        status,
                                          VTDecodeInfoFlags,
                                          CVImageBufferRef image_buffer,
                                          CMTime,
                                          CMTime)
{
    if (status != noErr || !image_buffer) return;

    auto* self = static_cast<VideoDecoderIOS*>(refcon);
    if (!self->on_decoded) return;

    CFRetain(image_buffer);
    self->on_decoded((CVPixelBufferRef)image_buffer);
}
