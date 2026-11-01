#include "mft_encoder.h"
#include <encdec/rate_control.h>

#include <mferror.h>
#include <codecapi.h>
#include <strmif.h>

#include <chrono>
#include <string>

#include <parties/log.h>
#include <parties/profiler.h>
#include <parties/thread_scheduling.h>
#include <parties/video_common.h>

using Microsoft::WRL::ComPtr;

namespace parties::encdec::mft {

namespace {

HRESULT set_codec_u32(ICodecAPI* api, const GUID& key, UINT32 value) {
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = value;
    return api->SetValue(&key, &var);
}

HRESULT set_codec_bool(ICodecAPI* api, const GUID& key, bool value) {
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_BOOL;
    var.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    return api->SetValue(&key, &var);
}

bool get_codec_u32(ICodecAPI* api, const GUID& key, UINT32& value) {
    VARIANT var;
    VariantInit(&var);
    const HRESULT hr = api->GetValue(&key, &var);
    const bool ok = SUCCEEDED(hr) && var.vt == VT_UI4;
    if (ok) value = var.ulVal;
    VariantClear(&var);
    return ok;
}

// CODECAPI_AVEncCommonBufferSize is documented by Microsoft in BYTES, unlike
// the bit-based StreamRateControl. Hardware MFTs are expected to follow the
// documentation; this could not be cross-checked against every vendor MFT.
UINT32 vbv_bytes(const StreamRateControl& rate_control) {
    return rate_control.vbv_buffer_size / 8u;
}

// How long the destructor waits for the event loop to finish the drain before
// it shuts the MFT down to unblock the loop's GetEvent.
constexpr auto ENCODER_LOOP_EXIT_TIMEOUT = std::chrono::milliseconds(500);

// Annex-B start code at data[i]: returns its length (3 or 4), or 0.
size_t start_code_length(const uint8_t* data, size_t len, size_t i) {
    if (i + 3 <= len && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) return 3;
    if (i + 4 <= len && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
        data[i + 3] == 1)
        return 4;
    return 0;
}

bool is_filler_nal(VideoCodecId codec, uint8_t header) {
    if (codec == VideoCodecId::H264) return (header & 0x1f) == 12;          // filler data
    if (codec == VideoCodecId::H265) return ((header >> 1) & 0x3f) == 38;   // FD_NUT
    return false;
}

// There is no ICodecAPI switch for filler data, and some hardware MFTs (AMD's
// H.264/HEVC encoders) pad CBR output with filler NAL units up to the exact
// target rate. Filler only exists for HRD conformance on constant-rate
// channels; on our link it is pure overhead that keeps the path saturated.
// Copies the Annex-B access unit without filler NAL units into `out`.
// Returns false (and leaves `out` untouched) when the unit has none.
bool strip_filler_nal_units(VideoCodecId codec, const uint8_t* data, size_t len,
                            std::vector<uint8_t>& out) {
    if (codec != VideoCodecId::H264 && codec != VideoCodecId::H265) return false;

    // First pass: locate the first NAL unit and look for any filler.
    bool has_filler = false;
    size_t first = len;
    for (size_t i = 0; i < len; ++i) {
        const size_t sc = start_code_length(data, len, i);
        if (sc == 0) continue;
        if (first == len) first = i;
        if (i + sc < len && is_filler_nal(codec, data[i + sc])) {
            has_filler = true;
            break;
        }
        i += sc - 1;
    }
    if (!has_filler) return false;

    out.clear();
    out.reserve(len);
    out.insert(out.end(), data, data + first);   // bytes before the first NAL (normally none)
    size_t nal_start = first;
    bool keep = true;
    for (size_t i = first; i <= len; ++i) {
        const size_t sc = i < len ? start_code_length(data, len, i) : 1;
        if (sc == 0) continue;
        if (i > nal_start && keep) out.insert(out.end(), data + nal_start, data + i);
        if (i == len) break;
        nal_start = i;
        keep = !(i + sc < len && is_filler_nal(codec, data[i + sc]));
        i += sc - 1;
    }
    return true;
}

} // namespace

MftEncoder::~MftEncoder() {
    // No early-out on !initialized_: a failed init() may still owe MFShutdown
    // and have partially constructed members; everything below is null-safe.
    encoder_running_ = false;

    if (encoder_) {
        encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    }

    if (encoder_thread_.joinable()) {
        // The loop leaves on the next event (the drain produces at least
        // METransformDrainComplete). Async MFTs implement IMFShutdown, which
        // also makes a GetEvent that would otherwise block forever return
        // MF_E_SHUTDOWN, so shutdown never depends on the driver's drain.
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            loop_exit_cv_.wait_for(lock, ENCODER_LOOP_EXIT_TIMEOUT,
                                   [this] { return loop_exited_; });
        }
        ComPtr<IMFShutdown> shutdown;
        if (encoder_ && SUCCEEDED(encoder_.As(&shutdown)) && shutdown)
            shutdown->Shutdown();
        encoder_thread_.join();
    }

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

    if (mf_started_)
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
        LOG_ERROR("MFStartup failed: {:#010x}", hr);
        return false;
    }
    mf_started_ = true;

    // Create DXGI device manager
    hr = MFCreateDXGIDeviceManager(&dxgi_reset_token_, &dxgi_manager_);
    if (FAILED(hr)) {
        LOG_ERROR("MFCreateDXGIDeviceManager failed");
        return false;
    }
    hr = dxgi_manager_->ResetDevice(device_.Get(), dxgi_reset_token_);
    if (FAILED(hr)) {
        LOG_ERROR("ResetDevice failed");
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
        LOG_ERROR("No hardware encoder found");
        return false;
    }

    if (!configure_encoder(width, height, fps, bitrate)) {
        LOG_ERROR("Failed to configure encoder");
        return false;
    }

    if (!create_color_converter(input_width, input_height, width, height)) {
        LOG_ERROR("Failed to create color converter");
        return false;
    }

    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

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
        LOG_ERROR("ActivateObject failed: {:#010x}", hr);
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
        LOG_ERROR("Failed to set D3D manager: {:#010x}", hr);

    const auto rate_control = make_stream_cbr_rate_control(bitrate, fps);

    // Rate control and latency properties must be configured before the media
    // types: encoders read them while negotiating SetOutputType, and several
    // hardware MFTs ignore (or reset) rate-control changes made afterwards.
    ComPtr<ICodecAPI> codec_api;
    if (FAILED(encoder_.As(&codec_api)))
        LOG_WARN("MFT encoder exposes no ICodecAPI; only MF_MT_AVG_BITRATE applies");
    if (codec_api) {
        auto check = [](HRESULT result, const char* name) {
            if (FAILED(result))
                LOG_WARN("MFT encoder rejected {}: {:#010x}", name,
                         static_cast<unsigned>(result));
        };
        check(set_codec_bool(codec_api.Get(), CODECAPI_AVLowLatencyMode, true),
              "AVLowLatencyMode");
        // CBR with a VBV of ~2 frame intervals (see rate_control.h).
        check(set_codec_u32(codec_api.Get(), CODECAPI_AVEncCommonRateControlMode,
                            eAVEncCommonRateControlMode_CBR),
              "AVEncCommonRateControlMode=CBR");
        check(set_codec_u32(codec_api.Get(), CODECAPI_AVEncCommonMeanBitRate,
                            rate_control.average_bitrate),
              "AVEncCommonMeanBitRate");
        check(set_codec_u32(codec_api.Get(), CODECAPI_AVEncCommonBufferSize,
                            vbv_bytes(rate_control)),
              "AVEncCommonBufferSize");
        check(set_codec_u32(codec_api.Get(), CODECAPI_AVEncMPVGOPSize,
                            fps * (VIDEO_KEYFRAME_INTERVAL_MS / 1000)),
              "AVEncMPVGOPSize");
        check(set_codec_u32(codec_api.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0),
              "AVEncMPVDefaultBPictureCount=0");
        check(set_codec_u32(codec_api.Get(), CODECAPI_AVEncCommonQualityVsSpeed, 25),
              "AVEncCommonQualityVsSpeed");
        check(set_codec_u32(codec_api.Get(), CODECAPI_VideoEncoderDisplayContentType, 1),
              "VideoEncoderDisplayContentType");
    }

    // Output type (must be set BEFORE input type)
    ComPtr<IMFMediaType> output_type;
    MFCreateMediaType(&output_type);
    output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    output_type->SetGUID(MF_MT_SUBTYPE, subtype);
    output_type->SetUINT32(MF_MT_AVG_BITRATE, rate_control.average_bitrate);
    MFSetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(output_type.Get(), MF_MT_FRAME_RATE, fps, 1);
    output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    hr = encoder_->SetOutputType(0, output_type.Get(), 0);
    if (FAILED(hr)) {
        LOG_ERROR("SetOutputType failed: {:#010x}", hr);
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
        LOG_ERROR("SetInputType failed: {:#010x}", hr);
        return false;
    }

    // Report what the MFT actually runs with after type negotiation: some
    // encoders silently keep their default rate control mode.
    if (codec_api) {
        UINT32 mode = 0, mean = 0, buffer = 0;
        const bool have_mode =
            get_codec_u32(codec_api.Get(), CODECAPI_AVEncCommonRateControlMode, mode);
        const bool have_mean =
            get_codec_u32(codec_api.Get(), CODECAPI_AVEncCommonMeanBitRate, mean);
        const bool have_buffer =
            get_codec_u32(codec_api.Get(), CODECAPI_AVEncCommonBufferSize, buffer);
        if (!have_mode) {
            LOG_WARN("MFT encoder does not report its rate control mode (requested CBR)");
        } else if (mode != static_cast<UINT32>(eAVEncCommonRateControlMode_CBR)) {
            LOG_WARN("MFT encoder runs rate control mode {} instead of CBR ({})",
                     mode, static_cast<UINT32>(eAVEncCommonRateControlMode_CBR));
        }
        LOG_INFO("MFT encoder {} ({}x{} @ {} fps): rate control mode {}, mean {} bps, "
                 "buffer {} bytes (requested CBR {} bps, VBV {} bits = {} bytes)",
                 codec_name(codec_), width, height, fps,
                 have_mode ? std::to_string(mode) : std::string("?"),
                 have_mean ? std::to_string(mean) : std::string("?"),
                 have_buffer ? std::to_string(buffer) : std::string("?"),
                 rate_control.average_bitrate, rate_control.vbv_buffer_size,
                 vbv_bytes(rate_control));
    }

    return true;
}

bool MftEncoder::create_color_converter(uint32_t in_w, uint32_t in_h,
                                         uint32_t out_w, uint32_t out_h) {
    HRESULT hr = CoCreateInstance(
        CLSID_VideoProcessorMFT, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&converter_));
    if (FAILED(hr)) {
        LOG_ERROR("CoCreateInstance VideoProcessorMFT failed: {:#010x}", hr);
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
        LOG_ERROR("Converter SetInputType failed: {:#010x}", hr);
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
        LOG_ERROR("Converter SetOutputType failed: {:#010x}", hr);
        return false;
    }

    return true;
}

void MftEncoder::encoder_loop() {
    TracySetThreadName("MftEncoder");
    if (!parties::set_current_thread_highest_priority())
        LOG_WARN("Failed to set the MFT encoder thread to highest priority");
    // The loop never waits for input: a METransformNeedInput only records a
    // request, and samples are fed whenever a request and a queued sample are
    // both present, by whichever thread completes the pair (this one, or the
    // encode() caller). Blocking here until the next frame arrived used to
    // leave a METransformHaveOutput queued behind the NeedInput, so output
    // always lagged one frame and a forced keyframe on a static screen never
    // left the encoder.
    while (encoder_running_) {
        ZoneScopedN("MftEncoder::encoder_loop");
        ComPtr<IMFMediaEvent> event;
        HRESULT hr = encoder_events_->GetEvent(0, &event);
        if (FAILED(hr)) {
            if (hr != MF_E_SHUTDOWN)
                LOG_ERROR("MFT encoder GetEvent failed: {:#010x}",
                          static_cast<unsigned>(hr));
            break;
        }

        MediaEventType event_type;
        event->GetType(&event_type);

        if (event_type == METransformNeedInput) {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            ++pending_input_requests_;
            feed_pending_input(lock);
        } else if (event_type == METransformHaveOutput) {
            // Async contract: exactly one ProcessOutput per HaveOutput event.
            collect_output(1);
        } else if (event_type == METransformDrainComplete) {
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        loop_exited_ = true;
    }
    // A dead loop accepts no more input; encode() reports that as a failure.
    encoder_running_ = false;
    loop_exit_cv_.notify_all();
}

void MftEncoder::feed_pending_input(std::unique_lock<std::mutex>& lock) {
    // Only one thread feeds at a time, and never while holding queue_mutex_
    // across ProcessInput, so neither thread can block on the other's call
    // into the MFT. A thread that finds feeding_ set leaves its new request /
    // sample to the feeding thread, which re-checks under the lock before it
    // stops.
    if (feeding_) return;
    feeding_ = true;
    while (encoder_running_ && pending_input_requests_ > 0 && !input_queue_.empty()) {
        ComPtr<IMFSample> sample = std::move(input_queue_.front());
        input_queue_.pop();
        --pending_input_requests_;
        lock.unlock();
        submit_input(sample.Get());
        lock.lock();
    }
    feeding_ = false;
}

void MftEncoder::submit_input(IMFSample* sample) {
    if (force_keyframe_.exchange(false)) {
        ComPtr<ICodecAPI> codec_api;
        if (SUCCEEDED(encoder_.As(&codec_api))) {
            const HRESULT hr =
                set_codec_u32(codec_api.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
            if (FAILED(hr))
                LOG_WARN("MFT encoder rejected AVEncVideoForceKeyFrame: {:#010x}",
                         static_cast<unsigned>(hr));
        }
    }

    const HRESULT hr = encoder_->ProcessInput(0, sample, 0);
    if (FAILED(hr))
        LOG_ERROR("ProcessInput failed: {:#010x}", static_cast<unsigned>(hr));
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
        LOG_ERROR("MFCreateDXGISurfaceBuffer failed: {:#010x}", hr);
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
        LOG_ERROR("Converter ProcessInput failed: {:#010x}", hr);
        return false;
    }

    MFT_OUTPUT_DATA_BUFFER conv_output = {};
    DWORD conv_status = 0;
    hr = converter_->ProcessOutput(0, 1, &conv_output, &conv_status);
    if (conv_output.pEvents) conv_output.pEvents->Release();
    if (FAILED(hr)) {
        LOG_ERROR("Converter ProcessOutput failed: {:#010x}", hr);
        if (conv_output.pSample) conv_output.pSample->Release();
        return false;
    }

    ComPtr<IMFSample> nv12_sample;
    nv12_sample.Attach(conv_output.pSample);
    if (!nv12_sample) {
        LOG_ERROR("Converter produced null sample");
        return false;
    }

    nv12_sample->SetSampleTime(timestamp_100ns);
    nv12_sample->SetSampleDuration(duration);

    // Feed NV12 to encoder
    if (async_mode_) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        // The event loop has stopped (its GetEvent failure was logged).
        if (!encoder_running_) return false;
        // Keep at most two frames waiting for the encoder: when it falls
        // behind, the oldest frame is the one not worth sending.
        while (input_queue_.size() >= 2)
            input_queue_.pop();
        input_queue_.push(nv12_sample);
        // Feed right away if the encoder already asked for input.
        feed_pending_input(lock);
    } else {
        if (force_keyframe_.exchange(false)) {
            ComPtr<ICodecAPI> codec_api;
            if (SUCCEEDED(encoder_.As(&codec_api))) {
                const HRESULT key_hr =
                    set_codec_u32(codec_api.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
                if (FAILED(key_hr))
                    LOG_WARN("MFT encoder rejected AVEncVideoForceKeyFrame: {:#010x}",
                             static_cast<unsigned>(key_hr));
            }
        }

        hr = encoder_->ProcessInput(0, nv12_sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            collect_output();
            hr = encoder_->ProcessInput(0, nv12_sample.Get(), 0);
        }
        if (FAILED(hr)) {
            LOG_ERROR("ProcessInput failed: {:#010x}", hr);
            return false;
        }
        collect_output();
    }

    return true;
}

bool MftEncoder::collect_output(int max_outputs) {
    ZoneScopedN("MftEncoder::collect_output");
    int collected = 0;
    for (int call = 0; max_outputs <= 0 || call < max_outputs; ++call) {
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

            if (on_encoded && data && len > 0) {
                if (strip_filler_nal_units(codec_, data, len, filtered_output_))
                    on_encoded(filtered_output_.data(), filtered_output_.size(), keyframe);
                else
                    on_encoded(data, len, keyframe);
            }

            buf->Unlock();
            collected++;

            // We always pass pSample = null in, so any sample here was
            // allocated by the MFT and is ours to release — regardless of
            // what GetOutputStreamInfo reported.
            output_data.pSample->Release();
        }

        if (output_data.pEvents) output_data.pEvents->Release();
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
        const auto rate_control = make_stream_cbr_rate_control(bitrate, fps_);
        const HRESULT mean_hr = set_codec_u32(
            codec_api.Get(), CODECAPI_AVEncCommonMeanBitRate, rate_control.average_bitrate);
        // Not every MFT accepts a VBV change while streaming; the mean bitrate
        // is the part that matters most for the link.
        const HRESULT buffer_hr = set_codec_u32(
            codec_api.Get(), CODECAPI_AVEncCommonBufferSize, vbv_bytes(rate_control));
        if (FAILED(mean_hr) || FAILED(buffer_hr)) {
            // set_bitrate runs for every send-controller step: warn once.
            if (!rate_update_warned_) {
                LOG_WARN("MFT encoder rate update to {} bps: mean bitrate {:#010x}, "
                         "buffer size {:#010x} (further failures are not logged)",
                         bitrate, static_cast<unsigned>(mean_hr),
                         static_cast<unsigned>(buffer_hr));
                rate_update_warned_ = true;
            }
        } else {
            LOG_DEBUG("MFT rate control updated: CBR {} bps, VBV {} bits",
                      rate_control.average_bitrate, rate_control.vbv_buffer_size);
        }
    }
}

EncoderInfo MftEncoder::info() const {
    return {Backend::MFT, codec_, width_, height_};
}

} // namespace parties::encdec::mft
