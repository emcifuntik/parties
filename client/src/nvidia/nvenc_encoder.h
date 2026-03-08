// NVENC hardware encoder wrapper using D3D11 device directly.
// Falls back gracefully — caller should try MFT if init() returns false.
#pragma once

#include "nvEncodeAPI.h"
#include <parties/video_common.h>

#include <cstdint>
#include <functional>
#include <d3d11.h>
#include <wrl/client.h>

namespace parties::client::nvidia {

class NvencEncoder {
public:
    NvencEncoder();
    ~NvencEncoder();

    // Initialize NVENC with a D3D11 device. Returns false if NVENC unavailable.
    bool init(ID3D11Device* device, uint32_t width, uint32_t height,
              uint32_t fps, uint32_t bitrate,
              parties::VideoCodecId preferred_codec);
    void shutdown();

    // Encode a BGRA texture. NVENC handles BGRA→NV12 internally.
    bool encode_frame(ID3D11Texture2D* bgra_texture, int64_t timestamp_100ns);

    void force_keyframe();
    void set_bitrate(uint32_t bitrate);

    parties::VideoCodecId codec() const { return codec_; }

    // Callback with encoded bitstream
    std::function<void(const uint8_t* data, size_t len, bool keyframe)> on_encoded;

private:
    bool try_codec(const GUID& codec_guid, parties::VideoCodecId id);

    NV_ENCODE_API_FUNCTION_LIST funcs_{};
    void* encoder_ = nullptr;  // NV_ENC_HANDLE

    // D3D11 staging texture (shared format with capture)
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_texture_;

    // NVENC resources
    NV_ENC_REGISTERED_PTR registered_resource_ = nullptr;
    NV_ENC_OUTPUT_PTR output_bitstream_ = nullptr;

    NV_ENC_INITIALIZE_PARAMS init_params_{};
    NV_ENC_CONFIG encode_config_{};

    parties::VideoCodecId codec_ = parties::VideoCodecId::H264;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t fps_ = 30;
    bool initialized_ = false;
    bool force_keyframe_ = false;
};

} // namespace parties::client::nvidia
