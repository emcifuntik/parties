#include <client/voice_mixer.h>
#include <parties/codec.h>
#include <parties/audio_common.h>

#include <opus/opus.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

using namespace parties;
using namespace parties::client;

#define TEST_ASSERT(cond, msg) do {                                   \
    if (!(cond)) { std::printf("FAIL: %s (line %d)\n", (msg), __LINE__); return 1; } \
} while (0)

namespace {

constexpr int kFrame = audio::OPUS_FRAME_SIZE;
constexpr UserId kUser = 1;
constexpr double kPi = 3.14159265358979323846;

std::vector<std::vector<float>> make_reference(int n_frames) {
    std::vector<std::vector<float>> frames(n_frames);
    double p1 = 0.0, p2 = 0.0;
    const double w1 = 2.0 * kPi * 200.0 / audio::SAMPLE_RATE;
    const double w2 = 2.0 * kPi * 400.0 / audio::SAMPLE_RATE;
    long n = 0;
    for (int f = 0; f < n_frames; ++f) {
        frames[f].resize(kFrame);
        for (int i = 0; i < kFrame; ++i, ++n) {
            double env = 0.5 + 0.5 * std::sin(2.0 * kPi * 2.5 * n / audio::SAMPLE_RATE);
            frames[f][i] = static_cast<float>(0.3 * env * (std::sin(p1) + 0.5 * std::sin(p2)));
            p1 += w1; p2 += w2;
        }
    }
    return frames;
}

struct Packet { uint16_t seq; std::vector<uint8_t> bytes; };

std::vector<Packet> encode_all(const std::vector<std::vector<float>>& frames,
                               int* out_lbrr_count) {
    OpusCodec enc;
    enc.init_encoder(audio::SAMPLE_RATE, audio::CHANNELS, audio::OPUS_BITRATE, kVoiceFec);
    std::vector<Packet> pkts;
    pkts.reserve(frames.size());
    int lbrr = 0;
    uint8_t buf[audio::MAX_OPUS_PACKET];
    for (size_t f = 0; f < frames.size(); ++f) {
        const auto n = enc.encode(frames[f].data(), kFrame, buf);
        if (!n) continue;
        Packet p{ static_cast<uint16_t>(f), std::vector<uint8_t>(buf, buf + *n) };
        if (opus_packet_has_lbrr(p.bytes.data(), static_cast<opus_int32>(*n)) == 1) ++lbrr;
        pkts.push_back(std::move(p));
    }
    if (out_lbrr_count) *out_lbrr_count = lbrr;
    return pkts;
}

struct RunResult { VoiceMixer::DecodeStats stats; int ticks; double out_energy; };

template <typename DropFn>
RunResult run(const std::vector<Packet>& pkts, DropFn dropped) {
    VoiceMixer mixer;
    std::vector<float> out(kFrame);
    const int total_ticks = static_cast<int>(pkts.size()) + 20;
    double energy = 0.0;
    for (int t = 0; t < total_ticks; ++t) {
        if (t < static_cast<int>(pkts.size()) && !dropped(pkts[t].seq)) {
            const auto& p = pkts[t];
            mixer.push_packet(kUser, p.seq, p.bytes.data(), p.bytes.size());
        }
        std::fill(out.begin(), out.end(), 0.0f);
        mixer.mix_output(out.data(), kFrame);
        for (float s : out) energy += static_cast<double>(s) * s;
    }
    return { mixer.decode_stats(), total_ticks, energy };
}

}

int main() {
    std::printf("[voice_loss_test] start\n");

    const int n = 250;
    auto frames = make_reference(n);

    int lbrr = 0;
    auto pkts = encode_all(frames, &lbrr);
    TEST_ASSERT(static_cast<int>(pkts.size()) >= n - 2, "encoded most frames");

    std::printf("[voice_loss_test] LBRR packets: %d / %zu\n", lbrr, pkts.size());
    TEST_ASSERT(lbrr >= static_cast<int>(pkts.size() * 7 / 10),
                "encoder emits in-band FEC (LBRR) on most packets");

    {
        auto r = run(pkts, [](uint16_t) { return false; });
        std::printf("[clean] normal=%llu fec=%llu plc=%llu resync=%llu\n",
                    (unsigned long long)r.stats.normal, (unsigned long long)r.stats.fec,
                    (unsigned long long)r.stats.plc, (unsigned long long)r.stats.resync);
        TEST_ASSERT(r.stats.fec == 0, "no FEC recovery on a lossless stream");
        TEST_ASSERT(r.stats.resync == 0, "no resync on a lossless stream");
        TEST_ASSERT(r.stats.normal >= (uint64_t)(n * 9 / 10), "clean run decodes ~all frames in order");
        TEST_ASSERT(r.out_energy > 0.0, "clean run produced audio");
    }

    {
        int dropped_total = 0;
        for (auto& p : pkts) if (p.seq % 10 == 5 && p.seq != 5) ++dropped_total;
        auto r = run(pkts, [](uint16_t s) { return s % 10 == 5 && s != 5; });
        std::printf("[10%%-isolated] drops=%d normal=%llu fec=%llu plc=%llu resync=%llu\n",
                    dropped_total, (unsigned long long)r.stats.normal,
                    (unsigned long long)r.stats.fec, (unsigned long long)r.stats.plc,
                    (unsigned long long)r.stats.resync);
        TEST_ASSERT(r.stats.fec >= (uint64_t)(dropped_total * 7 / 10),
                    "most isolated losses are recovered via FEC");
        TEST_ASSERT(r.stats.resync == 0, "isolated loss never triggers resync");
        TEST_ASSERT(r.out_energy > 0.0, "lossy run still produced audio");
    }

    {
        auto burst = [](uint16_t s) { uint16_t m = s % 40; return m == 10 || m == 11 || m == 12; };
        auto r = run(pkts, burst);
        std::printf("[burst3] normal=%llu fec=%llu plc=%llu resync=%llu\n",
                    (unsigned long long)r.stats.normal, (unsigned long long)r.stats.fec,
                    (unsigned long long)r.stats.plc, (unsigned long long)r.stats.resync);
        TEST_ASSERT(r.stats.fec > 0, "bursts still recover their trailing frame via FEC");
        TEST_ASSERT(r.stats.plc > 0, "bursts conceal earlier frames via PLC");
        TEST_ASSERT(r.stats.resync == 0, "a 3-burst is well under RESYNC_GAP");
        TEST_ASSERT(r.out_energy > 0.0, "burst run still produced audio");
    }

    {
        std::mt19937 rng(12345);
        std::uniform_real_distribution<double> u(0.0, 1.0);
        std::vector<bool> drop(pkts.size(), false);
        int dn = 0;
        for (size_t i = 5; i < pkts.size(); ++i) if (u(rng) < 0.10) { drop[i] = true; ++dn; }
        auto r = run(pkts, [&](uint16_t s) { return s < drop.size() && drop[s]; });
        std::printf("[10%%-random] drops=%d normal=%llu fec=%llu plc=%llu resync=%llu\n",
                    dn, (unsigned long long)r.stats.normal, (unsigned long long)r.stats.fec,
                    (unsigned long long)r.stats.plc, (unsigned long long)r.stats.resync);
        TEST_ASSERT(r.stats.fec > 0, "random loss exercises FEC recovery");
        TEST_ASSERT(r.stats.fec + r.stats.normal >= (uint64_t)(n * 8 / 10),
                    "the bulk of frames are real audio (decoded or FEC-recovered)");
        TEST_ASSERT(r.stats.resync <= 1, "random isolated/short loss rarely resyncs");
    }

    std::printf("=== ALL VOICE LOSS TESTS PASSED ===\n");
    return 0;
}
