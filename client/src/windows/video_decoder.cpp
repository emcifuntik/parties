#include <client/video_decoder.h>
#include <encdec/factory.h>

#include <chrono>
#include <cstdio>
#include <parties/log.h>
#include <parties/profiler.h>

namespace parties::client {

VideoDecoder::~VideoDecoder() { shutdown(); }

bool VideoDecoder::init(VideoCodecId codec, uint32_t width, uint32_t height,
                        ID3D12Device* render_device) {
	ZoneScopedN("VideoDecoder::init");
    shutdown();
    codec_ = codec;
    width_ = width;
    height_ = height;

    if (!hardware_disabled_) {
        // Full chain: NVDEC → AMF → dav1d/MFT
        decoder_ = encdec::create_decoder(codec, width, height, render_device);
    } else {
        // Software only (after GPU context loss)
        decoder_ = encdec::create_software_decoder(codec, width, height);
    }

    if (!decoder_) return false;
    initialized_ = true;
    return true;
}

void VideoDecoder::shutdown() {
    decoder_.reset();
    initialized_ = false;
}

bool VideoDecoder::decode(const uint8_t* data, size_t len, int64_t timestamp) {
	ZoneScopedN("VideoDecoder::decode");
    if (!initialized_) return false;
    decoder_->on_decoded = on_decoded;
    const auto start = std::chrono::steady_clock::now();
    const bool decoded = decoder_->decode(data, len, timestamp);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    if (elapsed >= 20'000) {
        LOG_WARN("VIDEO_DECODE_SLOW backend={} total_ms={:.2f} bytes={} ts={} result={}",
                 backend_name(), static_cast<double>(elapsed) / 1000.0, len,
                 timestamp, decoded);
    }
    return decoded;
}

void VideoDecoder::flush() {
	ZoneScopedN("VideoDecoder::flush");
    if (!initialized_) return;
    decoder_->on_decoded = on_decoded;
    decoder_->flush();
}

bool VideoDecoder::context_lost() const {
    if (decoder_) return decoder_->context_lost();
    return false;
}

const char* VideoDecoder::backend_name() const {
    if (decoder_) return encdec::backend_name(decoder_->info().backend);
    return "none";
}

} // namespace parties::client
