#pragma once

#include <encdec/codec.h>

#include <cstdint>
#include <functional>

struct ID3D11Texture2D;

namespace parties::encdec {

inline constexpr int64_t TICKS_100NS_PER_MS = 10'000;
inline constexpr int64_t TICKS_100NS_PER_SECOND = 10'000'000;

constexpr int64_t ticks_100ns_to_ms(int64_t ticks_100ns) {
    return ticks_100ns / TICKS_100NS_PER_MS;
}

struct EncoderInfo {
    Backend backend;
    VideoCodecId codec;
    uint32_t width;
    uint32_t height;
};

class Encoder {
public:
    virtual ~Encoder() = default;

    virtual bool encode(ID3D11Texture2D* bgra_texture, int64_t timestamp_100ns) = 0;

    // Zero-copy path: register caller-owned textures, encode directly.
    // Not all backends support this — check supports_registered_input() first.
    virtual bool supports_registered_input() const { return false; }
    virtual int  register_input(ID3D11Texture2D* texture) { return -1; }
    virtual void unregister_inputs() {}
    virtual bool encode_registered(int slot, int64_t timestamp_100ns) { return false; }

    virtual void force_keyframe() = 0;

    virtual void set_average_bitrate_bps(uint32_t average_bps) = 0;

    virtual EncoderInfo info() const = 0;

    std::function<void(const uint8_t* data, size_t len, bool keyframe)> on_encoded;

    VideoCodecId codec() const { return info().codec; }
    uint32_t width() const { return info().width; }
    uint32_t height() const { return info().height; }
};

}
