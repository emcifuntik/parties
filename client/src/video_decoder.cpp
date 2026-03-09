#include <client/video_decoder.h>
#include "nvidia/nvdec_decoder.h"

#include <cstdio>
#include <cstring>
#include <parties/profiler.h>
#include <vector>

// AV1 decoder
#include <dav1d/dav1d.h>

// MFT decoder for H.264/H.265
#include <mfapi.h>
#include <mftransform.h>
#include <mfidl.h>
#include <mferror.h>
#include <wrl/client.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace parties::client {

struct VideoDecoder::Impl {
    // AV1
    Dav1dContext* dav1d_ctx = nullptr;

    // H.264/H.265
    ComPtr<IMFTransform> mft;
    bool mft_provides_samples = false;
    DWORD mft_out_buf_size = 0;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t nv12_stride = 0;   // Actual row stride from MFT (may be > width)

    // NV12 -> I420 conversion buffer
    std::vector<uint8_t> i420_buffer;

    // Drain all available dav1d pictures and deliver via callback
    void drain_dav1d(const std::function<void(const DecodedFrame&)>& cb) {
        ZoneScopedN("VideoDecoder::drain_dav1d");
        Dav1dPicture pic = {};
        while (dav1d_get_picture(dav1d_ctx, &pic) == 0) {
            if (cb) {
                DecodedFrame f{};
                f.y_plane   = static_cast<const uint8_t*>(pic.data[0]);
                f.u_plane   = static_cast<const uint8_t*>(pic.data[1]);
                f.v_plane   = static_cast<const uint8_t*>(pic.data[2]);
                f.y_stride  = static_cast<uint32_t>(pic.stride[0]);
                f.uv_stride = static_cast<uint32_t>(pic.stride[1]);
                f.width     = static_cast<uint32_t>(pic.p.w);
                f.height    = static_cast<uint32_t>(pic.p.h);
                f.timestamp = pic.m.timestamp;
                cb(f);
            }
            dav1d_picture_unref(&pic);
        }
    }

    // Collect all available MFT output, convert NV12 -> I420, deliver
    void collect_mft(const std::function<void(const DecodedFrame&)>& cb) {
        ZoneScopedN("VideoDecoder::collect_mft");
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER output = {};
            ComPtr<IMFSample> out_sample;

            if (!mft_provides_samples) {
                ComPtr<IMFMediaBuffer> out_buf;
                MFCreateMemoryBuffer(mft_out_buf_size, &out_buf);
                MFCreateSample(&out_sample);
                out_sample->AddBuffer(out_buf.Get());
                output.pSample = out_sample.Get();
            }

            DWORD status = 0;
            HRESULT hr = mft->ProcessOutput(0, 1, &output, &status);

            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
                break;

            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                // Re-negotiate output type after format change
                for (DWORD i = 0; ; i++) {
                    ComPtr<IMFMediaType> avail;
                    if (FAILED(mft->GetOutputAvailableType(0, i, &avail))) break;
                    GUID sub;
                    avail->GetGUID(MF_MT_SUBTYPE, &sub);
                    if (sub == MFVideoFormat_NV12) {
                        mft->SetOutputType(0, avail.Get(), 0);
                        MFGetAttributeSize(avail.Get(), MF_MT_FRAME_SIZE,
                                           &width, &height);
                        UINT32 default_stride = 0;
                        if (SUCCEEDED(avail->GetUINT32(MF_MT_DEFAULT_STRIDE, &default_stride)))
                            nv12_stride = default_stride;
                        else
                            nv12_stride = 0;
                        break;
                    }
                }
                MFT_OUTPUT_STREAM_INFO info;
                if (SUCCEEDED(mft->GetOutputStreamInfo(0, &info)))
                    mft_out_buf_size = info.cbSize ? info.cbSize
                                                   : width * height * 3 / 2;
                continue;
            }

            if (FAILED(hr)) break;

            IMFSample* result = mft_provides_samples
                                    ? output.pSample : out_sample.Get();
            if (result && cb) {
                ComPtr<IMFMediaBuffer> buf;
                result->ConvertToContiguousBuffer(&buf);
                if (buf) {
                    BYTE* raw = nullptr;
                    DWORD raw_len = 0;
                    if (SUCCEEDED(buf->Lock(&raw, nullptr, &raw_len))) {
                        ZoneScopedN("NV12_to_I420");
                        uint32_t stride = nv12_stride;
                        if (stride == 0) stride = width;

                        // Infer stride from buffer size if it doesn't match expected
                        uint32_t expected_tight = width * height * 3 / 2;
                        if (raw_len > expected_tight && stride == width) {
                            // Buffer is larger than tight packing — stride must be wider
                            stride = raw_len * 2 / (height * 3);
                        }

                        uint32_t half_w = width / 2;
                        uint32_t half_h = height / 2;
                        uint32_t uv_stride = stride;  // NV12 UV rows have same stride as Y
                        uint32_t y_size = width * height;
                        uint32_t uv_size = half_w * half_h;

                        i420_buffer.resize(y_size + uv_size * 2);

                        // Y plane: copy with stride (skip padding bytes per row)
                        uint8_t* y_dst = i420_buffer.data();
                        for (uint32_t row = 0; row < height; row++)
                            std::memcpy(y_dst + row * width, raw + row * stride, width);

                        // Deinterleave NV12 UV -> I420 U + V (stride-aware)
                        uint8_t* u_dst = i420_buffer.data() + y_size;
                        uint8_t* v_dst = u_dst + uv_size;
                        const uint8_t* nv12_uv_base = raw + stride * height;
                        for (uint32_t row = 0; row < half_h; row++) {
                            const uint8_t* uv_row = nv12_uv_base + row * uv_stride;
                            for (uint32_t x = 0; x < half_w; x++) {
                                u_dst[row * half_w + x] = uv_row[x * 2];
                                v_dst[row * half_w + x] = uv_row[x * 2 + 1];
                            }
                        }

                        buf->Unlock();

                        int64_t ts = 0;
                        result->GetSampleTime(&ts);

                        DecodedFrame f{};
                        f.y_plane   = i420_buffer.data();
                        f.u_plane   = u_dst;
                        f.v_plane   = v_dst;
                        f.y_stride  = width;
                        f.uv_stride = half_w;
                        f.width     = width;
                        f.height    = height;
                        f.timestamp = ts;
                        cb(f);
                    }
                }
            }

            if (output.pEvents) output.pEvents->Release();
        }
    }
};

VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() { shutdown(); }

bool VideoDecoder::init(VideoCodecId codec, uint32_t width, uint32_t height) {
	ZoneScopedN("VideoDecoder::init");
    shutdown();

    impl_ = std::make_unique<Impl>();
    impl_->width = width;
    impl_->height = height;
    codec_ = codec;

    // Try NVDEC first for all codecs (unless hardware was disabled after context loss)
    if (!hardware_disabled_) {
        auto nvdec = std::make_unique<nvidia::NvdecDecoder>();
        if (nvdec->init(codec, width, height)) {
            nvdec_ = std::move(nvdec);
            initialized_ = true;
            return true;
        }
    }

    if (codec == VideoCodecId::AV1) {
        // Fall back to dav1d for AV1
        Dav1dSettings settings;
        dav1d_default_settings(&settings);
        settings.n_threads = 4;
        settings.max_frame_delay = 1;

        int ret = dav1d_open(&impl_->dav1d_ctx, &settings);
        if (ret < 0) {
            std::fprintf(stderr, "[VideoDecoder] dav1d_open failed: %d\n", ret);
            return false;
        }
    } else {
        // H.264 or H.265 via Media Foundation Transform
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(hr)) {
            std::fprintf(stderr, "[VideoDecoder] MFStartup failed: 0x%08lx\n", hr);
            return false;
        }

        GUID subtype = (codec == VideoCodecId::H265) ? MFVideoFormat_HEVC
                                                      : MFVideoFormat_H264;
        const char* name = (codec == VideoCodecId::H265) ? "H.265" : "H.264";

        // Find a synchronous decoder
        MFT_REGISTER_TYPE_INFO input_info = { MFMediaType_Video, subtype };
        IMFActivate** activates = nullptr;
        UINT32 count = 0;

        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                       MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                       &input_info, nullptr, &activates, &count);

        if (FAILED(hr) || count == 0) {
            std::fprintf(stderr, "[VideoDecoder] No %s decoder found\n", name);
            if (activates) CoTaskMemFree(activates);
            return false;
        }

        hr = activates[0]->ActivateObject(IID_PPV_ARGS(&impl_->mft));
        for (UINT32 i = 0; i < count; i++) activates[i]->Release();
        CoTaskMemFree(activates);

        if (FAILED(hr)) {
            std::fprintf(stderr,
                "[VideoDecoder] Activate %s decoder failed: 0x%08lx\n", name, hr);
            return false;
        }

        // Set input type (compressed)
        ComPtr<IMFMediaType> in_type;
        MFCreateMediaType(&in_type);
        in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in_type->SetGUID(MF_MT_SUBTYPE, subtype);
        MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, width, height);

        hr = impl_->mft->SetInputType(0, in_type.Get(), 0);
        if (FAILED(hr)) {
            std::fprintf(stderr,
                "[VideoDecoder] SetInputType failed: 0x%08lx\n", hr);
            return false;
        }

        // Set output type: enumerate available and pick NV12
        bool output_set = false;
        for (DWORD i = 0; !output_set; i++) {
            ComPtr<IMFMediaType> avail;
            hr = impl_->mft->GetOutputAvailableType(0, i, &avail);
            if (FAILED(hr)) break;

            GUID out_sub;
            avail->GetGUID(MF_MT_SUBTYPE, &out_sub);
            if (out_sub == MFVideoFormat_NV12) {
                hr = impl_->mft->SetOutputType(0, avail.Get(), 0);
                if (SUCCEEDED(hr)) {
                    // Query actual stride (may differ from width on some hardware)
                    UINT32 default_stride = 0;
                    if (SUCCEEDED(avail->GetUINT32(MF_MT_DEFAULT_STRIDE, &default_stride)))
                        impl_->nv12_stride = default_stride;
                    output_set = true;
                }
            }
        }

        if (!output_set) {
            std::fprintf(stderr,
                "[VideoDecoder] No NV12 output type available\n");
            return false;
        }

        // Query output stream info
        MFT_OUTPUT_STREAM_INFO stream_info;
        hr = impl_->mft->GetOutputStreamInfo(0, &stream_info);
        if (SUCCEEDED(hr)) {
            impl_->mft_provides_samples =
                (stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
            impl_->mft_out_buf_size = stream_info.cbSize;
            if (impl_->mft_out_buf_size == 0)
                impl_->mft_out_buf_size = width * height * 3 / 2;
        }

        impl_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        impl_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }

    initialized_ = true;
    return true;
}

void VideoDecoder::shutdown() {
    if (nvdec_) {
        nvdec_->shutdown();
        nvdec_.reset();
        initialized_ = false;
        return;
    }

    if (!impl_) return;

    if (impl_->dav1d_ctx) {
        dav1d_flush(impl_->dav1d_ctx);
        dav1d_close(&impl_->dav1d_ctx);
    }

    if (impl_->mft) {
        impl_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        impl_->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        impl_->mft.Reset();
    }

    impl_.reset();
    initialized_ = false;
}

bool VideoDecoder::decode(const uint8_t* data, size_t len, int64_t timestamp) {
	ZoneScopedN("VideoDecoder::decode");
    if (!initialized_) return false;

    if (nvdec_) {
        nvdec_->on_decoded = on_decoded;
        return nvdec_->decode(data, len, timestamp);
    }

    if (!impl_) return false;

    if (codec_ == VideoCodecId::AV1) {
        ZoneScopedN("decode::dav1d");
        Dav1dData dav1d_data = {};
        uint8_t* buf = dav1d_data_create(&dav1d_data, len);
        if (!buf) return false;
        std::memcpy(buf, data, len);
        dav1d_data.m.timestamp = timestamp;

        while (dav1d_data.sz > 0) {
            int ret = dav1d_send_data(impl_->dav1d_ctx, &dav1d_data);
            if (ret < 0 && ret != DAV1D_ERR(EAGAIN)) {
                std::fprintf(stderr, "[VideoDecoder] dav1d_send_data failed: %d (len=%zu)\n", ret, len);
                dav1d_data_unref(&dav1d_data);
                return false;
            }
            impl_->drain_dav1d(on_decoded);
        }
        return true;
    } else {
        ZoneScopedN("decode::mft");
        ComPtr<IMFMediaBuffer> in_buf;
        MFCreateMemoryBuffer(static_cast<DWORD>(len), &in_buf);

        BYTE* ptr = nullptr;
        in_buf->Lock(&ptr, nullptr, nullptr);
        std::memcpy(ptr, data, len);
        in_buf->Unlock();
        in_buf->SetCurrentLength(static_cast<DWORD>(len));

        ComPtr<IMFSample> in_sample;
        MFCreateSample(&in_sample);
        in_sample->AddBuffer(in_buf.Get());
        in_sample->SetSampleTime(timestamp);

        HRESULT hr = impl_->mft->ProcessInput(0, in_sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            // MFT is full — drain output first, then retry
            impl_->collect_mft(on_decoded);
            hr = impl_->mft->ProcessInput(0, in_sample.Get(), 0);
        }
        if (FAILED(hr) && hr != MF_E_NOTACCEPTING) return false;

        impl_->collect_mft(on_decoded);
        return true;
    }
}

void VideoDecoder::flush() {
	ZoneScopedN("VideoDecoder::flush");
    if (!initialized_) return;

    if (nvdec_) {
        nvdec_->on_decoded = on_decoded;
        nvdec_->flush();
        return;
    }

    if (!impl_) return;

    if (codec_ == VideoCodecId::AV1) {
        impl_->drain_dav1d(on_decoded);
    } else {
        impl_->mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        impl_->collect_mft(on_decoded);
    }
}

bool VideoDecoder::context_lost() const {
    return nvdec_ && nvdec_->context_lost();
}

} // namespace parties::client
