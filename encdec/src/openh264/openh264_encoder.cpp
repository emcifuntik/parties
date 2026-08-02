#include "openh264_encoder.h"
#include <encdec/rate_control.h>

#include <wels/codec_api.h>
#include <wels/codec_def.h>

#include <cstring>
#include <parties/log.h>
#include <parties/profiler.h>
#include <parties/video_common.h>

using Microsoft::WRL::ComPtr;

namespace parties::encdec::openh264 {

OpenH264Encoder::~OpenH264Encoder() {
    if (encoder_) {
        encoder_->Uninitialize();
        WelsDestroySVCEncoder(encoder_);
    }
}

bool OpenH264Encoder::init(ID3D11Device* device, uint32_t width, uint32_t height,
                            uint32_t fps, uint32_t bitrate) {
    device_ = device;
    device_->GetImmediateContext(&context_);
    width_ = width;
    height_ = height;
    fps_ = fps;

    int ret = WelsCreateSVCEncoder(&encoder_);
    if (ret != 0 || !encoder_) {
        LOG_ERROR("WelsCreateSVCEncoder failed: {}", ret);
        return false;
    }

    SEncParamExt params;
    encoder_->GetDefaultParams(&params);

    params.iUsageType = SCREEN_CONTENT_REAL_TIME;
    params.iPicWidth = static_cast<int>(width);
    params.iPicHeight = static_cast<int>(height);
    const auto rate_control = make_stream_vbr_rate_control(bitrate);
    params.iTargetBitrate = static_cast<int>(rate_control.average_bitrate);
    params.iMaxBitrate = static_cast<int>(rate_control.peak_bitrate);
    params.iRCMode = RC_BITRATE_MODE;
    params.fMaxFrameRate = static_cast<float>(fps);
    params.bEnableFrameSkip = false;
    params.iMultipleThreadIdc = 0;  // auto
    params.uiIntraPeriod = fps * (parties::VIDEO_KEYFRAME_INTERVAL_MS / 1000);
    params.iNumRefFrame = 1;
    params.bEnableDenoise = false;
    params.bEnableAdaptiveQuant = true;
    params.bEnableLongTermReference = false;

    params.iSpatialLayerNum = 1;
    params.sSpatialLayers[0].iVideoWidth = static_cast<int>(width);
    params.sSpatialLayers[0].iVideoHeight = static_cast<int>(height);
    params.sSpatialLayers[0].fFrameRate = static_cast<float>(fps);
    params.sSpatialLayers[0].iSpatialBitrate =
        static_cast<int>(rate_control.average_bitrate);
    params.sSpatialLayers[0].iMaxSpatialBitrate =
        static_cast<int>(rate_control.peak_bitrate);
    params.sSpatialLayers[0].uiProfileIdc = PRO_BASELINE;
    params.sSpatialLayers[0].uiLevelIdc = LEVEL_UNKNOWN;

    ret = encoder_->InitializeExt(&params);
    if (ret != cmResultSuccess) {
        LOG_ERROR("InitializeExt failed: {}", ret);
        WelsDestroySVCEncoder(encoder_);
        encoder_ = nullptr;
        return false;
    }

    // Allocate I420 buffer for BGRA→I420 conversion
    i420_buffer_.resize(static_cast<size_t>(width) * height * 3 / 2);

    // Create staging texture for GPU→CPU readback
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &staging_);
    if (FAILED(hr)) {
        LOG_ERROR("CreateTexture2D staging failed: {:#010x}", hr);
        encoder_->Uninitialize();
        WelsDestroySVCEncoder(encoder_);
        encoder_ = nullptr;
        return false;
    }

    return true;
}

bool OpenH264Encoder::encode(ID3D11Texture2D* bgra_texture, int64_t timestamp_100ns) {
    ZoneScopedN("OpenH264Encoder::encode");
    if (!encoder_) return false;

    // Force keyframe if requested
    if (force_keyframe_) {
        force_keyframe_ = false;
        encoder_->ForceIntraFrame(true);
    }

    // GPU → CPU readback
    context_->CopyResource(staging_.Get(), bgra_texture);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    // BGRA → I420 conversion (BT.601)
    {
        ZoneScopedN("BGRA_to_I420");
        const uint8_t* bgra = static_cast<const uint8_t*>(mapped.pData);
        uint32_t bgra_stride = mapped.RowPitch;

        uint8_t* y_plane = i420_buffer_.data();
        uint8_t* u_plane = y_plane + width_ * height_;
        uint8_t* v_plane = u_plane + (width_ / 2) * (height_ / 2);

        for (uint32_t y = 0; y < height_; y++) {
            const uint8_t* row = bgra + y * bgra_stride;
            uint8_t* y_row = y_plane + y * width_;

            for (uint32_t x = 0; x < width_; x++) {
                uint8_t b = row[x * 4 + 0];
                uint8_t g = row[x * 4 + 1];
                uint8_t r = row[x * 4 + 2];

                // Y = 0.299*R + 0.587*G + 0.114*B (fixed-point: >>8)
                y_row[x] = static_cast<uint8_t>((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            }

            // Subsample U/V every 2x2 block
            if ((y & 1) == 0) {
                uint32_t uv_y = y / 2;
                uint8_t* u_row = u_plane + uv_y * (width_ / 2);
                uint8_t* v_row = v_plane + uv_y * (width_ / 2);

                for (uint32_t x = 0; x < width_; x += 2) {
                    // Average 2x2 block (top-left pixel as approximation for speed)
                    uint8_t b = row[x * 4 + 0];
                    uint8_t g = row[x * 4 + 1];
                    uint8_t r = row[x * 4 + 2];

                    u_row[x / 2] = static_cast<uint8_t>((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                    v_row[x / 2] = static_cast<uint8_t>((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                }
            }
        }
    }

    context_->Unmap(staging_.Get(), 0);

    // Encode
    SSourcePicture pic = {};
    pic.iColorFormat = videoFormatI420;
    pic.iPicWidth = static_cast<int>(width_);
    pic.iPicHeight = static_cast<int>(height_);
    pic.uiTimeStamp = timestamp_100ns / 10000;  // 100ns → ms
    pic.iStride[0] = static_cast<int>(width_);
    pic.iStride[1] = static_cast<int>(width_ / 2);
    pic.iStride[2] = static_cast<int>(width_ / 2);
    pic.pData[0] = i420_buffer_.data();
    pic.pData[1] = pic.pData[0] + width_ * height_;
    pic.pData[2] = pic.pData[1] + (width_ / 2) * (height_ / 2);

    SFrameBSInfo bs_info = {};
    int ret = encoder_->EncodeFrame(&pic, &bs_info);
    if (ret != cmResultSuccess) {
        LOG_ERROR("EncodeFrame failed: {}", ret);
        return false;
    }

    if (bs_info.eFrameType == videoFrameTypeSkip)
        return true;  // No output this frame

    // Collect all NAL layers into a single buffer and fire callback
    if (on_encoded) {
        bool keyframe = (bs_info.eFrameType == videoFrameTypeIDR ||
                         bs_info.eFrameType == videoFrameTypeI);

        // Concatenate all layer NALs
        std::vector<uint8_t> output;
        for (int i = 0; i < bs_info.iLayerNum; i++) {
            const SLayerBSInfo& layer = bs_info.sLayerInfo[i];
            int offset = 0;
            for (int j = 0; j < layer.iNalCount; j++) {
                output.insert(output.end(),
                              layer.pBsBuf + offset,
                              layer.pBsBuf + offset + layer.pNalLengthInByte[j]);
                offset += layer.pNalLengthInByte[j];
            }
        }

        if (!output.empty())
            on_encoded(output.data(), output.size(), keyframe);
    }

    return true;
}

void OpenH264Encoder::force_keyframe() {
    force_keyframe_ = true;
}

void OpenH264Encoder::set_bitrate(uint32_t bitrate) {
    if (!encoder_) return;

    const auto rate_control = make_stream_vbr_rate_control(bitrate);
    SBitrateInfo info = {};
    info.iLayer = SPATIAL_LAYER_ALL;
    info.iBitrate = static_cast<int>(rate_control.average_bitrate);
    encoder_->SetOption(ENCODER_OPTION_BITRATE, &info);

    float max_br = static_cast<float>(rate_control.peak_bitrate);
    encoder_->SetOption(ENCODER_OPTION_MAX_BITRATE, &max_br);
}

EncoderInfo OpenH264Encoder::info() const {
    return {Backend::OpenH264, VideoCodecId::H264, width_, height_};
}

} // namespace parties::encdec::openh264
