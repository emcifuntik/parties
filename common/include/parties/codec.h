#pragma once

#include <cstdint>

struct OpusEncoder;
struct OpusDecoder;

namespace parties {

// Encoder tuning profile. Voice = mic speech (OPUS_APPLICATION_VOIP, speech
// signal). Music = the secondary/auxiliary stream (karaoke backing track,
// sound effects): OPUS_APPLICATION_AUDIO + music signal so transients and
// full-band content aren't smeared the way the speech-optimized path would.
enum class OpusMode { Voice, Music };

class OpusCodec {
public:
    OpusCodec();
    ~OpusCodec();

    // inband_fec: enable Opus in-band FEC (LBRR) and tell the encoder the
    // expected packet-loss percentage so it sizes redundancy accordingly. Use
    // for real-time voice over a lossy/unreliable transport.
    // mode: Voice (default) or Music (auxiliary stream); see OpusMode.
    bool init_encoder(int sample_rate, int channels, int bitrate,
                      bool inband_fec = false, int expected_loss_pct = 0,
                      OpusMode mode = OpusMode::Voice);
    bool init_decoder(int sample_rate, int channels);

    // Encode PCM -> Opus. Returns bytes written, or negative on error.
    int encode(const float* pcm_in, int frame_size,
               uint8_t* opus_out, int max_opus_bytes);

    // Decode Opus -> PCM. Returns samples decoded.
    //  - Normal: opus_in = the packet for this frame, decode_fec = 0.
    //  - PLC (packet missing, no successor): opus_in = nullptr, decode_fec = 0.
    //  - FEC recovery: opus_in = the NEXT packet (frame N+1), decode_fec = 1 → the
    //    decoder reconstructs the lost frame N from that packet's embedded FEC.
    //    max_frame_size must equal the lost frame's sample count.
    int decode(const uint8_t* opus_in, int opus_len,
               float* pcm_out, int max_frame_size, int decode_fec = 0);

    void set_bitrate(int bps);

private:
    OpusEncoder* encoder_ = nullptr;
    OpusDecoder* decoder_ = nullptr;
};

} // namespace parties
