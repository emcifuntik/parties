#include "nvenc_encoder.h"
#include "nvidia_loader.h"
#include <encdec/rate_control.h>

#include <cstring>
#include <parties/log.h>
#include <parties/profiler.h>

namespace parties::encdec::nvidia {

NvencEncoder::NvencEncoder() = default;

NvencEncoder::~NvencEncoder() {
    if (!initialized_) return;

    NV_ENC_PIC_PARAMS eos{};
    eos.version = NV_ENC_PIC_PARAMS_VER;
    eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    funcs_.nvEncEncodePicture(encoder_, &eos);

    if (output_bitstream_) {
        funcs_.nvEncDestroyBitstreamBuffer(encoder_, output_bitstream_);
        output_bitstream_ = nullptr;
    }

    unregister_inputs();

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

bool NvencEncoder::init(ID3D11Device* device, uint32_t width, uint32_t height,
                         uint32_t fps, uint32_t bitrate, VideoCodecId preferred_codec) {
    ZoneScopedN("NvencEncoder::init");
    if (initialized_) return false;
    if (!load_nvenc(funcs_)) return false;

    device_ = device;
    device_->GetImmediateContext(&context_);
    width_ = width;
    height_ = height;
    fps_ = fps;

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session_params{};
    session_params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    session_params.device = device;
    session_params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    session_params.apiVersion = NVENCAPI_VERSION;

    NVENCSTATUS status = funcs_.nvEncOpenEncodeSessionEx(&session_params, &encoder_);
    if (status != NV_ENC_SUCCESS) {
        LOG_ERROR("OpenEncodeSessionEx failed: {}", (int)status);
        return false;
    }

    struct CodecEntry { GUID guid; VideoCodecId id; };
    CodecEntry codecs[] = {
        {NV_ENC_CODEC_AV1_GUID,  VideoCodecId::AV1},
        {NV_ENC_CODEC_HEVC_GUID, VideoCodecId::H265},
        {NV_ENC_CODEC_H264_GUID, VideoCodecId::H264},
    };

    bool found = false;
    for (auto& c : codecs) {
        if (c.id == preferred_codec && try_codec(c.guid, c.id)) { found = true; break; }
    }
    if (!found) {
        for (auto& c : codecs) {
            if (c.id != preferred_codec && try_codec(c.guid, c.id)) { found = true; break; }
        }
    }

    if (!found) {
        LOG_ERROR("No supported codec found");
        funcs_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
        return false;
    }

    const auto rate_control = make_stream_vbr_rate_control(bitrate);
    LOG_INFO("Selected codec: {} ({}x{} @ {} fps), VBR average: {} bps, peak: {} bps",
             codec_name(codec_), width, height, fps,
             rate_control.average_bitrate, rate_control.peak_bitrate);

    GUID encode_guid = (codec_ == VideoCodecId::AV1)  ? NV_ENC_CODEC_AV1_GUID
                     : (codec_ == VideoCodecId::H265) ? NV_ENC_CODEC_HEVC_GUID
                                                       : NV_ENC_CODEC_H264_GUID;

    NV_ENC_PRESET_CONFIG preset_config{};
    preset_config.version = NV_ENC_PRESET_CONFIG_VER;
    preset_config.presetCfg.version = NV_ENC_CONFIG_VER;

    status = funcs_.nvEncGetEncodePresetConfigEx(
        encoder_, encode_guid,
        NV_ENC_PRESET_P4_GUID, NV_ENC_TUNING_INFO_LOW_LATENCY,
        &preset_config);
    if (status != NV_ENC_SUCCESS) {
        LOG_ERROR("GetEncodePresetConfigEx failed: {}", (int)status);
        funcs_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
        return false;
    }

    encode_config_ = preset_config.presetCfg;
    encode_config_.version = NV_ENC_CONFIG_VER;
    encode_config_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    encode_config_.rcParams.averageBitRate = rate_control.average_bitrate;
    encode_config_.rcParams.maxBitRate = rate_control.peak_bitrate;
    encode_config_.rcParams.vbvBufferSize = rate_control.vbv_buffer_size;
    encode_config_.rcParams.vbvInitialDelay = rate_control.vbv_initial_delay;
    encode_config_.rcParams.multiPass = NV_ENC_TWO_PASS_QUARTER_RESOLUTION;
    encode_config_.rcParams.enableMinQP = 0;
    encode_config_.rcParams.enableMaxQP = 0;
    encode_config_.gopLength = fps * (VIDEO_KEYFRAME_INTERVAL_MS / 1000);
    encode_config_.frameIntervalP = 1;

    // Output sequence headers with every keyframe so decoders can join mid-stream
    if (codec_ == VideoCodecId::AV1)
        encode_config_.encodeCodecConfig.av1Config.repeatSeqHdr = 1;
    else if (codec_ == VideoCodecId::H265)
        encode_config_.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
    else
        encode_config_.encodeCodecConfig.h264Config.repeatSPSPPS = 1;

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
    init_params_.enablePTD = 1;
    init_params_.encodeConfig = &encode_config_;
    init_params_.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    init_params_.maxEncodeWidth = width;
    init_params_.maxEncodeHeight = height;

    status = funcs_.nvEncInitializeEncoder(encoder_, &init_params_);
    if (status != NV_ENC_SUCCESS) {
        LOG_ERROR("InitializeEncoder failed: {}", (int)status);
        funcs_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;

    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &staging_texture_);
    if (FAILED(hr)) {
        LOG_ERROR("CreateTexture2D staging failed: {:#010x}", hr);
        funcs_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
        return false;
    }

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
        LOG_ERROR("RegisterResource failed: {}", (int)status);
        funcs_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
        return false;
    }
    registered_resource_ = reg.registeredResource;

    NV_ENC_CREATE_BITSTREAM_BUFFER bsb{};
    bsb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;

    status = funcs_.nvEncCreateBitstreamBuffer(encoder_, &bsb);
    if (status != NV_ENC_SUCCESS) {
        LOG_ERROR("CreateBitstreamBuffer failed: {}", (int)status);
        funcs_.nvEncUnregisterResource(encoder_, registered_resource_);
        funcs_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
        return false;
    }
    output_bitstream_ = bsb.bitstreamBuffer;

    initialized_ = true;
    return true;
}

bool NvencEncoder::try_codec(const GUID& codec_guid, VideoCodecId id) {
    uint32_t guid_count = 0;
    NVENCSTATUS status = funcs_.nvEncGetEncodeGUIDCount(encoder_, &guid_count);
    if (status != NV_ENC_SUCCESS || guid_count == 0) return false;

    std::vector<GUID> guids(guid_count);
    uint32_t actual = 0;
    status = funcs_.nvEncGetEncodeGUIDs(encoder_, guids.data(), guid_count, &actual);
    if (status != NV_ENC_SUCCESS) return false;

    bool found = false;
    for (uint32_t i = 0; i < actual; i++) {
        if (memcmp(&guids[i], &codec_guid, sizeof(GUID)) == 0) { found = true; break; }
    }
    if (!found) return false;

    uint32_t fmt_count = 0;
    status = funcs_.nvEncGetInputFormatCount(encoder_, codec_guid, &fmt_count);
    if (status != NV_ENC_SUCCESS || fmt_count == 0) return false;

    std::vector<NV_ENC_BUFFER_FORMAT> fmts(fmt_count);
    uint32_t fmt_actual = 0;
    status = funcs_.nvEncGetInputFormats(encoder_, codec_guid, fmts.data(), fmt_count, &fmt_actual);
    if (status != NV_ENC_SUCCESS) return false;

    for (uint32_t i = 0; i < fmt_actual; i++) {
        if (fmts[i] == NV_ENC_BUFFER_FORMAT_ARGB) { codec_ = id; return true; }
    }
    return false;
}

bool NvencEncoder::do_encode(NV_ENC_REGISTERED_PTR resource, int64_t timestamp_100ns) {
    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = resource;

    NVENCSTATUS status = funcs_.nvEncMapInputResource(encoder_, &map);
    if (status != NV_ENC_SUCCESS) {
        LOG_ERROR("MapInputResource failed: {}", (int)status);
        return false;
    }

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

    if (status != NV_ENC_SUCCESS) {
        funcs_.nvEncUnmapInputResource(encoder_, map.mappedResource);
        LOG_ERROR("EncodePicture failed: {}", (int)status);
        return false;
    }

    NV_ENC_LOCK_BITSTREAM lock{};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = output_bitstream_;

    {
        ZoneScopedN("nvenc::lock_bitstream");
        status = funcs_.nvEncLockBitstream(encoder_, &lock);
    }
    if (status != NV_ENC_SUCCESS) {
        funcs_.nvEncUnmapInputResource(encoder_, map.mappedResource);
        LOG_ERROR("LockBitstream failed: {}", (int)status);
        return false;
    }

    // nvEncodeAPI requires a mapped input to remain valid until the blocking
    // bitstream lock confirms completion of the encode that consumes it.
    const NVENCSTATUS unmap_status =
        funcs_.nvEncUnmapInputResource(encoder_, map.mappedResource);
    if (unmap_status != NV_ENC_SUCCESS)
        LOG_ERROR("UnmapInputResource failed: {}", (int)unmap_status);

    bool keyframe = (lock.pictureType == NV_ENC_PIC_TYPE_IDR ||
                     lock.pictureType == NV_ENC_PIC_TYPE_I);

    if (on_encoded && lock.bitstreamBufferPtr && lock.bitstreamSizeInBytes > 0) {
        on_encoded(static_cast<const uint8_t*>(lock.bitstreamBufferPtr),
                   lock.bitstreamSizeInBytes, keyframe);
    }

    const NVENCSTATUS unlock_status =
        funcs_.nvEncUnlockBitstream(encoder_, output_bitstream_);
    if (unlock_status != NV_ENC_SUCCESS)
        LOG_ERROR("UnlockBitstream failed: {}", (int)unlock_status);
    return unmap_status == NV_ENC_SUCCESS && unlock_status == NV_ENC_SUCCESS;
}

bool NvencEncoder::encode(ID3D11Texture2D* bgra_texture, int64_t timestamp_100ns) {
    ZoneScopedN("NvencEncoder::encode");
    if (!initialized_) return false;

    {
        ZoneScopedN("nvenc::copy_texture");
        context_->CopyResource(staging_texture_.Get(), bgra_texture);
    }

    return do_encode(registered_resource_, timestamp_100ns);
}

int NvencEncoder::register_input(ID3D11Texture2D* texture) {
    if (!initialized_ || num_external_inputs_ >= MAX_EXTERNAL_INPUTS) return -1;

    NV_ENC_REGISTER_RESOURCE reg{};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.resourceToRegister = texture;
    reg.width = width_;
    reg.height = height_;
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;

    NVENCSTATUS status = funcs_.nvEncRegisterResource(encoder_, &reg);
    if (status != NV_ENC_SUCCESS) {
        LOG_ERROR("RegisterResource (external {}) failed: {}",
                  num_external_inputs_, (int)status);
        return -1;
    }

    int slot = num_external_inputs_++;
    external_inputs_[slot] = reg.registeredResource;
    return slot;
}

void NvencEncoder::unregister_inputs() {
    for (int i = 0; i < num_external_inputs_; i++) {
        if (external_inputs_[i]) {
            funcs_.nvEncUnregisterResource(encoder_, external_inputs_[i]);
            external_inputs_[i] = nullptr;
        }
    }
    num_external_inputs_ = 0;
}

bool NvencEncoder::encode_registered(int slot, int64_t timestamp_100ns) {
    ZoneScopedN("NvencEncoder::encode_registered");
    if (!initialized_ || slot < 0 || slot >= num_external_inputs_) return false;
    return do_encode(external_inputs_[slot], timestamp_100ns);
}

void NvencEncoder::force_keyframe() {
    force_keyframe_ = true;
}

void NvencEncoder::set_bitrate(uint32_t bitrate) {
    if (!initialized_) return;

    const auto rate_control = make_stream_vbr_rate_control(bitrate);
    encode_config_.rcParams.averageBitRate = rate_control.average_bitrate;
    encode_config_.rcParams.maxBitRate = rate_control.peak_bitrate;
    encode_config_.rcParams.vbvBufferSize = rate_control.vbv_buffer_size;
    encode_config_.rcParams.vbvInitialDelay = rate_control.vbv_initial_delay;

    NV_ENC_RECONFIGURE_PARAMS reconfig{};
    reconfig.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    reconfig.reInitEncodeParams = init_params_;
    reconfig.reInitEncodeParams.encodeConfig = &encode_config_;

    NVENCSTATUS status = funcs_.nvEncReconfigureEncoder(encoder_, &reconfig);
    if (status != NV_ENC_SUCCESS) {
        LOG_ERROR("ReconfigureEncoder failed: {}", (int)status);
    }
}

EncoderInfo NvencEncoder::info() const {
    return {Backend::NVENC, codec_, width_, height_};
}

} // namespace parties::encdec::nvidia
