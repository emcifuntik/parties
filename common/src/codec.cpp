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

bool OpusCodec::init_encoder(int sample_rate, int channels, int bitrate,
                             bool inband_fec, int expected_loss_pct, OpusMode mode) {
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
    opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate));

    const int signal = (mode == OpusMode::Music) ? OPUS_SIGNAL_MUSIC : OPUS_SIGNAL_VOICE;

    if (inband_fec) {
        // Packet-loss resilience over unreliable datagrams. In-band FEC (LBRR)
        // embeds a low-bitrate copy of the previous frame in each packet; it is
        // ONLY emitted when packet-loss-perc > 0 and the encoder is in
        // SILK/hybrid (where LBRR lives). App-side VAD already gates voice
        // transmission, so keep Opus DTX off (it would add in-band gaps the
        // receiver would mistake for loss). For the music profile LBRR may not
        // engage at high bitrate; the decoder still PLCs lost frames.
        opus_encoder_ctl(encoder_, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(encoder_, OPUS_SET_PACKET_LOSS_PERC(expected_loss_pct));
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

int OpusCodec::encode(const float* pcm_in, int frame_size,
                      uint8_t* opus_out, int max_opus_bytes) {
    if (!encoder_) return -1;
    return opus_encode_float(encoder_, pcm_in, frame_size, opus_out, max_opus_bytes);
}

int OpusCodec::decode(const uint8_t* opus_in, int opus_len,
                      float* pcm_out, int max_frame_size, int decode_fec) {
    if (!decoder_) return -1;
    return opus_decode_float(decoder_, opus_in, opus_len, pcm_out, max_frame_size, decode_fec);
}

void OpusCodec::set_bitrate(int bps) {
    if (encoder_) opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bps));
}

} // namespace parties
