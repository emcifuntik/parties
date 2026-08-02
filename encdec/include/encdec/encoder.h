#pragma once

#include <encdec/codec.h>

#include <cstdint>
#include <functional>

struct ID3D11Texture2D;

namespace parties::encdec {

struct EncoderInfo {
    Backend backend;
    VideoCodecId codec;
    uint32_t width;
    uint32_t height;
};

class Encoder {
public:
    virtual ~Encoder() = default;

    // Encode a BGRA texture. Fires on_encoded when bitstream is ready.
    virtual bool encode(ID3D11Texture2D* bgra_texture, int64_t timestamp_100ns) = 0;

    // Zero-copy path: register caller-owned textures, encode directly.
    // Not all backends support this — check supports_registered_input() first.
    virtual bool supports_registered_input() const { return false; }
    virtual int  register_input(ID3D11Texture2D* /*texture*/) { return -1; }
    virtual void unregister_inputs() {}
    virtual bool encode_registered(int /*slot*/, int64_t /*timestamp_100ns*/) { return false; }

    // Force next frame to be a keyframe
    virtual void force_keyframe() = 0;

    // Dynamically change the average VBR target (bits per second).
    virtual void set_bitrate(uint32_t bitrate) = 0;

    // Metadata about the active encoder
    virtual EncoderInfo info() const = 0;

    // Callback with encoded bitstream data
    std::function<void(const uint8_t* data, size_t len, bool keyframe)> on_encoded;

    // Convenience accessors
    VideoCodecId codec() const { return info().codec; }
    uint32_t width() const { return info().width; }
    uint32_t height() const { return info().height; }
};

} // namespace parties::encdec
