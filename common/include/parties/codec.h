#pragma once

#include <cstdint>

struct OpusEncoder;
struct OpusDecoder;

namespace parties {

enum class OpusMode { Voice, Music };

class OpusCodec {
public:
    OpusCodec();
    ~OpusCodec();

    bool init_encoder(int sample_rate, int channels, int bitrate,
                      bool inband_fec = false, int expected_loss_pct = 0,
                      OpusMode mode = OpusMode::Voice);
    bool init_decoder(int sample_rate, int channels);

    int encode(const float* pcm_in, int frame_size,
               uint8_t* opus_out, int max_opus_bytes);

    int decode(const uint8_t* opus_in, int opus_len,
               float* pcm_out, int max_frame_size, int decode_fec = 0);

    void set_bitrate(int bps);

private:
    OpusEncoder* encoder_ = nullptr;
    OpusDecoder* decoder_ = nullptr;
};

}
