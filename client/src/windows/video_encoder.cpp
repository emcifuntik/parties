#include <client/video_encoder.h>
#include <encdec/factory.h>

#include <cstdio>
#include <parties/profiler.h>

namespace parties::client {

VideoEncoder::~VideoEncoder() {
    shutdown();
}

bool VideoEncoder::init(ID3D11Device* device, uint32_t width, uint32_t height,
                         uint32_t fps, uint32_t average_bps,
                         VideoCodecId preferred_codec) {
	ZoneScopedN("VideoEncoder::init");
    if (initialized_) return false;

    encoder_ = encdec::create_encoder(device, width, height,
                                       fps, average_bps, preferred_codec);
    if (!encoder_) return false;

    initialized_ = true;
    return true;
}

void VideoEncoder::shutdown() {
    if (!initialized_) return;
    encoder_.reset();
    initialized_ = false;
}

bool VideoEncoder::encode_frame(ID3D11Texture2D* bgra_texture, int64_t timestamp_100ns) {
	ZoneScopedN("VideoEncoder::encode_frame");
    if (!initialized_) return false;
    encoder_->on_encoded = on_encoded;
    return encoder_->encode(bgra_texture, timestamp_100ns);
}

bool VideoEncoder::supports_registered_input() const {
    return encoder_ && encoder_->supports_registered_input();
}

int VideoEncoder::register_input(ID3D11Texture2D* texture) {
    if (encoder_) return encoder_->register_input(texture);
    return -1;
}

void VideoEncoder::unregister_inputs() {
    if (encoder_) encoder_->unregister_inputs();
}

bool VideoEncoder::encode_registered(int slot, int64_t timestamp_100ns) {
    if (!initialized_ || !encoder_) return false;
    encoder_->on_encoded = on_encoded;
    return encoder_->encode_registered(slot, timestamp_100ns);
}

void VideoEncoder::force_keyframe() {
    if (encoder_) encoder_->force_keyframe();
}

void VideoEncoder::set_average_bitrate_bps(uint32_t average_bps) {
    if (encoder_) encoder_->set_average_bitrate_bps(average_bps);
}

const char* VideoEncoder::backend_name() const {
    if (encoder_) return encdec::backend_name(encoder_->info().backend);
    return "none";
}

}
