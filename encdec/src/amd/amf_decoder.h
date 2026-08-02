#pragma once

#include <encdec/decoder.h>

#include <AMF/core/Factory.h>
#include <AMF/core/Context.h>
#include <AMF/components/Component.h>

#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace parties::encdec::amd {

struct AmfNativeLifetime;
struct AmfD3D11InteropState;

class AmfDecoder final : public Decoder {
public:
    AmfDecoder();
    ~AmfDecoder() override;

    bool init(VideoCodecId codec, uint32_t width, uint32_t height,
              ID3D12Device* render_device = nullptr);

    bool decode(const uint8_t* data, size_t len, int64_t timestamp) override;
    void flush() override;
    bool context_lost() const override { return context_lost_; }
    DecoderInfo info() const override;

private:
    bool check_device_health();
    size_t poll_output(bool deliver_frames);
    bool deliver_output(amf::AMFData* data);
    void mark_context_lost(const char* operation, AMF_RESULT result);

    amf::AMFFactory* factory_ = nullptr;
    amf::AMFContext* context_ = nullptr;
    amf::AMFComponent* decoder_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> device_context_;
    Microsoft::WRL::ComPtr<ID3D12Device> d3d12_device_;

    VideoCodecId codec_ = VideoCodecId::AV1;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool initialized_ = false;
    bool context_lost_ = false;
    bool native_d3d12_output_ = false;
    std::shared_ptr<AmfNativeLifetime> native_lifetime_;
    std::shared_ptr<AmfD3D11InteropState> interop_;
};

} // namespace parties::encdec::amd
