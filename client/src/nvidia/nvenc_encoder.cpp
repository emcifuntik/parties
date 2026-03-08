// NVENC hardware encoder implementation using D3D11 device directly.
#include "nvenc_encoder.h"
#include "nvidia_loader.h"

#include <cstdio>
#include <cstring>
#include <parties/profiler.h>

namespace parties::client::nvidia {

NvencEncoder::NvencEncoder() = default;

NvencEncoder::~NvencEncoder() {
    shutdown();
}

bool NvencEncoder::init(ID3D11Device* device, uint32_t width, uint32_t height,
                         uint32_t fps, uint32_t bitrate,
                         parties::VideoCodecId preferred_codec) {
    ZoneScopedN("NvencEncoder::init");
    if (initialized_) return false;

    // Load NVENC DLL
    if (!load_nvenc(funcs_)) return false;

    device_ = device;
    device_->GetImmediateContext(&context_);
    width_ = width;
    height_ = height;
    fps_ = fps;

    // Open encode session with D3D11 device
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session_params{};
    session_params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    session_params.device = device;
    session_params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    session_params.apiVersion = NVENCAPI_VERSION;

    NVENCSTATUS status = funcs_.nvEncOpenEncodeSessionEx(&session_params, &encoder_);
    if (status != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "[NVENC] OpenEncodeSessionEx failed: %d\n", status);
        return false;
    }

    // Try codecs in priority order (preferred first)
    struct CodecEntry { GUID guid; parties::VideoCodecId id; };
    CodecEntry codecs[] = {
        {NV_ENC_CODEC_AV1_GUID,  parties::VideoCodecId::AV1},
        {NV_ENC_CODEC_HEVC_GUID, parties::VideoCodecId::H265},
        {NV_ENC_CODEC_H264_GUID, parties::VideoCodecId::H264},
    };

    bool found = false;
    for (auto& c : codecs) {
        if (c.id == preferred_codec && try_codec(c.guid, c.id)) {
            found = true;
            break;
        }
    }
    if (!found) {
        for (auto& c : codecs) {
            if (c.id != preferred_codec && try_codec(c.guid, c.id)) {
                found = true;
                break;
            }
        }
    }

    if (!found) {
        std::fprintf(stderr, "[NVENC] No supported codec found\n");
        funcs_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
        return false;
    }

    // Build codec GUID from try_codec result
    GUID encode_guid = (codec_ == parties::VideoCodecId::AV1)  ? NV_ENC_CODEC_AV1_GUID
                      : (codec_ == parties::VideoCodecId::H265) ? NV_ENC_CODEC_HEVC_GUID
                                                                 : NV_ENC_CODEC_H264_GUID;

    // Get preset config for low-latency tuning
    NV_ENC_PRESET_CONFIG preset_config{};
    preset_config.version = NV_ENC_PRESET_CONFIG_VER;
    preset_config.presetCfg.version = NV_ENC_CONFIG_VER;

    status = funcs_.nvEncGetEncodePresetConfigEx(
        encoder_, encode_guid,
        NV_ENC_PRESET_P4_GUID, NV_ENC_TUNING_INFO_LOW_LATENCY,
        &preset_config);
    if (status != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "[NVENC] GetEncodePresetConfigEx failed: %d\n", status);
        funcs_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
        return false;
    }

    encode_config_ = preset_config.presetCfg;
    encode_config_.version = NV_ENC_CONFIG_VER;
    encode_config_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    encode_config_.rcParams.averageBitRate = bitrate;
    encode_config_.rcParams.maxBitRate = bitrate;
    encode_config_.rcParams.vbvBufferSize = bitrate / fps;  // ~1 frame
    encode_config_.rcParams.vbvInitialDelay = encode_config_.rcParams.vbvBufferSize;
    encode_config_.gopLength = fps * (VIDEO_KEYFRAME_INTERVAL_MS / 1000);
    encode_config_.frameIntervalP = 1;  // No B-frames

    // Initialize encoder
    std::memset(&init_params_, 0, sizeof(init_params_));
    init_params_.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init_params_.encodeGUID = encode_guid;
    init_params_.presetGUID = NV_ENC_PRESET_P4_GUID;
    init_params_.encodeWidth = width;
    init_params_.encodeHeight = height;
    init_params_.darWidth = width;
    init_params_.darHeight = height;
    init_params_.frameRateNum = fps;
    init_params_.frameRateDen = 1;
    init_params_.enablePTD = 1;  // Let NVENC decide picture type
    init_params_.encodeConfig = &encode_config_;
    init_params_.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    init_params_.maxEncodeWidth = width;
    init_params_.maxEncodeHeight = height;

    status = funcs_.nvEncInitializeEncoder(encoder_, &init_params_);
    if (status != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "[NVENC] InitializeEncoder failed: %d\n", status);
        funcs_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
        return false;
    }

    // Create staging texture for BGRA input
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;

    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &staging_texture_);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[NVENC] CreateTexture2D staging failed: 0x%08lx\n", hr);
        funcs_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
        return false;
    }

    // Register the staging texture as NVENC input resource
    NV_ENC_REGISTER_RESOURCE reg{};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.resourceToRegister = staging_texture_.Get();
    reg.width = width;
    reg.height = height;
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;

    status = funcs_.nvEncRegisterResource(encoder_, &reg);
    if (status != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "[NVENC] RegisterResource failed: %d\n", status);
        funcs_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
        return false;
    }
    registered_resource_ = reg.registeredResource;

    // Create output bitstream buffer
    NV_ENC_CREATE_BITSTREAM_BUFFER bsb{};
    bsb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;

    status = funcs_.nvEncCreateBitstreamBuffer(encoder_, &bsb);
    if (status != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "[NVENC] CreateBitstreamBuffer failed: %d\n", status);
        funcs_.nvEncUnregisterResource(encoder_, registered_resource_);
        funcs_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
        return false;
    }
    output_bitstream_ = bsb.bitstreamBuffer;

    const char* codec_name = (codec_ == parties::VideoCodecId::AV1)  ? "AV1"
                           : (codec_ == parties::VideoCodecId::H265) ? "H.265"
                                                                      : "H.264";
    std::fprintf(stderr, "[NVENC] Initialized %s encoder %ux%u @ %u fps, %u kbps\n",
                 codec_name, width, height, fps, bitrate / 1000);

    initialized_ = true;
    return true;
}

void NvencEncoder::shutdown() {
    if (!initialized_) return;

    // Send EOS
    NV_ENC_PIC_PARAMS eos{};
    eos.version = NV_ENC_PIC_PARAMS_VER;
    eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    funcs_.nvEncEncodePicture(encoder_, &eos);

    if (output_bitstream_) {
        funcs_.nvEncDestroyBitstreamBuffer(encoder_, output_bitstream_);
        output_bitstream_ = nullptr;
    }

    if (registered_resource_) {
        funcs_.nvEncUnregisterResource(encoder_, registered_resource_);
        registered_resource_ = nullptr;
    }

    staging_texture_.Reset();

    if (encoder_) {
        funcs_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
    }

    context_.Reset();
    device_.Reset();
    initialized_ = false;
}

bool NvencEncoder::try_codec(const GUID& codec_guid, parties::VideoCodecId id) {
    // Check if this codec is supported by enumerating supported codecs
    uint32_t guid_count = 0;
    NVENCSTATUS status = funcs_.nvEncGetEncodeGUIDCount(encoder_, &guid_count);
    if (status != NV_ENC_SUCCESS || guid_count == 0) return false;

    std::vector<GUID> guids(guid_count);
    uint32_t actual = 0;
    status = funcs_.nvEncGetEncodeGUIDs(encoder_, guids.data(), guid_count, &actual);
    if (status != NV_ENC_SUCCESS) return false;

    bool found = false;
    for (uint32_t i = 0; i < actual; i++) {
        if (memcmp(&guids[i], &codec_guid, sizeof(GUID)) == 0) {
            found = true;
            break;
        }
    }

    if (!found) return false;

    // Check if ARGB input format is supported
    uint32_t fmt_count = 0;
    status = funcs_.nvEncGetInputFormatCount(encoder_, codec_guid, &fmt_count);
    if (status != NV_ENC_SUCCESS || fmt_count == 0) return false;

    std::vector<NV_ENC_BUFFER_FORMAT> fmts(fmt_count);
    uint32_t fmt_actual = 0;
    status = funcs_.nvEncGetInputFormats(encoder_, codec_guid, fmts.data(), fmt_count, &fmt_actual);
    if (status != NV_ENC_SUCCESS) return false;

    bool argb_ok = false;
    for (uint32_t i = 0; i < fmt_actual; i++) {
        if (fmts[i] == NV_ENC_BUFFER_FORMAT_ARGB) {
            argb_ok = true;
            break;
        }
    }

    if (!argb_ok) return false;

    codec_ = id;
    return true;
}

bool NvencEncoder::encode_frame(ID3D11Texture2D* bgra_texture, int64_t timestamp_100ns) {
    ZoneScopedN("NvencEncoder::encode_frame");
    if (!initialized_) return false;

    // Copy capture texture to our staging texture
    {
        ZoneScopedN("nvenc::copy_texture");
        context_->CopyResource(staging_texture_.Get(), bgra_texture);
    }

    // Map the registered resource
    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = registered_resource_;

    NVENCSTATUS status = funcs_.nvEncMapInputResource(encoder_, &map);
    if (status != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "[NVENC] MapInputResource failed: %d\n", status);
        return false;
    }

    // Encode
    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputWidth = width_;
    pic.inputHeight = height_;
    pic.inputBuffer = map.mappedResource;
    pic.outputBitstream = output_bitstream_;
    pic.bufferFmt = map.mappedBufferFmt;
    pic.inputTimeStamp = static_cast<uint64_t>(timestamp_100ns);

    if (force_keyframe_) {
        pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        force_keyframe_ = false;
    }

    {
        ZoneScopedN("nvenc::encode");
        status = funcs_.nvEncEncodePicture(encoder_, &pic);
    }

    // Unmap regardless of encode result
    funcs_.nvEncUnmapInputResource(encoder_, map.mappedResource);

    if (status != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "[NVENC] EncodePicture failed: %d\n", status);
        return false;
    }

    // Lock and read bitstream output
    NV_ENC_LOCK_BITSTREAM lock{};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = output_bitstream_;

    status = funcs_.nvEncLockBitstream(encoder_, &lock);
    if (status != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "[NVENC] LockBitstream failed: %d\n", status);
        return false;
    }

    bool keyframe = (lock.pictureType == NV_ENC_PIC_TYPE_IDR ||
                     lock.pictureType == NV_ENC_PIC_TYPE_I);

    if (on_encoded && lock.bitstreamBufferPtr && lock.bitstreamSizeInBytes > 0) {
        on_encoded(static_cast<const uint8_t*>(lock.bitstreamBufferPtr),
                   lock.bitstreamSizeInBytes, keyframe);
    }

    funcs_.nvEncUnlockBitstream(encoder_, output_bitstream_);
    return true;
}

void NvencEncoder::force_keyframe() {
    force_keyframe_ = true;
}

void NvencEncoder::set_bitrate(uint32_t bitrate) {
    if (!initialized_) return;

    encode_config_.rcParams.averageBitRate = bitrate;
    encode_config_.rcParams.maxBitRate = bitrate;

    NV_ENC_RECONFIGURE_PARAMS reconfig{};
    reconfig.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    reconfig.reInitEncodeParams = init_params_;
    reconfig.reInitEncodeParams.encodeConfig = &encode_config_;

    NVENCSTATUS status = funcs_.nvEncReconfigureEncoder(encoder_, &reconfig);
    if (status != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "[NVENC] ReconfigureEncoder failed: %d\n", status);
    }
}

} // namespace parties::client::nvidia
