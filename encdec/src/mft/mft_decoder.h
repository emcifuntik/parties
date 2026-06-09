#pragma once

#include <encdec/decoder.h>

#include <mfapi.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <vector>

namespace parties::encdec::mft {

class MftDecoder final : public Decoder {
public:
    ~MftDecoder() override;

    bool init(VideoCodecId codec, uint32_t width, uint32_t height);

    bool decode(const uint8_t* data, size_t len, int64_t timestamp) override;
    void flush() override;
    DecoderInfo info() const override;

private:
    void collect_output();

    Microsoft::WRL::ComPtr<IMFTransform> mft_;
    bool provides_samples_ = false;
    DWORD out_buf_size_ = 0;

    VideoCodecId codec_ = VideoCodecId::H264;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t nv12_stride_ = 0;
    bool initialized_ = false;
    bool mf_started_ = false;  // MFStartup succeeded — MFShutdown owed even if init() failed later

    std::vector<uint8_t> i420_buffer_;
};

} // namespace parties::encdec::mft
