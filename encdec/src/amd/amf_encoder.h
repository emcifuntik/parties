#pragma once

#include <encdec/encoder.h>

#include <AMF/core/Factory.h>
#include <AMF/core/Context.h>
#include <AMF/components/Component.h>

#include <d3d11.h>
#include <wrl/client.h>

namespace parties::encdec::amd {

class AmfEncoder final : public Encoder {
public:
    AmfEncoder();
    ~AmfEncoder() override;

    bool init(ID3D11Device* device, uint32_t width, uint32_t height,
              uint32_t fps, uint32_t bitrate, VideoCodecId preferred_codec);

    bool encode(ID3D11Texture2D* bgra_texture, int64_t timestamp_100ns) override;

    bool supports_registered_input() const override { return true; }
    int  register_input(ID3D11Texture2D* texture) override;
    void unregister_inputs() override;
    bool encode_registered(int slot, int64_t timestamp_100ns) override;

    void force_keyframe() override;
    void set_bitrate(uint32_t bitrate) override;
    EncoderInfo info() const override;

private:
    bool try_codec(const wchar_t* component_id, VideoCodecId id);
    bool ensure_staging_texture();
    bool do_encode(ID3D11Texture2D* texture, int64_t timestamp_100ns);
    bool deliver_output(amf::AMFData* data);

    amf::AMFFactory* factory_ = nullptr;
    amf::AMFContext* context_ = nullptr;
    amf::AMFComponent* encoder_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_texture_;

    static constexpr int MAX_EXTERNAL_INPUTS = 4;
    ID3D11Texture2D* external_inputs_[MAX_EXTERNAL_INPUTS]{};
    int num_external_inputs_ = 0;

    VideoCodecId codec_ = VideoCodecId::H264;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t fps_ = 30;
    bool initialized_ = false;
    bool force_keyframe_ = false;
};

} // namespace parties::encdec::amd
