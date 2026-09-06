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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
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
std::vector<Packet> encode_music(int n_frames, float amplitude = 0.2f) {
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
            frame[i] = static_cast<float>(amplitude * (std::sin(p1) + 0.4 * std::sin(p2)));
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

std::vector<Packet> with_sequence_base(const std::vector<Packet>& packets,
                                       uint16_t sequence_base) {
    std::vector<Packet> result = packets;
    for (size_t index = 0; index < result.size(); ++index)
        result[index].seq = static_cast<uint16_t>(sequence_base + index);
    return result;
}

// A full receive queue must return to decoding real packets, even while the
// producer keeps sending. Otherwise every eviction becomes another lost slot.
int test_overflow_recovery() {
    const auto packets = encode_music(160);
    TEST_ASSERT(packets.size() == 160, "overflow test encoded every frame");
    bool recovered = true;
    for (uint16_t sequence_base : {uint16_t{0}, uint16_t{65512}}) {
        for (int burst : {7, 10}) {
            VoiceMixer mixer(/*apply_makeup=*/false);
            std::vector<float> output(kFrame);
            size_t next = 0;
            auto push_next = [&] {
                const auto& packet = packets[next++];
                mixer.push_packet(kUser, static_cast<uint16_t>(sequence_base + packet.seq),
                                  packet.bytes.data(), packet.bytes.size());
            };
            for (int tick = 0; tick < 20; ++tick) {
                push_next();
                mixer.mix_output(output.data(), kFrame);
            }
            // Simulate accumulated sender/receiver clock drift or a playback stall.
            for (int i = 0; i < burst; ++i) push_next();
            const auto before = mixer.decode_stats();
            for (int tick = 0; tick < 100; ++tick) {
                push_next();
                mixer.mix_output(output.data(), kFrame);
            }
            const auto after = mixer.decode_stats();
            const auto normal = after.normal - before.normal;
            std::printf("[overflow base=%u burst=%d] normal=%llu fec=%llu plc=%llu\n",
                        sequence_base, burst, static_cast<unsigned long long>(normal),
                        static_cast<unsigned long long>(after.fec - before.fec),
                        static_cast<unsigned long long>(after.plc - before.plc));
            recovered &= normal >= 99;
        }
    }
    TEST_ASSERT(recovered, "a full queue recovers without reconnecting or stopping the sender");
    return 0;
}

// Replay encoded music with a slightly faster/slower producer clock. Cached
// payloads let --soak cover 46 minutes and multiple sequence wraps per clock
// direction without waiting for wall time or involving audio hardware.
int test_clock_drift(bool soak) {
    const auto packets = encode_music(32);
    TEST_ASSERT(packets.size() == 32, "clock drift test encoded every frame");
    const int ticks = soak ? 140000 : 12000;
    const int drift_interval = soak ? 5000 : 500;
    for (bool faster : {true, false}) {
        VoiceMixer mixer(/*apply_makeup=*/false);
        std::vector<float> output(kFrame);
        uint32_t sent = 0;
        auto push_next = [&] {
            const auto& packet = packets[sent % packets.size()];
            mixer.push_packet(kUser, static_cast<uint16_t>(65500u + sent),
                              packet.bytes.data(), packet.bytes.size());
            ++sent;
        };
        uint64_t normal_before_tail = 0;
        for (int tick = 0; tick < ticks; ++tick) {
            const bool drift = tick > 0 && tick % drift_interval == 0;
            if (faster || !drift) push_next();
            if (faster && drift) push_next();
            mixer.mix_output(output.data(), kFrame);
            for (float sample : output)
                TEST_ASSERT(std::isfinite(sample), "continuous playback remains finite");
            if (tick == ticks - 1001) normal_before_tail = mixer.decode_stats().normal;
        }
        const auto stats = mixer.decode_stats();
        std::printf("[clock %s ticks=%d] normal=%llu fec=%llu plc=%llu resync=%llu tail-normal=%llu\n",
                    faster ? "faster" : "slower", ticks,
                    static_cast<unsigned long long>(stats.normal),
                    static_cast<unsigned long long>(stats.fec),
                    static_cast<unsigned long long>(stats.plc),
                    static_cast<unsigned long long>(stats.resync),
                    static_cast<unsigned long long>(stats.normal - normal_before_tail));
        TEST_ASSERT(stats.normal > static_cast<uint64_t>(ticks * 0.98),
                    "clock drift does not trap sustained playback in concealment");
        TEST_ASSERT(stats.normal - normal_before_tail > 980,
                    "the final 20 seconds still decode normally without a reconnect");
        if (faster) {
            TEST_ASSERT(stats.resync > 0, "faster producer exercises queue catch-up");
            TEST_ASSERT(stats.fec == 0 && stats.plc == 0,
                        "intentional queue evictions do not become synthetic packet loss");
        }
    }
    return 0;
}

// Device callbacks need not divide 960 samples. A second user joins halfway
// through the first user's PCM frame, so their read positions also differ.
std::vector<float> render_partitioned(const std::vector<Packet>& packets,
                                      const std::vector<int>& callback_sizes) {
    VoiceMixer mixer(/*apply_makeup=*/false);
    auto add_user = [&](UserId uid) {
        for (const auto& packet : packets)
            mixer.push_packet(uid, packet.seq, packet.bytes.data(), packet.bytes.size());
    };
    add_user(kUser);
    std::vector<float> output(6 * kFrame);
    size_t offset = 0;
    size_t callback = 0;
    while (offset < output.size()) {
        if (offset == 480) add_user(kUser + 1);
        size_t count = std::min(static_cast<size_t>(callback_sizes[callback++ % callback_sizes.size()]),
                                output.size() - offset);
        if (offset < 480) count = std::min(count, 480 - offset);
        mixer.mix_output(output.data() + offset, static_cast<int>(count));
        offset += count;
    }
    return output;
}

int test_partial_playback_frames() {
    const auto packets = encode_music(8);
    TEST_ASSERT(packets.size() == 8, "partial playback test encoded every frame");
    const auto reference = render_partitioned(packets, {1});
    bool matched = true;
    for (const std::vector<int>& callbacks : {
            std::vector<int>{480}, {128}, {512}, {1024}, {4096}, {127, 256, 480, 1024, 37}}) {
        const auto output = render_partitioned(packets, callbacks);
        float max_error = 0.0f;
        for (size_t i = 0; i < output.size(); ++i) {
            TEST_ASSERT(std::isfinite(output[i]), "partial playback produces finite samples");
            max_error = std::max(max_error, std::fabs(output[i] - reference[i]));
        }
        std::printf("[callback=%d variants=%zu] max PCM error=%.6f\n",
                    callbacks.front(), callbacks.size(), max_error);
        matched &= max_error < 1e-6f;
    }
    TEST_ASSERT(matched, "playback PCM is independent of callback size and user join timing");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const bool soak = argc == 2 && std::strcmp(argv[1], "--soak") == 0;
    std::printf("[aux_stream_test] start\n");

    const int overflow_result = test_overflow_recovery();
    const int partial_result = test_partial_playback_frames();
    TEST_ASSERT(overflow_result == 0 && partial_result == 0, "receiver regression tests");
    TEST_ASSERT(test_clock_drift(soak) == 0, "continuous receiver clock drift");

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

    // 3. Per-user music preferences are applied from on_stream_created, exactly
    //    like AppCore does when the first VOICE2 packet for a member arrives.
    //    This matters because the saved preference exists before the mixer has
    //    created that user's auxiliary stream.
    {
        VoiceMixer per_user_muted(/*apply_makeup=*/false);
        per_user_muted.on_stream_created = [&per_user_muted](UserId user_id) {
            per_user_muted.set_user_volume(user_id, 0.0f);
        };
        double muted_energy = run_energy(per_user_muted, pkts, -1.0f);
        std::printf("[per-user=0] energy=%.6f\n", muted_energy);
        TEST_ASSERT(muted_energy < aux_energy * 1e-3,
                    "saved per-user music volume is applied on stream creation");

        VoiceMixer per_user_loud(/*apply_makeup=*/false);
        per_user_loud.on_stream_created = [&per_user_loud](UserId user_id) {
            per_user_loud.set_user_volume(user_id, 2.0f);
        };
        double loud_energy = run_energy(per_user_loud, pkts, -1.0f);
        std::printf("[per-user=2] energy=%.3f (unity=%.3f)\n", loud_energy, aux_energy);
        TEST_ASSERT(loud_energy > aux_energy * 1.5,
                    "per-user music volume scales only that member's VOICE2 stream");
    }

    // 4. VOICE2 has a 16-bit sequence number, so a continuous stream wraps
    // every ~21.8 minutes. Exercise the exact 65535 -> 0 transition rather than
    // waiting that long in the test. No packet may be discarded as stale and
    // the decoder must not enter PLC/resync at the boundary.
    {
        auto wrap_packets = with_sequence_base(encode_music(32), 65524);
        VoiceMixer wrapped(/*apply_makeup=*/false);
        double wrap_energy = run_energy(wrapped, wrap_packets, -1.0f);
        const auto stats = wrapped.decode_stats();
        std::printf("[sequence-wrap] energy=%.3f normal=%llu plc=%llu resync=%llu\n",
                    wrap_energy,
                    static_cast<unsigned long long>(stats.normal),
                    static_cast<unsigned long long>(stats.plc),
                    static_cast<unsigned long long>(stats.resync));
        TEST_ASSERT(wrap_energy > 1.0, "audio survives the uint16 sequence wrap");
        TEST_ASSERT(stats.normal >= wrap_packets.size(),
                    "all packets around the sequence wrap decode normally");
        TEST_ASSERT(stats.resync == 0, "sequence wrap does not trigger jitter resync");
    }

    // 5. Activity is attributed to the decoded source before receive volumes.
    // Removing one participant must not clear another participant's indicator.
    {
        VoiceMixer aux(/*apply_makeup=*/false);
        aux.set_master_volume(0.0f);
        const auto music = encode_music(8);
        const auto silence = encode_music(8, 0.0f);
        std::vector<float> output(kFrame);
        for (const auto& packet : music) {
            aux.push_packet(kUser, packet.seq, packet.bytes.data(), packet.bytes.size());
            aux.push_packet(kUser + 1, packet.seq, packet.bytes.data(), packet.bytes.size());
            const auto& quiet = silence[packet.seq];
            aux.push_packet(kUser + 2, quiet.seq, quiet.bytes.data(), quiet.bytes.size());
            aux.set_user_volume(kUser, 0.0f);
            aux.mix_output(output.data(), kFrame);
        }
        const auto active = aux.get_active_users();
        TEST_ASSERT(active.size() == 2, "only audible sources are visible, even at zero receive volume");
        TEST_ASSERT(std::find(active.begin(), active.end(), kUser) != active.end(),
                    "music activity identifies the sender");
        aux.remove_user(kUser);
        const auto remaining = aux.get_active_users();
        TEST_ASSERT(remaining.size() == 1 && remaining[0] == kUser + 1,
                    "leaving clears only that sender's music activity");
        aux.clear();
        TEST_ASSERT(aux.get_active_users().empty(), "channel cleanup clears music activity");
    }

    // 6. Silence cannot renew activity, and the deadline needs no audio callback.
    {
        using namespace std::chrono_literals;
        AudioActivity activity;
        const auto start = AudioActivity::Clock::time_point{} + 1s;
        TEST_ASSERT(!activity.active(start), "a new source is inactive");
        activity.observe_level(0.1f, start);
        activity.observe_level(0.0f, start + 400ms);
        activity.observe_level(AudioActivity::minimum_rms, start + 450ms);
        TEST_ASSERT(activity.active(start + 499ms), "short pauses retain the music indicator");
        TEST_ASSERT(!activity.active(start + 500ms), "stopped or silent sources expire after 500 ms");
        activity.observe_level(0.1f, start + 600ms);
        TEST_ASSERT(activity.active(start + 700ms), "music restarts the indicator");
        activity.reset();
        TEST_ASSERT(!activity.active(start + 700ms), "reset immediately clears local activity");
        const float silence[32]{};
        activity.observe_pcm(silence, 32);
        TEST_ASSERT(!activity.active(), "silent local PCM does not indicate music");
        const float audible[] = {0.1f, -0.1f};
        activity.observe_pcm(audible, 2);
        TEST_ASSERT(activity.active(), "audible local PCM indicates music");
    }

    std::printf("=== ALL AUX STREAM TESTS PASSED ===\n");
    return 0;
}
