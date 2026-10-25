#pragma once

#include <encdec/encoder.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <vector>

class ISVCEncoder;

namespace parties::encdec::openh264 {

class OpenH264Encoder final : public Encoder {
public:
    ~OpenH264Encoder() override;

    bool init(ID3D11Device* device, uint32_t width, uint32_t height,
              uint32_t fps, uint32_t average_bps);

    bool encode(ID3D11Texture2D* bgra_texture, int64_t timestamp_100ns) override;
    void force_keyframe() override;
    void set_average_bitrate_bps(uint32_t average_bps) override;
    EncoderInfo info() const override;

private:
    ISVCEncoder* encoder_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;

    std::vector<uint8_t> i420_buffer_;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t fps_ = 30;
    bool force_keyframe_ = false;
};

}
