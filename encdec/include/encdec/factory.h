#pragma once

#include <encdec/encoder.h>
#include <encdec/decoder.h>

#include <memory>

struct ID3D11Device;
struct ID3D12Device;

namespace parties::encdec {

std::unique_ptr<Encoder> create_encoder(
    ID3D11Device* device,
    uint32_t width, uint32_t height,
    uint32_t fps = 30,
    uint32_t average_bps = parties::VIDEO_DEFAULT_BITRATE,
    VideoCodecId preferred_codec = VideoCodecId::AV1);

std::unique_ptr<Decoder> create_decoder(
    VideoCodecId codec,
    uint32_t width = 0,
    uint32_t height = 0,
    ID3D12Device* render_device = nullptr);

std::unique_ptr<Decoder> create_software_decoder(
    VideoCodecId codec,
    uint32_t width = 0,
    uint32_t height = 0);

}
