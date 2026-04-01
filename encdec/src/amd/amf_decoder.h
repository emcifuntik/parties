#pragma once

#include <encdec/decoder.h>

#include <AMF/core/Factory.h>
#include <AMF/core/Context.h>
#include <AMF/components/Component.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace parties::encdec::amd {

class AmfDecoder final : public Decoder {
public:
    AmfDecoder();
    ~AmfDecoder() override;

    bool init(VideoCodecId codec, uint32_t width, uint32_t height);

    bool decode(const uint8_t* data, size_t len, int64_t timestamp) override;
    void flush() override;
    bool context_lost() const override { return context_lost_; }
    DecoderInfo info() const override;

private:
    bool check_device_health();

    amf::AMFFactory* factory_ = nullptr;
    amf::AMFContext* context_ = nullptr;
    amf::AMFComponent* decoder_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;

    VideoCodecId codec_ = VideoCodecId::AV1;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool initialized_ = false;
    bool context_lost_ = false;
};

} // namespace parties::encdec::amd
