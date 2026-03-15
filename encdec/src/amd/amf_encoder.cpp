#include "amf_encoder.h"
#include "amf_loader.h"

#include <AMF/components/VideoEncoderVCE.h>
#include <AMF/components/VideoEncoderHEVC.h>
#include <AMF/components/VideoEncoderAV1.h>

#include <parties/profiler.h>
#include <cstdio>

namespace parties::encdec::amd {

AmfEncoder::AmfEncoder() = default;

AmfEncoder::~AmfEncoder() {
    if (!initialized_) return;

    if (encoder_) {
        encoder_->Drain();
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
        std::fprintf(stderr, "[AMF] CreateContext failed: %d\n", res);
        return false;
    }

    res = context_->InitDX11(device);
    if (res != AMF_OK) {
        std::fprintf(stderr, "[AMF] InitDX11 failed: %d\n", res);
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
        std::fprintf(stderr, "[AMF] No supported encoder codec found\n");
        context_->Release();
        context_ = nullptr;
        return false;
    }

    std::fprintf(stderr, "[AMF] Selected encoder codec: %s (%ux%u @ %u fps)\n",
                 codec_name(codec_), width, height, fps);

    if (codec_ == VideoCodecId::AV1) {
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_USAGE,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_USAGE_LOW_LATENCY));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, static_cast<amf_int64>(bitrate));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, static_cast<amf_int64>(bitrate * 5));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_FRAMERATE, AMFConstructRate(fps, 1));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_GOP_SIZE,
            static_cast<amf_int64>(fps * (VIDEO_KEYFRAME_INTERVAL_MS / 1000)));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_SPEED));
    } else if (codec_ == VideoCodecId::H265) {
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, static_cast<amf_int64>(bitrate));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, static_cast<amf_int64>(bitrate * 5));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, AMFConstructRate(fps, 1));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE,
            static_cast<amf_int64>(fps * (VIDEO_KEYFRAME_INTERVAL_MS / 1000)));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED));
    } else {
        encoder_->SetProperty(AMF_VIDEO_ENCODER_USAGE,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, static_cast<amf_int64>(bitrate));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, static_cast<amf_int64>(bitrate * 5));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, AMFConstructRate(fps, 1));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD,
            static_cast<amf_int64>(fps * (VIDEO_KEYFRAME_INTERVAL_MS / 1000)));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, static_cast<amf_int64>(0));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED));
    }

    res = encoder_->Init(amf::AMF_SURFACE_BGRA, width, height);
    if (res != AMF_OK) {
        std::fprintf(stderr, "[AMF] Encoder Init(BGRA) failed: %d\n", res);
        encoder_->Release();
        encoder_ = nullptr;
        context_->Release();
        context_ = nullptr;
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc = {1, 0};
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &staging_texture_);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[AMF] CreateTexture2D staging failed: 0x%08lx\n", hr);
        encoder_->Terminate();
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
    if (!initialized_) return false;

    d3d_context_->CopyResource(staging_texture_.Get(), bgra_texture);
    d3d_context_->Flush();

    return do_encode(staging_texture_.Get(), timestamp_100ns);
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
        std::fprintf(stderr, "[AMF] CreateSurfaceFromDX11Native failed: %d\n", res);
        return false;
    }

    surface->SetPts(timestamp_100ns);

    if (force_keyframe_) {
        force_keyframe_ = false;
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

    res = encoder_->SubmitInput(surface);
    surface->Release();

    if (res != AMF_OK) {
        if (res == AMF_INPUT_FULL) return true;
        std::fprintf(stderr, "[AMF] SubmitInput failed: %d\n", res);
        return false;
    }

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

    if (codec_ == VideoCodecId::AV1)
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, static_cast<amf_int64>(bitrate));
    else if (codec_ == VideoCodecId::H265)
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, static_cast<amf_int64>(bitrate));
    else
        encoder_->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, static_cast<amf_int64>(bitrate));
}

EncoderInfo AmfEncoder::info() const {
    return {Backend::AMF, codec_, width_, height_};
}

} // namespace parties::encdec::amd
