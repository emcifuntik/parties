#pragma once

#include <encdec/decoder.h>

#include <cstdint>
#include <vector>

namespace parties::encdec::libhevc {

class LibhevcDecoder final : public Decoder {
public:
    ~LibhevcDecoder() override;

    bool init(VideoCodecId codec, uint32_t width, uint32_t height);

    bool decode(const uint8_t* data, size_t len, int64_t timestamp) override;
    void flush() override;
    DecoderInfo info() const override;

private:
    bool set_decode_mode(int mode);
    bool set_num_cores();
    void drain_flush();

    void* codec_obj_ = nullptr;

    std::vector<uint8_t> y_buf_;
    std::vector<uint8_t> u_buf_;
    std::vector<uint8_t> v_buf_;
    uint32_t buf_width_ = 0;
    uint32_t buf_height_ = 0;
};

}
