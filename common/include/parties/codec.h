#pragma once

#include <parties/audio_common.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

struct OpusEncoder;
struct OpusDecoder;

namespace parties {

enum class OpusMode { Voice, Music };

struct OpusFec {
    int expected_loss_pct;
};

inline constexpr OpusFec kVoiceFec{audio::OPUS_EXPECTED_LOSS_PCT};

class OpusCodec {
public:
    OpusCodec();
    ~OpusCodec();
    OpusCodec(const OpusCodec&) = delete;
    OpusCodec& operator=(const OpusCodec&) = delete;

    bool init_encoder(int sample_rate, int channels, int bitrate_bps,
                      std::optional<OpusFec> fec, OpusMode mode = OpusMode::Voice);
    bool init_decoder(int sample_rate, int channels);

    [[nodiscard]] std::optional<size_t> encode(const float* pcm, int frame_size, std::span<uint8_t> out);

    [[nodiscard]] std::optional<int> decode(std::span<const uint8_t> packet, float* pcm, int max_frame_size);
    [[nodiscard]] std::optional<int> conceal_lost_frame(float* pcm, int frame_size);
    // Opus needs the successor itself decoded normally afterwards to keep its state continuous.
    [[nodiscard]] std::optional<int> recover_from_successor_fec(std::span<const uint8_t> successor,
                                                                float* pcm, int frame_size);

private:
    OpusEncoder* encoder_ = nullptr;
    OpusDecoder* decoder_ = nullptr;
};

}
