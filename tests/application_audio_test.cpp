#include <client/application_audio.h>

#include <array>
#include <cmath>
#include <cstdio>

int main() {
    constexpr std::array<float, 10> stereo{
        1.0f, 1.0f,
        1.0f, -1.0f,
        -0.5f, 0.5f,
        0.25f, 0.75f,
        -1.0f, -0.5f,
    };
    constexpr std::array<float, 5> expected{1.0f, 0.0f, 0.0f, 0.5f, -0.75f};
    std::array<float, expected.size()> mono{};

    parties::client::downmix_application_audio(
        stereo.data(), mono.data(), static_cast<int>(mono.size()));

    for (size_t index = 0; index < mono.size(); ++index) {
        if (std::fabs(mono[index] - expected[index]) > 0.00001f) {
            std::fprintf(stderr, "downmix mismatch at %zu: %.6f != %.6f\n",
                index, mono[index], expected[index]);
            return 1;
        }
    }

    // Invalid buffers and lengths are deliberately ignored; capture teardown
    // can race a final empty WASAPI packet without touching caller memory.
    parties::client::downmix_application_audio(nullptr, mono.data(), 5);
    parties::client::downmix_application_audio(stereo.data(), nullptr, 5);
    parties::client::downmix_application_audio(stereo.data(), mono.data(), 0);
    return 0;
}
