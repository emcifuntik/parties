#include "dav1d_decoder.h"

#include <dav1d/dav1d.h>

#include <algorithm>
#include <cstring>
#include <thread>
#include <parties/log.h>
#include <parties/profiler.h>

namespace parties::encdec::dav1d {

Dav1dDecoder::~Dav1dDecoder() {
    if (ctx_) {
        dav1d_flush(ctx_);
        dav1d_close(&ctx_);
    }
}

bool Dav1dDecoder::init(VideoCodecId codec, uint32_t /*width*/, uint32_t /*height*/) {
    if (codec != VideoCodecId::AV1)
        return false;

    codec_ = codec;

    // Use about half of the machine's hardware threads for the decoder, bounded
    // to [4, 16]. hardware_concurrency() may report 0; treat that as 4.
    // max_frame_delay = 1 keeps the decoder from buffering frames for
    // throughput: every submitted frame is output as soon as it is decoded.
    const unsigned hw_threads = std::thread::hardware_concurrency();
    const int n_threads = hw_threads == 0
        ? 4
        : std::clamp(static_cast<int>(hw_threads / 2u), 4, 16);

    Dav1dSettings settings;
    dav1d_default_settings(&settings);
    settings.n_threads = n_threads;
    settings.max_frame_delay = 1;

    int ret = dav1d_open(&ctx_, &settings);
    if (ret < 0) {
        LOG_ERROR("dav1d_open failed: {}", ret);
        return false;
    }

    LOG_INFO("dav1d decoder initialized with {} threads (hardware concurrency {}), max_frame_delay 1",
             n_threads, hw_threads);
    return true;
}

void Dav1dDecoder::drain() {
    ZoneScopedN("Dav1dDecoder::drain");
    Dav1dPicture pic = {};
    while (dav1d_get_picture(ctx_, &pic) == 0) {
        if (on_decoded) {
            DecodedFrame f{};
            f.y_plane   = static_cast<const uint8_t*>(pic.data[0]);
            f.u_plane   = static_cast<const uint8_t*>(pic.data[1]);
            f.v_plane   = static_cast<const uint8_t*>(pic.data[2]);
            f.y_stride  = static_cast<uint32_t>(pic.stride[0]);
            f.uv_stride = static_cast<uint32_t>(pic.stride[1]);
            f.width     = static_cast<uint32_t>(pic.p.w);
            f.height    = static_cast<uint32_t>(pic.p.h);
            f.timestamp = pic.m.timestamp;
            on_decoded(f);
        }
        dav1d_picture_unref(&pic);
    }
}

bool Dav1dDecoder::decode(const uint8_t* data, size_t len, int64_t timestamp) {
    ZoneScopedN("Dav1dDecoder::decode");
    if (!ctx_) return false;

    Dav1dData dav1d_data = {};
    uint8_t* buf = dav1d_data_create(&dav1d_data, len);
    if (!buf) return false;
    std::memcpy(buf, data, len);
    dav1d_data.m.timestamp = timestamp;

    while (dav1d_data.sz > 0) {
        int ret = dav1d_send_data(ctx_, &dav1d_data);
        if (ret < 0 && ret != DAV1D_ERR(EAGAIN)) {
            LOG_ERROR("dav1d_send_data failed: {} (len={})", ret, len);
            dav1d_data_unref(&dav1d_data);
            return false;
        }
        drain();
    }
    return true;
}

void Dav1dDecoder::flush() {
    ZoneScopedN("Dav1dDecoder::flush");
    if (ctx_) drain();
}

DecoderInfo Dav1dDecoder::info() const {
    return {Backend::Software, codec_};
}

} // namespace parties::encdec::dav1d
