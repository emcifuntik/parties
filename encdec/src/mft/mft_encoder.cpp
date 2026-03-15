#include "mft_encoder.h"

#include <mferror.h>
#include <codecapi.h>
#include <strmif.h>

#include <cstdio>
#include <parties/profiler.h>
#include <parties/video_common.h>

using Microsoft::WRL::ComPtr;

namespace parties::encdec::mft {

MftEncoder::~MftEncoder() {
    if (!initialized_) return;

    encoder_running_ = false;
    queue_cv_.notify_all();

    if (encoder_) {
        encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    }

    if (encoder_thread_.joinable())
        encoder_thread_.join();

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        while (!input_queue_.empty()) input_queue_.pop();
    }

    encoder_events_.Reset();
    encoder_.Reset();
    converter_.Reset();
    dxgi_manager_.Reset();
    context_.Reset();
    device_.Reset();

    MFShutdown();
}

bool MftEncoder::init(ID3D11Device* device, uint32_t width, uint32_t height,
                       uint32_t input_width, uint32_t input_height,
                       uint32_t fps, uint32_t bitrate, VideoCodecId preferred_codec) {
    device_ = device;
    device_->GetImmediateContext(&context_);
    fps_ = fps;

    if (input_width == 0) input_width = width;
    if (input_height == 0) input_height = height;

    // NV12 requires even dimensions
    width = (width + 1) & ~1u;
    height = (height + 1) & ~1u;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[MftEncoder] MFStartup failed: 0x%08lx\n", hr);
        return false;
    }

    // Create DXGI device manager
    hr = MFCreateDXGIDeviceManager(&dxgi_reset_token_, &dxgi_manager_);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[MftEncoder] MFCreateDXGIDeviceManager failed\n");
        return false;
    }
    hr = dxgi_manager_->ResetDevice(device_.Get(), dxgi_reset_token_);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[MftEncoder] ResetDevice failed\n");
        return false;
    }

    // Try preferred codec first, then fall back to others
    struct CodecEntry { GUID subtype; VideoCodecId id; };
    CodecEntry all_codecs[] = {
        {MFVideoFormat_AV1,  VideoCodecId::AV1},
        {MFVideoFormat_HEVC, VideoCodecId::H265},
        {MFVideoFormat_H264, VideoCodecId::H264},
    };

    bool found = false;

    for (auto& c : all_codecs) {
        if (c.id == preferred_codec) {
            found = try_create_encoder(c.subtype, c.id);
            break;
        }
    }

    if (!found) {
        for (auto& c : all_codecs) {
            if (c.id != preferred_codec) {
                found = try_create_encoder(c.subtype, c.id);
                if (found) break;
            }
        }
    }

    if (!found) {
        std::fprintf(stderr, "[MftEncoder] No hardware encoder found\n");
        return false;
    }

    if (!configure_encoder(width, height, fps, bitrate)) {
        std::fprintf(stderr, "[MftEncoder] Failed to configure encoder\n");
        return false;
    }

    if (!create_color_converter(input_width, input_height, width, height)) {
        std::fprintf(stderr, "[MftEncoder] Failed to create color converter\n");
        return false;
    }

    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    MFT_OUTPUT_STREAM_INFO stream_info = {};
    if (SUCCEEDED(encoder_->GetOutputStreamInfo(0, &stream_info)))
        encoder_provides_samples_ = (stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;

    if (async_mode_ && encoder_events_) {
        encoder_running_ = true;
        encoder_thread_ = std::thread(&MftEncoder::encoder_loop, this);
    }

    width_ = width;
    height_ = height;
    initialized_ = true;
    return true;
}

bool MftEncoder::try_create_encoder(const GUID& codec_subtype, VideoCodecId id) {
    ZoneScopedN("MftEncoder::try_create_encoder");
    MFT_REGISTER_TYPE_INFO output_info = {};
    output_info.guidMajorType = MFMediaType_Video;
    output_info.guidSubtype = codec_subtype;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;

    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags,
                            nullptr, &output_info, &activates, &count);
    if (FAILED(hr) || count == 0) return false;

    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&encoder_));
    for (UINT32 i = 0; i < count; i++) activates[i]->Release();
    CoTaskMemFree(activates);

    if (FAILED(hr)) {
        std::fprintf(stderr, "[MftEncoder] ActivateObject failed: 0x%08lx\n", hr);
        return false;
    }

    // Check async mode
    ComPtr<IMFAttributes> attrs;
    hr = encoder_->GetAttributes(&attrs);

    UINT32 is_async = FALSE;
    if (attrs) attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);

    if (is_async && attrs) {
        hr = attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        if (SUCCEEDED(hr)) {
            hr = encoder_.As(&encoder_events_);
            if (SUCCEEDED(hr) && encoder_events_)
                async_mode_ = true;
        }
    }

    if (!async_mode_) encoder_events_.Reset();

    codec_ = id;
    return true;
}

bool MftEncoder::configure_encoder(uint32_t width, uint32_t height,
                                    uint32_t fps, uint32_t bitrate) {
    GUID subtype;
    switch (codec_) {
        case VideoCodecId::AV1:  subtype = MFVideoFormat_AV1;  break;
        case VideoCodecId::H265: subtype = MFVideoFormat_HEVC; break;
        case VideoCodecId::H264: subtype = MFVideoFormat_H264; break;
    }

    // Set D3D11 device manager
    HRESULT hr = encoder_->ProcessMessage(
        MFT_MESSAGE_SET_D3D_MANAGER,
        reinterpret_cast<ULONG_PTR>(dxgi_manager_.Get()));
    if (FAILED(hr))
        std::fprintf(stderr, "[MftEncoder] Failed to set D3D manager: 0x%08lx\n", hr);

    // Output type (must be set BEFORE input type)
    ComPtr<IMFMediaType> output_type;
    MFCreateMediaType(&output_type);
    output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    output_type->SetGUID(MF_MT_SUBTYPE, subtype);
    output_type->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    MFSetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(output_type.Get(), MF_MT_FRAME_RATE, fps, 1);
    output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    hr = encoder_->SetOutputType(0, output_type.Get(), 0);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[MftEncoder] SetOutputType failed: 0x%08lx\n", hr);
        return false;
    }

    // Input type: NV12
    ComPtr<IMFMediaType> input_type;
    MFCreateMediaType(&input_type);
    input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE, fps, 1);
    input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    hr = encoder_->SetInputType(0, input_type.Get(), 0);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[MftEncoder] SetInputType failed: 0x%08lx\n", hr);
        return false;
    }

    // Configure codec properties via ICodecAPI
    ComPtr<ICodecAPI> codec_api;
    if (SUCCEEDED(encoder_.As(&codec_api))) {
        VARIANT var;

        VariantInit(&var);
        var.vt = VT_BOOL;
        var.boolVal = VARIANT_TRUE;
        codec_api->SetValue(&CODECAPI_AVLowLatencyMode, &var);

        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = eAVEncCommonRateControlMode_PeakConstrainedVBR;
        codec_api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);

        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = bitrate;
        codec_api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);

        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = bitrate * 5;
        codec_api->SetValue(&CODECAPI_AVEncCommonMaxBitRate, &var);

        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = fps * (VIDEO_KEYFRAME_INTERVAL_MS / 1000);
        codec_api->SetValue(&CODECAPI_AVEncMPVGOPSize, &var);

        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = 0;
        codec_api->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &var);

        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = 25;
        codec_api->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &var);

        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = 1;
        codec_api->SetValue(&CODECAPI_VideoEncoderDisplayContentType, &var);
    }

    return true;
}

bool MftEncoder::create_color_converter(uint32_t in_w, uint32_t in_h,
                                         uint32_t out_w, uint32_t out_h) {
    HRESULT hr = CoCreateInstance(
        CLSID_VideoProcessorMFT, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&converter_));
    if (FAILED(hr)) {
        std::fprintf(stderr, "[MftEncoder] CoCreateInstance VideoProcessorMFT failed: 0x%08lx\n", hr);
        return false;
    }

    converter_->ProcessMessage(
        MFT_MESSAGE_SET_D3D_MANAGER,
        reinterpret_cast<ULONG_PTR>(dxgi_manager_.Get()));

    // Input: BGRA
    ComPtr<IMFMediaType> in_type;
    MFCreateMediaType(&in_type);
    in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
    MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, in_w, in_h);

    hr = converter_->SetInputType(0, in_type.Get(), 0);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[MftEncoder] Converter SetInputType failed: 0x%08lx\n", hr);
        return false;
    }

    // Output: NV12
    ComPtr<IMFMediaType> out_type;
    MFCreateMediaType(&out_type);
    out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(out_type.Get(), MF_MT_FRAME_SIZE, out_w, out_h);

    hr = converter_->SetOutputType(0, out_type.Get(), 0);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[MftEncoder] Converter SetOutputType failed: 0x%08lx\n", hr);
        return false;
    }

    return true;
}

void MftEncoder::encoder_loop() {
    TracySetThreadName("MftEncoder");
    while (encoder_running_) {
        ZoneScopedN("MftEncoder::encoder_loop");
        ComPtr<IMFMediaEvent> event;
        HRESULT hr = encoder_events_->GetEvent(0, &event);
        if (FAILED(hr)) break;

        MediaEventType event_type;
        event->GetType(&event_type);

        if (event_type == METransformNeedInput) {
            ComPtr<IMFSample> sample;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait(lock, [this] {
                    return !input_queue_.empty() || !encoder_running_;
                });
                if (!encoder_running_) break;
                sample = input_queue_.front();
                input_queue_.pop();
            }

            if (force_keyframe_) {
                force_keyframe_ = false;
                ComPtr<ICodecAPI> codec_api;
                if (SUCCEEDED(encoder_.As(&codec_api))) {
                    VARIANT var;
                    VariantInit(&var);
                    var.vt = VT_UI4;
                    var.ulVal = 1;
                    codec_api->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &var);
                }
            }

            hr = encoder_->ProcessInput(0, sample.Get(), 0);
            if (FAILED(hr))
                std::fprintf(stderr, "[MftEncoder] ProcessInput failed: 0x%08lx\n", hr);
        } else if (event_type == METransformHaveOutput) {
            collect_output();
        } else if (event_type == METransformDrainComplete) {
            break;
        }
    }
}

bool MftEncoder::encode(ID3D11Texture2D* bgra_texture, int64_t timestamp_100ns) {
    ZoneScopedN("MftEncoder::encode");
    if (!initialized_) return false;

    int64_t duration = 10'000'000LL / fps_;

    // Wrap BGRA texture into MF sample
    ComPtr<IMFMediaBuffer> bgra_buffer;
    HRESULT hr = MFCreateDXGISurfaceBuffer(
        __uuidof(ID3D11Texture2D), bgra_texture, 0, FALSE, &bgra_buffer);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[MftEncoder] MFCreateDXGISurfaceBuffer failed: 0x%08lx\n", hr);
        return false;
    }

    ComPtr<IMFSample> bgra_sample;
    MFCreateSample(&bgra_sample);
    bgra_sample->AddBuffer(bgra_buffer.Get());
    bgra_sample->SetSampleTime(timestamp_100ns);
    bgra_sample->SetSampleDuration(duration);

    // BGRA → NV12 via Video Processor MFT
    hr = converter_->ProcessInput(0, bgra_sample.Get(), 0);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[MftEncoder] Converter ProcessInput failed: 0x%08lx\n", hr);
        return false;
    }

    MFT_OUTPUT_DATA_BUFFER conv_output = {};
    DWORD conv_status = 0;
    hr = converter_->ProcessOutput(0, 1, &conv_output, &conv_status);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[MftEncoder] Converter ProcessOutput failed: 0x%08lx\n", hr);
        return false;
    }

    ComPtr<IMFSample> nv12_sample;
    nv12_sample.Attach(conv_output.pSample);
    if (!nv12_sample) {
        std::fprintf(stderr, "[MftEncoder] Converter produced null sample\n");
        return false;
    }

    nv12_sample->SetSampleTime(timestamp_100ns);
    nv12_sample->SetSampleDuration(duration);

    // Feed NV12 to encoder
    if (async_mode_) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            while (input_queue_.size() >= 2)
                input_queue_.pop();
            input_queue_.push(nv12_sample);
        }
        queue_cv_.notify_one();
    } else {
        if (force_keyframe_) {
            force_keyframe_ = false;
            ComPtr<ICodecAPI> codec_api;
            if (SUCCEEDED(encoder_.As(&codec_api))) {
                VARIANT var;
                VariantInit(&var);
                var.vt = VT_UI4;
                var.ulVal = 1;
                codec_api->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &var);
            }
        }

        hr = encoder_->ProcessInput(0, nv12_sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            collect_output();
            hr = encoder_->ProcessInput(0, nv12_sample.Get(), 0);
        }
        if (FAILED(hr)) {
            std::fprintf(stderr, "[MftEncoder] ProcessInput failed: 0x%08lx\n", hr);
            return false;
        }
        collect_output();
    }

    return true;
}

bool MftEncoder::collect_output() {
    ZoneScopedN("MftEncoder::collect_output");
    int collected = 0;
    while (true) {
        MFT_OUTPUT_DATA_BUFFER output_data = {};
        DWORD status = 0;
        HRESULT hr = encoder_->ProcessOutput(0, 1, &output_data, &status);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
            break;
        if (FAILED(hr))
            break;

        if (output_data.pSample) {
            ComPtr<IMFMediaBuffer> buf;
            output_data.pSample->ConvertToContiguousBuffer(&buf);

            BYTE* data = nullptr;
            DWORD len = 0;
            buf->Lock(&data, nullptr, &len);

            bool keyframe = false;
            UINT32 clean_point = 0;
            if (SUCCEEDED(output_data.pSample->GetUINT32(
                    MFSampleExtension_CleanPoint, &clean_point)))
                keyframe = (clean_point != 0);

            if (on_encoded && data && len > 0)
                on_encoded(data, len, keyframe);

            buf->Unlock();
            collected++;

            if (encoder_provides_samples_)
                output_data.pSample->Release();
        }
    }

    return collected > 0;
}

void MftEncoder::force_keyframe() {
    force_keyframe_ = true;
}

void MftEncoder::set_bitrate(uint32_t bitrate) {
    if (!initialized_) return;

    ComPtr<ICodecAPI> codec_api;
    if (SUCCEEDED(encoder_.As(&codec_api))) {
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = bitrate;
        codec_api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);
    }
}

EncoderInfo MftEncoder::info() const {
    return {Backend::MFT, codec_, width_, height_};
}

} // namespace parties::encdec::mft
