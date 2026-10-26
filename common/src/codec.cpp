#include <parties/codec.h>
#include <parties/audio_common.h>
#include <parties/log.h>

#include <opus/opus.h>

namespace parties {

OpusCodec::OpusCodec() = default;

OpusCodec::~OpusCodec() {
    if (encoder_) opus_encoder_destroy(encoder_);
    if (decoder_) opus_decoder_destroy(decoder_);
}

bool OpusCodec::init_encoder(int sample_rate, int channels, int bitrate_bps,
                             std::optional<OpusFec> fec, OpusMode mode) {
    if (encoder_) {
        opus_encoder_destroy(encoder_);
        encoder_ = nullptr;
    }
    int err;
    const int application = (mode == OpusMode::Music) ? OPUS_APPLICATION_AUDIO
                                                      : OPUS_APPLICATION_VOIP;
    encoder_ = opus_encoder_create(sample_rate, channels, application, &err);
    if (err != OPUS_OK) {
        encoder_ = nullptr;
        return false;
    }
    opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate_bps));

    const int signal = (mode == OpusMode::Music) ? OPUS_SIGNAL_MUSIC : OPUS_SIGNAL_VOICE;

    if (fec) {
        opus_encoder_ctl(encoder_, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(encoder_, OPUS_SET_PACKET_LOSS_PERC(fec->expected_loss_pct));
        opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(signal));
        opus_encoder_ctl(encoder_, OPUS_SET_DTX(0));
        opus_encoder_ctl(encoder_, OPUS_SET_VBR(1));
        opus_encoder_ctl(encoder_, OPUS_SET_VBR_CONSTRAINT(1));
        opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(10));

        opus_int32 fec_on = 0;
        opus_encoder_ctl(encoder_, OPUS_GET_INBAND_FEC(&fec_on));
        if (!fec_on && mode == OpusMode::Voice)
            LOG_WARN("Opus in-band FEC was requested but did not enable");
    } else if (mode == OpusMode::Music) {
        opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(signal));
        opus_encoder_ctl(encoder_, OPUS_SET_DTX(0));
        opus_encoder_ctl(encoder_, OPUS_SET_VBR(1));
        opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(10));
    }
    return true;
}

bool OpusCodec::init_decoder(int sample_rate, int channels) {
    if (decoder_) {
        opus_decoder_destroy(decoder_);
        decoder_ = nullptr;
    }
    int err;
    decoder_ = opus_decoder_create(sample_rate, channels, &err);
    if (err != OPUS_OK) {
        decoder_ = nullptr;
        return false;
    }
    return true;
}

std::optional<size_t> OpusCodec::encode(const float* pcm, int frame_size, std::span<uint8_t> out) {
    if (!encoder_) return std::nullopt;
    const opus_int32 n = opus_encode_float(encoder_, pcm, frame_size, out.data(),
                                           static_cast<opus_int32>(out.size()));
    if (n <= 0) return std::nullopt;
    return static_cast<size_t>(n);
}

namespace {

std::optional<int> decoded_samples(int result) {
    if (result < 0) return std::nullopt;
    return result;
}

}

std::optional<int> OpusCodec::decode(std::span<const uint8_t> packet, float* pcm, int max_frame_size) {
    if (!decoder_) return std::nullopt;
    return decoded_samples(opus_decode_float(decoder_, packet.data(), static_cast<opus_int32>(packet.size()),
                                             pcm, max_frame_size, 0));
}

std::optional<int> OpusCodec::conceal_lost_frame(float* pcm, int frame_size) {
    if (!decoder_) return std::nullopt;
    return decoded_samples(opus_decode_float(decoder_, nullptr, 0, pcm, frame_size, 0));
}

std::optional<int> OpusCodec::recover_from_successor_fec(std::span<const uint8_t> successor,
                                                         float* pcm, int frame_size) {
    if (!decoder_) return std::nullopt;
    return decoded_samples(opus_decode_float(decoder_, successor.data(),
                                             static_cast<opus_int32>(successor.size()),
                                             pcm, frame_size, 1));
}

}
