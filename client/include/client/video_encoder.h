#pragma once

#include <encdec/encoder.h>

#include <cstdint>
#include <functional>
#include <memory>

struct ID3D11Device;
struct ID3D11Texture2D;

namespace parties::client {

class VideoEncoder {
public:
    VideoEncoder() = default;
    ~VideoEncoder();

    // Initialize with a D3D11 device (shared with screen capture).
    // Tries NVENC → AMF → MFT. Probes codecs: AV1 > H.265 > H.264.
    // input_width/height = capture texture size, width/height = encode output size.
    bool init(ID3D11Device* device, uint32_t width, uint32_t height,
              uint32_t input_width = 0, uint32_t input_height = 0,
              uint32_t fps = 30, uint32_t bitrate = VIDEO_DEFAULT_BITRATE,
              VideoCodecId preferred_codec = VideoCodecId::AV1);
    void shutdown();

    // Encode a BGRA texture. Calls on_encoded when output is ready.
    bool encode_frame(ID3D11Texture2D* bgra_texture, int64_t timestamp_100ns);

    // Zero-copy path: register caller-owned textures, encode directly.
    // Only works when the backend supports it (NVENC/AMF).
    bool supports_registered_input() const;
    int register_input(ID3D11Texture2D* texture);
    void unregister_inputs();
    bool encode_registered(int slot, int64_t timestamp_100ns);

    // Force next frame to be a keyframe
    void force_keyframe();

    // Dynamically change the average VBR target.
    void set_bitrate(uint32_t bitrate);

    // Which codec was selected?
    VideoCodecId codec() const { return encoder_ ? encoder_->codec() : VideoCodecId::H264; }
    uint32_t width() const { return encoder_ ? encoder_->width() : 0; }
    uint32_t height() const { return encoder_ ? encoder_->height() : 0; }

    // Which backend is active?
    const char* backend_name() const;

    // Callback with encoded bitstream data
    std::function<void(const uint8_t* data, size_t len, bool keyframe)> on_encoded;

private:
    std::unique_ptr<encdec::Encoder> encoder_;
    bool initialized_ = false;
};

} // namespace parties::client
