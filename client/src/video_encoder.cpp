#include <client/video_encoder.h>
#include "nvidia/nvenc_encoder.h"

#include <mfapi.h>
#include <mftransform.h>
#include <mfidl.h>
#include <mferror.h>
#include <codecapi.h>
#include <strmif.h>
#include <d3d11.h>

#include <cstdio>
#include <parties/profiler.h>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "strmiids.lib")

using Microsoft::WRL::ComPtr;

namespace parties::client {

VideoEncoder::VideoEncoder() = default;

VideoEncoder::~VideoEncoder() {
    shutdown();
}

bool VideoEncoder::init(ID3D11Device* device, uint32_t width, uint32_t height,
                         uint32_t input_width, uint32_t input_height,
                         uint32_t fps, uint32_t bitrate,
                         VideoCodecId preferred_codec) {
	ZoneScopedN("VideoEncoder::init");
    if (initialized_) return false;

    width_ = width;
    height_ = height;
    fps_ = fps;

    // Even dimensions for NV12
    uint32_t enc_w = (width + 1) & ~1u;
    uint32_t enc_h = (height + 1) & ~1u;

    // Try NVENC first (direct D3D11, no color conversion needed)
    {
        auto nvenc = std::make_unique<nvidia::NvencEncoder>();
        if (nvenc->init(device, enc_w, enc_h, fps, bitrate, preferred_codec)) {
            nvenc_ = std::move(nvenc);
            codec_ = nvenc_->codec();
            width_ = enc_w;
            height_ = enc_h;
            initialized_ = true;
            return true;
        }
    }

    // Fall back to MFT encoder
    return init_mft(device, width, height, input_width, input_height,
                    fps, bitrate, preferred_codec);
}

bool VideoEncoder::init_mft(ID3D11Device* device, uint32_t width, uint32_t height,
                              uint32_t input_width, uint32_t input_height,
                              uint32_t fps, uint32_t bitrate,
                              VideoCodecId preferred_codec) {
    device_ = device;
    device_->GetImmediateContext(&context_);

    // Default input size to output size if not specified
    if (input_width == 0) input_width = width;
    if (input_height == 0) input_height = height;

    // NV12 requires even dimensions (2:1 chroma subsampling)
    width = (width + 1) & ~1u;
    height = (height + 1) & ~1u;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[VideoEncoder] MFStartup failed: 0x%08lx\n", hr);
        return false;
    }

    // Create DXGI device manager
    hr = MFCreateDXGIDeviceManager(&dxgi_reset_token_, &dxgi_manager_);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[VideoEncoder] MFCreateDXGIDeviceManager failed\n");
        return false;
    }
    hr = dxgi_manager_->ResetDevice(device_.Get(), dxgi_reset_token_);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[VideoEncoder] ResetDevice failed\n");
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

    // Try preferred codec first
    for (auto& c : all_codecs) {
        if (c.id == preferred_codec) {
            found = try_create_encoder(c.subtype, c.id);
            break;
        }
    }

    // Fall back to remaining codecs in priority order
    if (!found) {
        for (auto& c : all_codecs) {
            if (c.id != preferred_codec) {
                found = try_create_encoder(c.subtype, c.id);
                if (found) break;
            }
        }
    }

    if (!found) {
        std::fprintf(stderr, "[VideoEncoder] No hardware encoder found\n");
        return false;
    }

    // Configure the encoder
    if (!configure_encoder(width, height, fps, bitrate)) {
        std::fprintf(stderr, "[VideoEncoder] Failed to configure encoder\n");
        return false;
    }

    // Create BGRA→NV12 color converter (also handles scaling if input != output)
    if (!create_color_converter(input_width, input_height, width, height)) {
        std::fprintf(stderr, "[VideoEncoder] Failed to create color converter\n");
        return false;
    }

    // Signal start of stream (correct order: BEGIN first, then START)
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    // Cache output stream info (constant for the encoder's lifetime)
    MFT_OUTPUT_STREAM_INFO stream_info = {};
    if (SUCCEEDED(encoder_->GetOutputStreamInfo(0, &stream_info)))
        encoder_provides_samples_ = (stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;

    // Start encoder thread for async MFTs
    if (async_mode_ && encoder_events_) {
        encoder_running_ = true;
        encoder_thread_ = std::thread(&VideoEncoder::encoder_loop, this);
    }

    width_ = width;
    height_ = height;
    initialized_ = true;
    return true;
}

void VideoEncoder::shutdown() {
    if (!initialized_) return;

    if (nvenc_) {
        nvenc_->shutdown();
        nvenc_.reset();
        initialized_ = false;
        return;
    }

    // Stop encoder thread first
    encoder_running_ = false;
    queue_cv_.notify_all();

    if (encoder_) {
        encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    }

    if (encoder_thread_.joinable())
        encoder_thread_.join();

    // Drain remaining queue
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
    initialized_ = false;
    async_mode_ = false;
}

bool VideoEncoder::try_create_encoder(const GUID& codec_subtype, VideoCodecId id) {
	ZoneScopedN("VideoEncoder::try_create_encoder");
    MFT_REGISTER_TYPE_INFO output_info = {};
    output_info.guidMajorType = MFMediaType_Video;
    output_info.guidSubtype = codec_subtype;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;

    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        flags,
        nullptr,        // any input type
        &output_info,
        &activates,
        &count);

    if (FAILED(hr) || count == 0) return false;

    // Try the first (best ranked) hardware encoder
    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&encoder_));

    // Free all activate objects
    for (UINT32 i = 0; i < count; i++)
        activates[i]->Release();
    CoTaskMemFree(activates);

    if (FAILED(hr)) {
        std::fprintf(stderr, "[VideoEncoder] ActivateObject failed: 0x%08lx\n", hr);
        return false;
    }

    // Check if MFT supports async mode
    ComPtr<IMFAttributes> attrs;
    hr = encoder_->GetAttributes(&attrs);

    UINT32 is_async = FALSE;
    if (attrs)
        attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);

    if (is_async && attrs) {
        hr = attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        if (SUCCEEDED(hr)) {
            hr = encoder_.As(&encoder_events_);
            if (SUCCEEDED(hr) && encoder_events_)
                async_mode_ = true;
        }
    }

    if (!async_mode_)
        encoder_events_.Reset();

    codec_ = id;
    return true;
}

bool VideoEncoder::configure_encoder(uint32_t width, uint32_t height,
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
        std::fprintf(stderr, "[VideoEncoder] Failed to set D3D manager: 0x%08lx\n", hr);

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
        std::fprintf(stderr, "[VideoEncoder] SetOutputType failed: 0x%08lx\n", hr);
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
        std::fprintf(stderr, "[VideoEncoder] SetInputType failed: 0x%08lx\n", hr);
        return false;
    }

    // Configure codec properties via ICodecAPI
    ComPtr<ICodecAPI> codec_api;
    if (SUCCEEDED(encoder_.As(&codec_api))) {
        VARIANT var;

        // Low-latency mode
        VariantInit(&var);
        var.vt = VT_BOOL;
        var.boolVal = VARIANT_TRUE;
        codec_api->SetValue(&CODECAPI_AVLowLatencyMode, &var);

        // CBR rate control
        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = eAVEncCommonRateControlMode_CBR;
        codec_api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);

        // Mean bitrate
        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = bitrate;
        codec_api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);

        // GOP size (keyframe interval)
        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = fps * (VIDEO_KEYFRAME_INTERVAL_MS / 1000);
        codec_api->SetValue(&CODECAPI_AVEncMPVGOPSize, &var);

        // No B-frames (minimize latency)
        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = 0;
        codec_api->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &var);

        // Speed over quality for real-time
        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = 25;  // 0-33 = fastest
        codec_api->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &var);

        // Screen content hint
        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = 1;  // screen content
        codec_api->SetValue(&CODECAPI_VideoEncoderDisplayContentType, &var);
    }

    return true;
}

bool VideoEncoder::create_color_converter(uint32_t in_w, uint32_t in_h,
                                           uint32_t out_w, uint32_t out_h) {
    HRESULT hr = CoCreateInstance(
        CLSID_VideoProcessorMFT, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&converter_));
    if (FAILED(hr)) {
        std::fprintf(stderr, "[VideoEncoder] CoCreateInstance VideoProcessorMFT failed: 0x%08lx\n", hr);
        return false;
    }

    // Set D3D11 device manager on converter
    converter_->ProcessMessage(
        MFT_MESSAGE_SET_D3D_MANAGER,
        reinterpret_cast<ULONG_PTR>(dxgi_manager_.Get()));

    // Input: BGRA (from screen capture, at capture resolution)
    ComPtr<IMFMediaType> in_type;
    MFCreateMediaType(&in_type);
    in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
    MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, in_w, in_h);

    hr = converter_->SetInputType(0, in_type.Get(), 0);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[VideoEncoder] Converter SetInputType failed: 0x%08lx\n", hr);
        return false;
    }

    // Output: NV12 (for encoder, at encode resolution — GPU handles scaling)
    ComPtr<IMFMediaType> out_type;
    MFCreateMediaType(&out_type);
    out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(out_type.Get(), MF_MT_FRAME_SIZE, out_w, out_h);

    hr = converter_->SetOutputType(0, out_type.Get(), 0);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[VideoEncoder] Converter SetOutputType failed: 0x%08lx\n", hr);
        return false;
    }

    return true;
}

void VideoEncoder::encoder_loop() {
	TracySetThreadName("VideoEncoder");
    while (encoder_running_) {
        ZoneScopedN("VideoEncoder::encoder_loop");
        // Blocking wait for next MFT event
        ComPtr<IMFMediaEvent> event;
        HRESULT hr = encoder_events_->GetEvent(0, &event);
        if (FAILED(hr))
            break;

        MediaEventType event_type;
        event->GetType(&event_type);

        if (event_type == METransformNeedInput) {
            // Wait for a sample from the queue
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

            // Force keyframe if requested
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
            if (FAILED(hr)) {
                std::fprintf(stderr, "[VideoEncoder] ProcessInput failed: 0x%08lx\n", hr);
            }
        } else if (event_type == METransformHaveOutput) {
            collect_output();
        } else if (event_type == METransformDrainComplete) {
            break;
        }
    }
}

bool VideoEncoder::encode_frame(ID3D11Texture2D* bgra_texture, int64_t timestamp_100ns) {
	ZoneScopedN("VideoEncoder::encode_frame");
    if (!initialized_) return false;

    if (nvenc_) {
        nvenc_->on_encoded = on_encoded;
        return nvenc_->encode_frame(bgra_texture, timestamp_100ns);
    }

    int64_t duration = 10'000'000LL / fps_;

    // Step 1: Wrap BGRA texture into MF sample
    ComPtr<IMFMediaBuffer> bgra_buffer;
    HRESULT hr = MFCreateDXGISurfaceBuffer(
        __uuidof(ID3D11Texture2D), bgra_texture, 0, FALSE, &bgra_buffer);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[VideoEncoder] MFCreateDXGISurfaceBuffer failed: 0x%08lx\n", hr);
        return false;
    }

    ComPtr<IMFSample> bgra_sample;
    MFCreateSample(&bgra_sample);
    bgra_sample->AddBuffer(bgra_buffer.Get());
    bgra_sample->SetSampleTime(timestamp_100ns);
    bgra_sample->SetSampleDuration(duration);

    // Step 2: BGRA → NV12 via Video Processor MFT
    hr = converter_->ProcessInput(0, bgra_sample.Get(), 0);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[VideoEncoder] Converter ProcessInput failed: 0x%08lx\n", hr);
        return false;
    }

    MFT_OUTPUT_DATA_BUFFER conv_output = {};
    DWORD conv_status = 0;
    hr = converter_->ProcessOutput(0, 1, &conv_output, &conv_status);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[VideoEncoder] Converter ProcessOutput failed: 0x%08lx\n", hr);
        return false;
    }

    ComPtr<IMFSample> nv12_sample;
    nv12_sample.Attach(conv_output.pSample);
    if (!nv12_sample) {
        std::fprintf(stderr, "[VideoEncoder] Converter produced null sample\n");
        return false;
    }

    nv12_sample->SetSampleTime(timestamp_100ns);
    nv12_sample->SetSampleDuration(duration);

    // Step 3: Feed NV12 to encoder
    if (async_mode_) {
        // Push to queue for encoder thread
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            // Drop old frames if encoder can't keep up (keep ~33ms of buffer at 60fps)
            while (input_queue_.size() >= 2)
                input_queue_.pop();
            input_queue_.push(nv12_sample);
        }
        queue_cv_.notify_one();
    } else {
        // Synchronous mode
        // Force keyframe if requested
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
            // Encoder is full — collect output first then retry
            collect_output();
            hr = encoder_->ProcessInput(0, nv12_sample.Get(), 0);
        }
        if (FAILED(hr)) {
            std::fprintf(stderr, "[VideoEncoder] ProcessInput failed: 0x%08lx\n", hr);
            return false;
        }
        collect_output();
    }

    return true;
}

bool VideoEncoder::collect_output() {
	ZoneScopedN("VideoEncoder::collect_output");
    int collected = 0;
    while (true) {
        MFT_OUTPUT_DATA_BUFFER output_data = {};
        DWORD status = 0;
        HRESULT hr = encoder_->ProcessOutput(0, 1, &output_data, &status);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            break;
        }
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
                    MFSampleExtension_CleanPoint, &clean_point))) {
                keyframe = (clean_point != 0);
            }

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

void VideoEncoder::force_keyframe() {
    if (nvenc_) { nvenc_->force_keyframe(); return; }
    force_keyframe_ = true;
}

void VideoEncoder::set_bitrate(uint32_t bitrate) {
    if (!initialized_) return;

    if (nvenc_) { nvenc_->set_bitrate(bitrate); return; }

    ComPtr<ICodecAPI> codec_api;
    if (SUCCEEDED(encoder_.As(&codec_api))) {
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = bitrate;
        codec_api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);
    }
}

} // namespace parties::client
