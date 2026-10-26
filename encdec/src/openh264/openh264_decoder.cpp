#include "openh264_decoder.h"

#include <wels/codec_api.h>
#include <wels/codec_def.h>

#include <parties/log.h>
#include <parties/profiler.h>

namespace parties::encdec::openh264 {

OpenH264Decoder::~OpenH264Decoder() {
    if (decoder_) {
        decoder_->Uninitialize();
        WelsDestroyDecoder(decoder_);
    }
}

bool OpenH264Decoder::init(VideoCodecId codec, uint32_t, uint32_t) {
    if (codec != VideoCodecId::H264)
        return false;

    long ret = WelsCreateDecoder(&decoder_);
    if (ret != 0 || !decoder_) {
        LOG_ERROR("WelsCreateDecoder failed: {}", ret);
        return false;
    }

    SDecodingParam params = {};
    params.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    params.eEcActiveIdc = ERROR_CON_SLICE_COPY;

    ret = decoder_->Initialize(&params);
    if (ret != cmResultSuccess) {
        LOG_ERROR("Decoder Initialize failed: {}", ret);
        WelsDestroyDecoder(decoder_);
        decoder_ = nullptr;
        return false;
    }

    return true;
}

bool OpenH264Decoder::decode(const uint8_t* data, size_t len, int64_t timestamp) {
    ZoneScopedN("OpenH264Decoder::decode");
    if (!decoder_) return false;

    unsigned char* dst[3] = {};
    SBufferInfo buf_info = {};

    DECODING_STATE state = decoder_->DecodeFrameNoDelay(
        data, static_cast<int>(len), dst, &buf_info);

    if (state != dsErrorFree && state != dsFramePending) {
        return true;
    }

    if (buf_info.iBufferStatus == 1 && dst[0] && on_decoded) {
        DecodedFrame f{};
        f.y_plane   = dst[0];
        f.u_plane   = dst[1];
        f.v_plane   = dst[2];
        f.y_stride  = static_cast<uint32_t>(buf_info.UsrData.sSystemBuffer.iStride[0]);
        f.uv_stride = static_cast<uint32_t>(buf_info.UsrData.sSystemBuffer.iStride[1]);
        f.width     = static_cast<uint32_t>(buf_info.UsrData.sSystemBuffer.iWidth);
        f.height    = static_cast<uint32_t>(buf_info.UsrData.sSystemBuffer.iHeight);
        f.timestamp = timestamp;
        on_decoded(f);
    }

    return true;
}

void OpenH264Decoder::flush() {
    ZoneScopedN("OpenH264Decoder::flush");
    if (!decoder_) return;

    unsigned char* dst[3] = {};
    SBufferInfo buf_info = {};

    while (true) {
        DECODING_STATE state = decoder_->FlushFrame(dst, &buf_info);
        if (state != dsErrorFree || buf_info.iBufferStatus != 1)
            break;

        if (dst[0] && on_decoded) {
            DecodedFrame f{};
            f.y_plane   = dst[0];
            f.u_plane   = dst[1];
            f.v_plane   = dst[2];
            f.y_stride  = static_cast<uint32_t>(buf_info.UsrData.sSystemBuffer.iStride[0]);
            f.uv_stride = static_cast<uint32_t>(buf_info.UsrData.sSystemBuffer.iStride[1]);
            f.width     = static_cast<uint32_t>(buf_info.UsrData.sSystemBuffer.iWidth);
            f.height    = static_cast<uint32_t>(buf_info.UsrData.sSystemBuffer.iHeight);
            f.timestamp = 0;
            on_decoded(f);
        }
    }
}

DecoderInfo OpenH264Decoder::info() const {
    return {Backend::OpenH264, VideoCodecId::H264};
}

}
