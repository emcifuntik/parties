#include <client/echo_canceller.h>
#include <parties/audio_common.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

float next_noise(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return (static_cast<float>((state >> 8u) & 0xffffu) / 32767.5f) - 1.0f;
}

float delayed(const std::vector<float>& signal, int64_t index) {
    return index >= 0 ? signal[static_cast<size_t>(index)] : 0.0f;
}

} // namespace

int main() {
    using parties::audio::FRAME_SIZE;
    using parties::client::EchoCanceller;

    EchoCanceller canceller;
    if (!canceller.init()) {
        std::fprintf(stderr, "failed to initialize echo canceller\n");
        return 1;
    }
    canceller.set_enabled(true);

    constexpr int total_frames = 1100;
    constexpr int echo_measure_start_frame = 650;
    constexpr int near_end_start_frame = 900;
    constexpr int near_end_measure_start_frame = 950;
    constexpr int render_delay =
        static_cast<int>(EchoCanceller::REFERENCE_DELAY_FRAMES) * FRAME_SIZE;
    constexpr int left_path_delay = 73;
    constexpr int right_path_delay = 151;
    constexpr size_t total_samples = static_cast<size_t>(total_frames) * FRAME_SIZE;

    std::vector<float> left(total_samples);
    std::vector<float> right(total_samples);
    std::vector<float> near_end(total_samples);
    uint32_t left_rng = 0x12345678u;
    uint32_t right_rng = 0x9abcdef0u;
    float left_filtered = 0.0f;
    float right_filtered = 0.0f;
    for (size_t i = 0; i < total_samples; ++i) {
        // Independent, mildly band-limited speaker signals exercise both AEC
        // reference channels without presenting an unrealistically white input.
        left_filtered = 0.72f * left_filtered + 0.28f * next_noise(left_rng);
        right_filtered = 0.64f * right_filtered + 0.36f * next_noise(right_rng);
        left[i] = left_filtered * 0.55f;
        right[i] = right_filtered * 0.45f;
        const float time = static_cast<float>(i) / 48000.0f;
        const float envelope = 0.7f + 0.3f * std::sin(6.2831853f * 3.0f * time);
        near_end[i] = envelope *
            (0.12f * std::sin(6.2831853f * 190.0f * time) +
             0.06f * std::sin(6.2831853f * 380.0f * time) +
             0.03f * std::sin(6.2831853f * 760.0f * time));
    }

    double input_energy = 0.0;
    double output_energy = 0.0;
    size_t measured_samples = 0;
    double near_end_energy = 0.0;
    double near_end_projection = 0.0;

    std::vector<float> playback(static_cast<size_t>(FRAME_SIZE) * 2u);
    std::vector<float> microphone(FRAME_SIZE);
    for (int frame = 0; frame < total_frames; ++frame) {
        const int64_t base = static_cast<int64_t>(frame) * FRAME_SIZE;
        for (int i = 0; i < FRAME_SIZE; ++i) {
            const size_t source = static_cast<size_t>(base + i);
            playback[static_cast<size_t>(i) * 2u] = left[source];
            playback[static_cast<size_t>(i) * 2u + 1u] = right[source];

            const int64_t capture_sample = base + i;
            microphone[i] =
                0.48f * delayed(left, capture_sample - render_delay - left_path_delay) +
                0.34f * delayed(right, capture_sample - render_delay - right_path_delay);
            if (frame >= near_end_start_frame)
                microphone[i] += near_end[source];
        }

        // Exercise arbitrary callback boundaries: the production playback
        // callback is not guaranteed to hand us exactly one AEC frame.
        constexpr size_t split = 173;
        canceller.push_playback(playback.data(), split);
        canceller.push_playback(playback.data() + split * 2u,
                                static_cast<size_t>(FRAME_SIZE) - split);

        if (frame >= echo_measure_start_frame && frame < near_end_start_frame) {
            for (float sample : microphone)
                input_energy += static_cast<double>(sample) * sample;
        }

        canceller.process_capture(microphone.data(), FRAME_SIZE);

        if (frame >= echo_measure_start_frame && frame < near_end_start_frame) {
            for (float sample : microphone) {
                output_energy += static_cast<double>(sample) * sample;
                ++measured_samples;
            }
        }
        if (frame >= near_end_measure_start_frame) {
            for (int i = 0; i < FRAME_SIZE; ++i) {
                const float expected = near_end[static_cast<size_t>(base + i)];
                near_end_energy += static_cast<double>(expected) * expected;
                near_end_projection += static_cast<double>(microphone[i]) * expected;
            }
        }
    }

    const double input_rms = std::sqrt(input_energy / measured_samples);
    const double output_rms = std::sqrt(output_energy / measured_samples);
    const double attenuation_db = 20.0 * std::log10(output_rms / input_rms);
    std::printf("Stereo AEC: input %.6f, output %.6f, attenuation %.2f dB\n",
                input_rms, output_rms, attenuation_db);

    // This leaves broad headroom for different SpeexDSP build modes while still
    // catching missing stereo reference, wrong queue alignment, or a dead AEC.
    if (!(output_rms < input_rms * 0.35)) {
        std::fprintf(stderr, "echo attenuation is insufficient\n");
        return 1;
    }

    const double near_end_gain = near_end_projection / near_end_energy;
    std::printf("Stereo AEC double-talk near-end gain: %.3f\n", near_end_gain);
    if (!(near_end_gain > 0.6 && near_end_gain < 1.4)) {
        std::fprintf(stderr, "double-talk suppressed or amplified the near end\n");
        return 1;
    }

    canceller.set_enabled(false);
    std::fill(microphone.begin(), microphone.end(), 0.125f);
    if (canceller.process_capture(microphone.data(), FRAME_SIZE) ||
        !std::all_of(microphone.begin(), microphone.end(),
                     [](float sample) { return sample == 0.125f; })) {
        std::fprintf(stderr, "disabled AEC modified capture audio\n");
        return 1;
    }

    return 0;
}
