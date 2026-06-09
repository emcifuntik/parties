#include "mft_decoder.h"

#include <mfidl.h>
#include <mferror.h>

#include <cstring>
#include <parties/log.h>
#include <parties/profiler.h>

using Microsoft::WRL::ComPtr;

namespace parties::encdec::mft {

MftDecoder::~MftDecoder() {
    if (mft_) {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        mft_.Reset();
    }
    if (mf_started_)
        MFShutdown();
}

bool MftDecoder::init(VideoCodecId codec, uint32_t width, uint32_t height) {
    // MFT decoder handles H.264 and H.265 only
    if (codec == VideoCodecId::AV1)
        return false;

    codec_ = codec;
    width_ = width;
    height_ = height;

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        LOG_ERROR("MFStartup failed: {:#010x}", hr);
        return false;
    }
    mf_started_ = true;

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
        LOG_ERROR("No {} decoder found", name);
        if (activates) CoTaskMemFree(activates);
        return false;
    }

    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&mft_));
    for (UINT32 i = 0; i < count; i++) activates[i]->Release();
    CoTaskMemFree(activates);

    if (FAILED(hr)) {
        LOG_ERROR("Activate {} decoder failed: {:#010x}", name, hr);
        return false;
    }

    // Set input type (compressed)
    ComPtr<IMFMediaType> in_type;
    MFCreateMediaType(&in_type);
    in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in_type->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, width, height);

    hr = mft_->SetInputType(0, in_type.Get(), 0);
    if (FAILED(hr)) {
        LOG_ERROR("SetInputType failed: {:#010x}", hr);
        return false;
    }

    // Set output type: enumerate available and pick NV12
    bool output_set = false;
    for (DWORD i = 0; !output_set; i++) {
        ComPtr<IMFMediaType> avail;
        hr = mft_->GetOutputAvailableType(0, i, &avail);
        if (FAILED(hr)) break;

        GUID out_sub;
        avail->GetGUID(MF_MT_SUBTYPE, &out_sub);
        if (out_sub == MFVideoFormat_NV12) {
            hr = mft_->SetOutputType(0, avail.Get(), 0);
            if (SUCCEEDED(hr)) {
                UINT32 default_stride = 0;
                if (SUCCEEDED(avail->GetUINT32(MF_MT_DEFAULT_STRIDE, &default_stride)))
                    nv12_stride_ = default_stride;
                output_set = true;
            }
        }
    }

    if (!output_set) {
        LOG_ERROR("No NV12 output type available");
        return false;
    }

    // Query output stream info
    MFT_OUTPUT_STREAM_INFO stream_info;
    hr = mft_->GetOutputStreamInfo(0, &stream_info);
    if (SUCCEEDED(hr)) {
        provides_samples_ =
            (stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        out_buf_size_ = stream_info.cbSize;
        if (out_buf_size_ == 0)
            out_buf_size_ = width * height * 3 / 2;
    }

    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    initialized_ = true;
    return true;
}

void MftDecoder::collect_output() {
    ZoneScopedN("MftDecoder::collect_output");
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER output = {};
        ComPtr<IMFSample> out_sample;

        if (!provides_samples_) {
            ComPtr<IMFMediaBuffer> out_buf;
            MFCreateMemoryBuffer(out_buf_size_, &out_buf);
            MFCreateSample(&out_sample);
            out_sample->AddBuffer(out_buf.Get());
            output.pSample = out_sample.Get();
        }

        DWORD status = 0;
        HRESULT hr = mft_->ProcessOutput(0, 1, &output, &status);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
            break;

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (output.pEvents) output.pEvents->Release();
            // Re-negotiate output type after format change
            for (DWORD i = 0; ; i++) {
                ComPtr<IMFMediaType> avail;
                if (FAILED(mft_->GetOutputAvailableType(0, i, &avail))) break;
                GUID sub;
                avail->GetGUID(MF_MT_SUBTYPE, &sub);
                if (sub == MFVideoFormat_NV12) {
                    mft_->SetOutputType(0, avail.Get(), 0);
                    MFGetAttributeSize(avail.Get(), MF_MT_FRAME_SIZE,
                                       &width_, &height_);
                    UINT32 default_stride = 0;
                    if (SUCCEEDED(avail->GetUINT32(MF_MT_DEFAULT_STRIDE, &default_stride)))
                        nv12_stride_ = default_stride;
                    else
                        nv12_stride_ = 0;
                    break;
                }
            }
            MFT_OUTPUT_STREAM_INFO info;
            if (SUCCEEDED(mft_->GetOutputStreamInfo(0, &info)))
                out_buf_size_ = info.cbSize ? info.cbSize
                                             : width_ * height_ * 3 / 2;
            continue;
        }

        if (FAILED(hr)) break;

        IMFSample* result = provides_samples_
                                ? output.pSample : out_sample.Get();
        if (result && on_decoded) {
            ComPtr<IMFMediaBuffer> buf;
            result->ConvertToContiguousBuffer(&buf);
            if (buf) {
                BYTE* raw = nullptr;
                DWORD raw_len = 0;
                if (SUCCEEDED(buf->Lock(&raw, nullptr, &raw_len))) {
                    ZoneScopedN("NV12_to_I420");
                    uint32_t stride = nv12_stride_;
                    if (stride == 0) stride = width_;

                    // Infer stride from buffer size if it doesn't match expected
                    uint32_t expected_tight = width_ * height_ * 3 / 2;
                    if (raw_len > expected_tight && stride == width_) {
                        stride = raw_len * 2 / (height_ * 3);
                    }

                    uint32_t half_w = width_ / 2;
                    uint32_t half_h = height_ / 2;
                    uint32_t uv_stride = stride;
                    uint32_t y_size = width_ * height_;
                    uint32_t uv_size = half_w * half_h;

                    i420_buffer_.resize(y_size + uv_size * 2);

                    // Y plane: copy with stride
                    uint8_t* y_dst = i420_buffer_.data();
                    for (uint32_t row = 0; row < height_; row++)
                        std::memcpy(y_dst + row * width_, raw + row * stride, width_);

                    // Deinterleave NV12 UV -> I420 U + V
                    uint8_t* u_dst = i420_buffer_.data() + y_size;
                    uint8_t* v_dst = u_dst + uv_size;
                    const uint8_t* nv12_uv_base = raw + stride * height_;
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
                    f.y_plane   = i420_buffer_.data();
                    f.u_plane   = u_dst;
                    f.v_plane   = v_dst;
                    f.y_stride  = width_;
                    f.uv_stride = half_w;
                    f.width     = width_;
                    f.height    = height_;
                    f.timestamp = ts;
                    on_decoded(f);
                }
            }
        }

        if (output.pEvents) output.pEvents->Release();
        // When the MFT allocates its own output samples, ownership transfers
        // to us on ProcessOutput success — release or it leaks per frame.
        if (provides_samples_ && output.pSample) output.pSample->Release();
    }
}

bool MftDecoder::decode(const uint8_t* data, size_t len, int64_t timestamp) {
    ZoneScopedN("MftDecoder::decode");
    if (!initialized_) return false;

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

    HRESULT hr = mft_->ProcessInput(0, in_sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        collect_output();
        hr = mft_->ProcessInput(0, in_sample.Get(), 0);
    }
    if (FAILED(hr) && hr != MF_E_NOTACCEPTING) return false;

    collect_output();
    return true;
}

void MftDecoder::flush() {
    ZoneScopedN("MftDecoder::flush");
    if (!initialized_) return;
    mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    collect_output();
}

DecoderInfo MftDecoder::info() const {
    return {Backend::MFT, codec_};
}

} // namespace parties::encdec::mft
