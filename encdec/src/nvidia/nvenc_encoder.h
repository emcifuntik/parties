#pragma once

#include <encdec/encoder.h>
#include "nvEncodeAPI.h"

#include <d3d11.h>
#include <wrl/client.h>

namespace parties::encdec::nvidia {

class NvencEncoder final : public Encoder {
public:
    NvencEncoder();
    ~NvencEncoder() override;

    bool init(ID3D11Device* device, uint32_t width, uint32_t height,
              uint32_t fps, uint32_t average_bps, VideoCodecId preferred_codec);

    bool encode(ID3D11Texture2D* bgra_texture, int64_t timestamp_100ns) override;

    bool supports_registered_input() const override { return true; }
    int  register_input(ID3D11Texture2D* texture) override;
    void unregister_inputs() override;
    bool encode_registered(int slot, int64_t timestamp_100ns) override;

    void force_keyframe() override;
    void set_average_bitrate_bps(uint32_t average_bps) override;
    EncoderInfo info() const override;

private:
    bool try_codec(const GUID& codec_guid, VideoCodecId id);
    bool do_encode(NV_ENC_REGISTERED_PTR resource, int64_t timestamp_100ns);

    NV_ENCODE_API_FUNCTION_LIST funcs_{};
    void* encoder_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_texture_;

    NV_ENC_REGISTERED_PTR registered_resource_ = nullptr;
    NV_ENC_OUTPUT_PTR output_bitstream_ = nullptr;

    static constexpr int MAX_EXTERNAL_INPUTS = 4;
    NV_ENC_REGISTERED_PTR external_inputs_[MAX_EXTERNAL_INPUTS]{};
    int num_external_inputs_ = 0;

    NV_ENC_INITIALIZE_PARAMS init_params_{};
    NV_ENC_CONFIG encode_config_{};

    VideoCodecId codec_ = VideoCodecId::H264;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t fps_ = 30;
    bool initialized_ = false;
    bool force_keyframe_ = false;
};

}
