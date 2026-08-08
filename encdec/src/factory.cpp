#include <encdec/factory.h>

#include "nvidia/nvenc_encoder.h"
#include "nvidia/nvdec_decoder.h"
#include "amd/amf_encoder.h"
#include "amd/amf_decoder.h"
#include "mft/mft_encoder.h"
#include "mft/mft_decoder.h"
#include "dav1d/dav1d_decoder.h"
#include "openh264/openh264_encoder.h"
#include "openh264/openh264_decoder.h"
#include "libhevc/libhevc_decoder.h"

#include <parties/log.h>

namespace parties::encdec {

std::unique_ptr<Encoder> create_encoder(
    ID3D11Device* device,
    uint32_t width, uint32_t height,
    uint32_t input_width, uint32_t input_height,
    uint32_t fps, uint32_t bitrate,
    VideoCodecId preferred_codec) {

    // Even dimensions for NV12
    uint32_t enc_w = (width + 1) & ~1u;
    uint32_t enc_h = (height + 1) & ~1u;

    // Try NVENC first
    {
        auto enc = std::make_unique<nvidia::NvencEncoder>();
        if (enc->init(device, enc_w, enc_h, fps, bitrate, preferred_codec)) {
            LOG_INFO("Selected video encoder backend={} codec={} size={}x{} fps={} bitrate={}",
                     backend_name(enc->info().backend), codec_name(enc->info().codec),
                     enc_w, enc_h, fps, bitrate);
            return enc;
        }
    }

    // Try AMF second
    {
        auto enc = std::make_unique<amd::AmfEncoder>();
        if (enc->init(device, enc_w, enc_h, fps, bitrate, preferred_codec)) {
            LOG_INFO("Selected video encoder backend={} codec={} size={}x{} fps={} bitrate={}",
                     backend_name(enc->info().backend), codec_name(enc->info().codec),
                     enc_w, enc_h, fps, bitrate);
            return enc;
        }
    }

    // Try MFT fallback
    {
        auto enc = std::make_unique<mft::MftEncoder>();
        if (enc->init(device, enc_w, enc_h, input_width, input_height,
                      fps, bitrate, preferred_codec)) {
            LOG_INFO("Selected video encoder backend={} codec={} size={}x{} fps={} bitrate={}",
                     backend_name(enc->info().backend), codec_name(enc->info().codec),
                     enc_w, enc_h, fps, bitrate);
            return enc;
        }
    }

    // Last resort: OpenH264 software encoder (H.264 only)
    {
        auto enc = std::make_unique<openh264::OpenH264Encoder>();
        if (enc->init(device, enc_w, enc_h, fps, bitrate)) {
            LOG_INFO("Selected video encoder backend={} codec={} size={}x{} fps={} bitrate={}",
                     backend_name(enc->info().backend), codec_name(enc->info().codec),
                     enc_w, enc_h, fps, bitrate);
            return enc;
        }
    }

    LOG_ERROR("No encoder available");
    return nullptr;
}

std::unique_ptr<Decoder> create_decoder(
    VideoCodecId codec,
    uint32_t width, uint32_t height,
    ID3D12Device* render_device) {

    // Try NVDEC first
    {
        auto dec = std::make_unique<nvidia::NvdecDecoder>();
        if (dec->init(codec, width, height, render_device)) {
            LOG_INFO("Selected video decoder backend={} codec={} size={}x{}",
                     backend_name(dec->info().backend), codec_name(codec), width, height);
            return dec;
        }
    }

    // Try AMF second
    {
        auto dec = std::make_unique<amd::AmfDecoder>();
        if (dec->init(codec, width, height, render_device)) {
            LOG_INFO("Selected video decoder backend={} codec={} size={}x{}",
                     backend_name(dec->info().backend), codec_name(codec), width, height);
            return dec;
        }
    }

    // Software fallback
    return create_software_decoder(codec, width, height);
}

std::unique_ptr<Decoder> create_software_decoder(
    VideoCodecId codec,
    uint32_t width, uint32_t height) {

    if (codec == VideoCodecId::AV1) {
        auto dec = std::make_unique<dav1d::Dav1dDecoder>();
        if (dec->init(codec, width, height)) {
            LOG_INFO("Selected video decoder backend={} codec={} size={}x{}",
                     backend_name(dec->info().backend), codec_name(codec), width, height);
            return dec;
        }
    } else if (codec == VideoCodecId::H265) {
        // H.265: try MFT first, then libhevc software fallback
        {
            auto dec = std::make_unique<mft::MftDecoder>();
            if (dec->init(codec, width, height)) {
                LOG_INFO("Selected video decoder backend={} codec={} size={}x{}",
                         backend_name(dec->info().backend), codec_name(codec), width, height);
                return dec;
            }
        }
        {
            auto dec = std::make_unique<libhevc::LibhevcDecoder>();
            if (dec->init(codec, width, height)) {
                LOG_INFO("Selected video decoder backend={} codec={} size={}x{}",
                         backend_name(dec->info().backend), codec_name(codec), width, height);
                return dec;
            }
        }
    } else {
        // H.264: try MFT first, then OpenH264
        {
            auto dec = std::make_unique<mft::MftDecoder>();
            if (dec->init(codec, width, height)) {
                LOG_INFO("Selected video decoder backend={} codec={} size={}x{}",
                         backend_name(dec->info().backend), codec_name(codec), width, height);
                return dec;
            }
        }
        {
            auto dec = std::make_unique<openh264::OpenH264Decoder>();
            if (dec->init(codec, width, height)) {
                LOG_INFO("Selected video decoder backend={} codec={} size={}x{}",
                         backend_name(dec->info().backend), codec_name(codec), width, height);
                return dec;
            }
        }
    }

    LOG_ERROR("No decoder available for {}", codec_name(codec));
    return nullptr;
}

} // namespace parties::encdec
