#pragma once

#include <encdec/decoder.h>

struct Dav1dContext;

namespace parties::encdec::dav1d {

class Dav1dDecoder final : public Decoder {
public:
    ~Dav1dDecoder() override;

    bool init(VideoCodecId codec, uint32_t width, uint32_t height);

    bool decode(const uint8_t* data, size_t len, int64_t timestamp) override;
    void flush() override;
    DecoderInfo info() const override;

private:
    void drain();

    Dav1dContext* ctx_ = nullptr;
    VideoCodecId codec_ = VideoCodecId::AV1;
};

}
