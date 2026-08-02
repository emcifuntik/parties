#pragma once

#include <encdec/encoder.h>
#include <encdec/decoder.h>

#include <memory>

struct ID3D11Device;
struct ID3D12Device;

namespace parties::encdec {

// Create the best available encoder.
// Tries NVENC → AMF → MFT. Returns nullptr if nothing works.
// input_width/input_height are only used by MFT (for BGRA→NV12 color converter);
// hardware encoders ignore them. Pass 0 to default to output dimensions.
std::unique_ptr<Encoder> create_encoder(
    ID3D11Device* device,
    uint32_t width, uint32_t height,
    uint32_t input_width = 0, uint32_t input_height = 0,
    uint32_t fps = 30,
    uint32_t bitrate = parties::VIDEO_DEFAULT_BITRATE,
    VideoCodecId preferred_codec = VideoCodecId::AV1);

// Create the best available decoder for the given codec.
// Tries NVDEC → AMF → dav1d (AV1) / MFT (H.264/H.265).
std::unique_ptr<Decoder> create_decoder(
    VideoCodecId codec,
    uint32_t width = 0,
    uint32_t height = 0,
    ID3D12Device* render_device = nullptr);

// Create a software-only decoder (skips NVDEC/AMF).
// Used after GPU context loss to avoid re-triggering the same failure.
std::unique_ptr<Decoder> create_software_decoder(
    VideoCodecId codec,
    uint32_t width = 0,
    uint32_t height = 0);

} // namespace parties::encdec
