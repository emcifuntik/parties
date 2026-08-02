#include "amf_encoder.h"
#include "amf_loader.h"
#include <encdec/rate_control.h>

#include <AMF/components/VideoEncoderVCE.h>
#include <AMF/components/VideoEncoderHEVC.h>
#include <AMF/components/VideoEncoderAV1.h>

#include <parties/profiler.h>
#include <parties/log.h>

namespace parties::encdec::amd {

AmfEncoder::AmfEncoder() = default;

AmfEncoder::~AmfEncoder() {
    if (!initialized_) return;

    if (encoder_) {
        // Drain is an end-of-stream operation and can block in a driver while
        // the application is tearing down its capture device. There is no
        // consumer for delayed output at this point, so terminate directly.
        encoder_->Terminate();
        encoder_->Release();
        encoder_ = nullptr;
    }

    staging_texture_.Reset();
    d3d_context_.Reset();
    device_.Reset();
    num_external_inputs_ = 0;

    if (context_) {
        context_->Terminate();
        context_->Release();
        context_ = nullptr;
    }

    initialized_ = false;
}

bool AmfEncoder::init(ID3D11Device* device, uint32_t width, uint32_t height,
                       uint32_t fps, uint32_t bitrate, VideoCodecId preferred_codec) {
    ZoneScopedN("AmfEncoder::init");
    if (initialized_) return false;

    if (!load_amf(factory_)) return false;

    AMF_RESULT res = factory_->CreateContext(&context_);
    if (res != AMF_OK || !context_) {
        LOG_ERROR("CreateContext failed: {}", (int)res);
        return false;
    }

    res = context_->InitDX11(device);
    if (res != AMF_OK) {
        LOG_ERROR("InitDX11 failed: {}", (int)res);
        context_->Release();
        context_ = nullptr;
        return false;
    }

    device_ = device;
    device_->GetImmediateContext(&d3d_context_);
    width_ = width;
    height_ = height;
    fps_ = fps;

    struct CodecEntry { const wchar_t* id; VideoCodecId codec; };
    CodecEntry codecs[] = {
        {AMFVideoEncoder_AV1,    VideoCodecId::AV1},
        {AMFVideoEncoder_HEVC,   VideoCodecId::H265},
        {AMFVideoEncoderVCE_AVC, VideoCodecId::H264},
    };

    bool found = false;
    for (auto& c : codecs) {
        if (c.codec == preferred_codec && try_codec(c.id, c.codec)) { found = true; break; }
    }
    if (!found) {
        for (auto& c : codecs) {
            if (c.codec != preferred_codec && try_codec(c.id, c.codec)) { found = true; break; }
        }
    }

    if (!found) {
        LOG_ERROR("No supported encoder codec found");
        context_->Release();
        context_ = nullptr;
        return false;
    }

    const auto rate_control = make_stream_vbr_rate_control(bitrate);
    LOG_INFO("Selected encoder codec: {} ({}x{} @ {} fps), VBR average: {} bps, peak: {} bps",
             codec_name(codec_), width, height, fps,
             rate_control.average_bitrate, rate_control.peak_bitrate);

    const amf_int64 average_bitrate =
        static_cast<amf_int64>(rate_control.average_bitrate);
    const amf_int64 peak_bitrate =
        static_cast<amf_int64>(rate_control.peak_bitrate);
    const amf_int64 vbv_buffer_size =
        static_cast<amf_int64>(rate_control.vbv_buffer_size);
    amf_int64 keyframe_period = static_cast<amf_int64>(fps * (VIDEO_KEYFRAME_INTERVAL_MS / 1000));

    if (codec_ == VideoCodecId::AV1) {
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_USAGE,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_USAGE_LOW_LATENCY));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_SPEED));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, average_bitrate);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, peak_bitrate);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE, vbv_buffer_size);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_INITIAL_VBV_BUFFER_FULLNESS, static_cast<amf_int64>(64));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_ENFORCE_HRD, true);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_FILLER_DATA, false);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_FRAMERATE, AMFConstructRate(fps, 1));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_GOP_SIZE, keyframe_period);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE_KEY_FRAME_ALIGNED));
    } else if (codec_ == VideoCodecId::H265) {
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, average_bitrate);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, peak_bitrate);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, vbv_buffer_size);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_INITIAL_VBV_BUFFER_FULLNESS, static_cast<amf_int64>(64));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_ENFORCE_HRD, true);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_FILLER_DATA_ENABLE, false);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, AMFConstructRate(fps, 1));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, keyframe_period);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE_IDR_ALIGNED));
    } else {
        encoder_->SetProperty(AMF_VIDEO_ENCODER_USAGE,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, average_bitrate);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, peak_bitrate);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, vbv_buffer_size);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_INITIAL_VBV_BUFFER_FULLNESS, static_cast<amf_int64>(64));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_ENFORCE_HRD, true);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE, false);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, AMFConstructRate(fps, 1));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, keyframe_period);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, static_cast<amf_int64>(0));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING, keyframe_period);
    }

    res = encoder_->Init(amf::AMF_SURFACE_BGRA, width, height);
    if (res != AMF_OK) {
        LOG_ERROR("Encoder Init(BGRA) failed: {}", (int)res);
        encoder_->Release();
        encoder_ = nullptr;
        context_->Release();
        context_ = nullptr;
        return false;
    }

    initialized_ = true;
    return true;
}

bool AmfEncoder::try_codec(const wchar_t* component_id, VideoCodecId id) {
    amf::AMFComponent* enc = nullptr;
    AMF_RESULT res = factory_->CreateComponent(context_, component_id, &enc);
    if (res != AMF_OK || !enc) return false;

    encoder_ = enc;
    codec_ = id;
    return true;
}

bool AmfEncoder::encode(ID3D11Texture2D* bgra_texture, int64_t timestamp_100ns) {
    ZoneScopedN("AmfEncoder::encode");
    if (!initialized_ || !bgra_texture || !ensure_staging_texture()) return false;

    d3d_context_->CopyResource(staging_texture_.Get(), bgra_texture);
    d3d_context_->Flush();

    return do_encode(staging_texture_.Get(), timestamp_100ns);
}

bool AmfEncoder::ensure_staging_texture() {
    if (staging_texture_) return true;

    // Registered capture textures are the normal path and are wrapped by AMF
    // directly. Allocate a copy target only for callers using encode().
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc = {1, 0};
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    const HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &staging_texture_);
    if (FAILED(hr)) {
        LOG_ERROR("CreateTexture2D AMF fallback staging failed: {:#010x}",
                  static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

int AmfEncoder::register_input(ID3D11Texture2D* texture) {
    if (num_external_inputs_ >= MAX_EXTERNAL_INPUTS) return -1;
    int slot = num_external_inputs_++;
    external_inputs_[slot] = texture;
    return slot;
}

void AmfEncoder::unregister_inputs() {
    for (int i = 0; i < num_external_inputs_; i++)
        external_inputs_[i] = nullptr;
    num_external_inputs_ = 0;
}

bool AmfEncoder::encode_registered(int slot, int64_t timestamp_100ns) {
    ZoneScopedN("AmfEncoder::encode_registered");
    if (!initialized_ || slot < 0 || slot >= num_external_inputs_) return false;
    return do_encode(external_inputs_[slot], timestamp_100ns);
}

bool AmfEncoder::do_encode(ID3D11Texture2D* texture, int64_t timestamp_100ns) {
    amf::AMFSurface* surface = nullptr;
    AMF_RESULT res = context_->CreateSurfaceFromDX11Native(texture, &surface, nullptr);
    if (res != AMF_OK || !surface) {
        LOG_ERROR("CreateSurfaceFromDX11Native failed: {}", (int)res);
        return false;
    }

    surface->SetPts(timestamp_100ns);

    const bool requested_keyframe = force_keyframe_;
    if (requested_keyframe) {
        if (codec_ == VideoCodecId::AV1) {
            surface->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE,
                static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE_KEY));
        } else if (codec_ == VideoCodecId::H265) {
            surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE,
                static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR));
        } else {
            surface->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE,
                static_cast<amf_int64>(AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR));
        }
    }

    // AMF_INPUT_FULL means ownership has not transferred. Retain and retry the
    // exact same surface after consuming output; releasing it here used to drop
    // frames and could also lose a requested IDR.
    static constexpr int max_submit_retries = 8;
    for (int retry = 0; retry < max_submit_retries; ++retry) {
        res = encoder_->SubmitInput(surface);
        if (res == AMF_OK) break;
        if (res != AMF_INPUT_FULL) {
            LOG_ERROR("SubmitInput failed: {}", (int)res);
            surface->Release();
            return false;
        }

        amf::AMFData* pending = nullptr;
        const AMF_RESULT output_result = encoder_->QueryOutput(&pending);
        if (pending && !deliver_output(pending)) {
            surface->Release();
            return false;
        }
        if (output_result != AMF_OK && output_result != AMF_REPEAT) {
            LOG_ERROR("QueryOutput while encoder input was full failed: {}",
                      static_cast<int>(output_result));
            surface->Release();
            return false;
        }
        Sleep(0);
    }
    surface->Release();

    if (res != AMF_OK) {
        LOG_WARN("AMF encoder remained full after {} retries; dropping newest frame",
                 max_submit_retries);
        return true;
    }
    if (requested_keyframe) force_keyframe_ = false;

    amf::AMFData* data = nullptr;
    for (int retry = 0; retry < 100; retry++) {
        res = encoder_->QueryOutput(&data);
        if (res == AMF_OK && data) break;
        if (res == AMF_REPEAT || res == AMF_OK) {
            Sleep(1);
            continue;
        }
        break;
    }

    if (!data) return true;
    return deliver_output(data);
}

bool AmfEncoder::deliver_output(amf::AMFData* data) {
    if (!data) return true;

    amf::AMFBuffer* buffer = nullptr;
    data->QueryInterface(amf::AMFBuffer::IID(), reinterpret_cast<void**>(&buffer));
    if (!buffer) {
        data->Release();
        return false;
    }

    bool keyframe = false;
    amf_int64 frame_type = 0;
    if (codec_ == VideoCodecId::AV1) {
        if (data->GetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE, &frame_type) == AMF_OK)
            keyframe = (frame_type == AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE_KEY ||
                        frame_type == AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE_INTRA_ONLY);
    } else if (codec_ == VideoCodecId::H265) {
        if (data->GetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE, &frame_type) == AMF_OK)
            keyframe = (frame_type == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR ||
                        frame_type == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_I);
    } else {
        if (data->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &frame_type) == AMF_OK)
            keyframe = (frame_type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR ||
                        frame_type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_I);
    }

    if (on_encoded) {
        on_encoded(static_cast<const uint8_t*>(buffer->GetNative()),
                   buffer->GetSize(), keyframe);
    }

    buffer->Release();
    data->Release();
    return true;
}

void AmfEncoder::force_keyframe() {
    force_keyframe_ = true;
}

void AmfEncoder::set_bitrate(uint32_t bitrate) {
    if (!initialized_ || !encoder_) return;

    const auto rate_control = make_stream_vbr_rate_control(bitrate);
    const amf_int64 average_bitrate =
        static_cast<amf_int64>(rate_control.average_bitrate);
    const amf_int64 peak_bitrate =
        static_cast<amf_int64>(rate_control.peak_bitrate);
    const amf_int64 vbv_buffer_size =
        static_cast<amf_int64>(rate_control.vbv_buffer_size);
    if (codec_ == VideoCodecId::AV1) {
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, average_bitrate);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, peak_bitrate);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE, vbv_buffer_size);
    } else if (codec_ == VideoCodecId::H265) {
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, average_bitrate);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, peak_bitrate);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, vbv_buffer_size);
    } else {
        encoder_->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, average_bitrate);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, peak_bitrate);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, vbv_buffer_size);
    }
}

EncoderInfo AmfEncoder::info() const {
    return {Backend::AMF, codec_, width_, height_};
}

} // namespace parties::encdec::amd
