#pragma once

#include <encdec/decoder.h>

class ISVCDecoder;

namespace parties::encdec::openh264 {

class OpenH264Decoder final : public Decoder {
public:
    ~OpenH264Decoder() override;

    bool init(VideoCodecId codec, uint32_t width, uint32_t height);

    bool decode(const uint8_t* data, size_t len, int64_t timestamp) override;
    void flush() override;
    DecoderInfo info() const override;

private:
    ISVCDecoder* decoder_ = nullptr;
};

}
