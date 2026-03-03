// iOS SoundPlayer — programmatically synthesized UI tones.
// No .wav files needed; all sounds are generated in the constructor.
// play()       — lock-free, safe to call from any thread.
// mix_output() — called from the miniaudio callback thread.

#include <client/sound_player.h>

#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace parties::client {

namespace {

// Sine sweep with linear attack/release envelope.
// freq_start → freq_end over duration_s seconds.
std::vector<float> gen_sweep(float duration_s, float freq_start, float freq_end,
                             float amplitude, int sr)
{
    int n = static_cast<int>(duration_s * sr);
    std::vector<float> buf(n);
    float attack  = 0.005f * sr;   // 5 ms attack
    float release = 0.010f * sr;   // 10 ms release
    for (int i = 0; i < n; ++i) {
        float t     = static_cast<float>(i) / sr;
        float phase = 2.0f * static_cast<float>(M_PI) *
                      (freq_start * t +
                       (freq_end - freq_start) * t * t / (2.0f * duration_s));
        float env;
        if (i < static_cast<int>(attack))
            env = static_cast<float>(i) / attack;
        else if (i >= n - static_cast<int>(release))
            env = static_cast<float>(n - i) / release;
        else
            env = 1.0f;
        buf[i] = amplitude * env * std::sin(phase);
    }
    return buf;
}

// Single tone with exponential decay (chime character).
std::vector<float> gen_chime(float duration_s, float freq,
                             float amplitude, int sr)
{
    int n = static_cast<int>(duration_s * sr);
    std::vector<float> buf(n);
    float attack     = 0.005f * sr;
    float decay_rate = 5.0f / duration_s;
    for (int i = 0; i < n; ++i) {
        float t     = static_cast<float>(i) / sr;
        float phase = 2.0f * static_cast<float>(M_PI) * freq * t;
        float env   = std::exp(-decay_rate * t);
        if (i < static_cast<int>(attack))
            env *= static_cast<float>(i) / attack;
        buf[i] = amplitude * env * std::sin(phase);
    }
    return buf;
}

// Two sequential tones (e.g. deafen / undeafen double-blip).
std::vector<float> gen_two_tones(float duration_s, float freq1, float freq2,
                                 float amplitude, int sr)
{
    int n    = static_cast<int>(duration_s * sr);
    int half = n / 2;
    std::vector<float> buf(n);
    float attack  = 0.005f * sr;
    float release = 0.010f * sr;
    for (int i = 0; i < n; ++i) {
        float freq   = (i < half) ? freq1 : freq2;
        int   li     = (i < half) ? i : (i - half);
        int   ln     = half;
        float t      = static_cast<float>(li) / sr;
        float phase  = 2.0f * static_cast<float>(M_PI) * freq * t;
        float env;
        if (li < static_cast<int>(attack))
            env = static_cast<float>(li) / attack;
        else if (li >= ln - static_cast<int>(release))
            env = static_cast<float>(ln - li) / release;
        else
            env = 1.0f;
        buf[i] = amplitude * env * std::sin(phase);
    }
    return buf;
}

} // anonymous namespace

// ── Constructor ───────────────────────────────────────────────────────────────

SoundPlayer::SoundPlayer()
{
    constexpr int sr = kSampleRate;    // 48 000 Hz

    // Mute    — descending sweep, feels like "silenced"
    sounds_[static_cast<int>(Effect::Mute)].samples =
        gen_sweep(0.15f, 700.0f, 350.0f, 0.40f, sr);

    // Unmute  — ascending sweep
    sounds_[static_cast<int>(Effect::Unmute)].samples =
        gen_sweep(0.15f, 350.0f, 700.0f, 0.40f, sr);

    // Deafen  — two descending blips
    sounds_[static_cast<int>(Effect::Deafen)].samples =
        gen_two_tones(0.28f, 640.0f, 420.0f, 0.40f, sr);

    // Undeafen — two ascending blips
    sounds_[static_cast<int>(Effect::Undeafen)].samples =
        gen_two_tones(0.28f, 420.0f, 640.0f, 0.40f, sr);

    // JoinChannel  — warm ascending chime
    sounds_[static_cast<int>(Effect::JoinChannel)].samples =
        gen_chime(0.35f, 880.0f, 0.50f, sr);

    // LeaveChannel — falling chime
    sounds_[static_cast<int>(Effect::LeaveChannel)].samples =
        gen_chime(0.35f, 660.0f, 0.50f, sr);

    // UserJoined   — brief high ping
    sounds_[static_cast<int>(Effect::UserJoined)].samples =
        gen_chime(0.12f, 1100.0f, 0.35f, sr);

    // UserLeft     — brief low ping
    sounds_[static_cast<int>(Effect::UserLeft)].samples =
        gen_chime(0.12f, 740.0f, 0.35f, sr);
}

// ── play() ────────────────────────────────────────────────────────────────────
// Called from any thread.  Uses compare_exchange to atomically claim a slot
// so that concurrent callers never write to the same entry.

void SoundPlayer::play(Effect effect)
{
    int idx = static_cast<int>(effect);
    if (idx < 0 || idx >= static_cast<int>(Effect::Count_))
        return;

    for (auto& slot : playing_) {
        int expected = -1;
        // -2 is a transient "being armed" sentinel visible only within play().
        if (slot.effect.compare_exchange_strong(
                expected, -2,
                std::memory_order_acquire,
                std::memory_order_relaxed)) {
            slot.position.store(0, std::memory_order_relaxed);
            // Release makes position visible to the audio thread.
            slot.effect.store(idx, std::memory_order_release);
            return;
        }
    }
    // All slots busy — drop the sound rather than block.
}

// ── mix_output() ──────────────────────────────────────────────────────────────
// Called from the miniaudio callback thread.  Adds active sounds into the
// (already-zeroed) mono float output buffer.

void SoundPlayer::mix_output(float* output, int frame_count)
{
    for (auto& slot : playing_) {
        int idx = slot.effect.load(std::memory_order_acquire);
        if (idx < 0)
            continue;   // -1 = inactive, -2 = being armed (skip safely)

        const auto& snd = sounds_[static_cast<size_t>(idx)];
        size_t pos      = slot.position.load(std::memory_order_relaxed);
        size_t len      = snd.samples.size();
        if (pos >= len) {
            slot.effect.store(-1, std::memory_order_release);
            continue;
        }

        int to_mix = std::min(frame_count, static_cast<int>(len - pos));
        for (int i = 0; i < to_mix; ++i)
            output[i] += snd.samples[pos + i];

        pos += static_cast<size_t>(to_mix);
        slot.position.store(pos, std::memory_order_relaxed);
        if (pos >= len)
            slot.effect.store(-1, std::memory_order_release);
    }
}

} // namespace parties::client
