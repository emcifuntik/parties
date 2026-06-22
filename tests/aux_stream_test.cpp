// Offline, deterministic test for the secondary/auxiliary audio stream (VOICE2).
//
// The 2nd stream reuses the VoiceMixer jitter buffer/decoder but is constructed
// with apply_makeup=false and carries music-profile Opus. This test asserts the
// two properties the feature promises:
//   1. The aux mix does NOT get the +VOICE_OUTPUT_GAIN_DB voice makeup gain that
//      the primary mix gets (same packets → primary is ~4x the energy of aux).
//   2. The aux mix's own master volume scales it: 0 → silence, 2 → louder.
//
// Hand-rolled asserts (no gtest), mirroring voice_loss_test.cpp.

#include <client/voice_mixer.h>
#include <parties/codec.h>
#include <parties/audio_common.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace parties;
using namespace parties::client;

#define TEST_ASSERT(cond, msg) do {                                   \
    if (!(cond)) { std::printf("FAIL: %s (line %d)\n", (msg), __LINE__); return 1; } \
} while (0)

namespace {

constexpr int kFrame = audio::OPUS_FRAME_SIZE;   // 960 samples / 20 ms
constexpr UserId kUser = 7;
constexpr double kPi = 3.14159265358979323846;

struct Packet { uint16_t seq; std::vector<uint8_t> bytes; };

// A low-amplitude tone (0.2) so the primary mix's makeup gain (+6 dB ≈ 2x) stays
// well under full scale — no clipping to distort the energy ratio.
std::vector<Packet> encode_music(int n_frames) {
    OpusCodec enc;
    enc.init_encoder(audio::SAMPLE_RATE, audio::CHANNELS, audio::SECONDARY_OPUS_BITRATE,
                     /*inband_fec=*/true, audio::OPUS_EXPECTED_LOSS_PCT, OpusMode::Music);
    std::vector<Packet> pkts;
    pkts.reserve(n_frames);
    uint8_t buf[audio::MAX_OPUS_PACKET];
    double p1 = 0.0, p2 = 0.0;
    const double w1 = 2.0 * kPi * 220.0 / audio::SAMPLE_RATE;
    const double w2 = 2.0 * kPi * 660.0 / audio::SAMPLE_RATE;
    for (int f = 0; f < n_frames; ++f) {
        float frame[kFrame];
        for (int i = 0; i < kFrame; ++i) {
            frame[i] = static_cast<float>(0.2 * (std::sin(p1) + 0.4 * std::sin(p2)));
            p1 += w1; p2 += w2;
        }
        int n = enc.encode(frame, kFrame, buf, sizeof(buf));
        if (n <= 0) continue;
        pkts.push_back({ static_cast<uint16_t>(f), std::vector<uint8_t>(buf, buf + n) });
    }
    return pkts;
}

// Drive a mixer tick-by-tick (one packet pushed, one frame pulled), summing the
// output energy. master < 0 leaves the mixer's master volume at its default.
double run_energy(VoiceMixer& mixer, const std::vector<Packet>& pkts, float master) {
    if (master >= 0.0f) mixer.set_master_volume(master);
    std::vector<float> out(kFrame);
    const int ticks = static_cast<int>(pkts.size()) + 20;
    double energy = 0.0;
    for (int t = 0; t < ticks; ++t) {
        if (t < static_cast<int>(pkts.size())) {
            const auto& p = pkts[t];
            mixer.push_packet(kUser, p.seq, p.bytes.data(), p.bytes.size());
        }
        std::fill(out.begin(), out.end(), 0.0f);
        mixer.mix_output(out.data(), kFrame);
        for (float s : out) energy += static_cast<double>(s) * s;
    }
    return energy;
}

} // namespace

int main() {
    std::printf("[aux_stream_test] start\n");

    auto pkts = encode_music(200);   // 4 s
    TEST_ASSERT(pkts.size() >= 190, "encoded most music frames");

    // 1. Same packets through a primary (makeup on) vs aux (makeup off) mixer at
    //    unity. Primary gets +6 dB ≈ 2x amplitude ≈ 4x energy; aux does not.
    double aux_energy, primary_energy;
    {
        VoiceMixer aux(/*apply_makeup=*/false);
        aux_energy = run_energy(aux, pkts, -1.0f);
    }
    {
        VoiceMixer primary(/*apply_makeup=*/true);
        primary_energy = run_energy(primary, pkts, -1.0f);
    }
    std::printf("[makeup] aux=%.3f primary=%.3f ratio=%.2f\n",
                aux_energy, primary_energy,
                aux_energy > 0 ? primary_energy / aux_energy : 0.0);
    TEST_ASSERT(aux_energy > 0.0, "aux mix produced audio");
    TEST_ASSERT(primary_energy > aux_energy * 3.0,
                "primary makeup gain makes it markedly louder than the aux mix");
    TEST_ASSERT(primary_energy < aux_energy * 5.0,
                "makeup ratio is ~4x (no clipping distorting it)");

    // 2. Aux master volume scales the mix: 0 → silence, 2 → louder than unity.
    {
        VoiceMixer muted(/*apply_makeup=*/false);
        double e0 = run_energy(muted, pkts, 0.0f);
        std::printf("[master=0] energy=%.6f\n", e0);
        TEST_ASSERT(e0 < aux_energy * 1e-3, "master volume 0 silences the aux mix");

        VoiceMixer loud(/*apply_makeup=*/false);
        double e2 = run_energy(loud, pkts, 2.0f);
        std::printf("[master=2] energy=%.3f (unity=%.3f)\n", e2, aux_energy);
        TEST_ASSERT(e2 > aux_energy * 1.5, "master volume 2 is louder than unity");
    }

    std::printf("=== ALL AUX STREAM TESTS PASSED ===\n");
    return 0;
}
