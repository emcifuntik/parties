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

    bool init(ID3D11Device* device, uint32_t width, uint32_t height,
              uint32_t fps = 30, uint32_t average_bps = VIDEO_DEFAULT_BITRATE,
              VideoCodecId preferred_codec = VideoCodecId::AV1);
    void shutdown();

    bool encode_frame(ID3D11Texture2D* bgra_texture, int64_t timestamp_100ns);

    bool supports_registered_input() const;
    int register_input(ID3D11Texture2D* texture);
    void unregister_inputs();
    bool encode_registered(int slot, int64_t timestamp_100ns);

    void force_keyframe();

    void set_average_bitrate_bps(uint32_t average_bps);

    VideoCodecId codec() const { return encoder_ ? encoder_->codec() : VideoCodecId::H264; }
    uint32_t width() const { return encoder_ ? encoder_->width() : 0; }
    uint32_t height() const { return encoder_ ? encoder_->height() : 0; }

    const char* backend_name() const;

    std::function<void(const uint8_t* data, size_t len, bool keyframe)> on_encoded;

private:
    std::unique_ptr<encdec::Encoder> encoder_;
    bool initialized_ = false;
};

}
