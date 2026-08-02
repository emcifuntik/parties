#pragma once

#include <encdec/decoder.h>
#include "nvidia_loader.h"

#include <cstdint>
#include <vector>

struct ID3D12Device;

namespace parties::encdec::nvidia {

struct NvdecCudaInteropState;

class NvdecDecoder final : public Decoder {
public:
    NvdecDecoder();
    ~NvdecDecoder() override;

    bool init(VideoCodecId codec, uint32_t width, uint32_t height,
              ID3D12Device* render_device = nullptr);

    bool decode(const uint8_t* data, size_t len, int64_t timestamp) override;
    void flush() override;
    bool context_lost() const override { return context_lost_; }
    DecoderInfo info() const override;

private:
    struct PacketDiagnostics {
        uint64_t packet_id = 0;
        size_t payload_size = 0;
        int64_t timestamp = 0;

        uint64_t context_push_us = 0;
        uint64_t parser_us = 0;
        uint64_t context_pop_us = 0;
        uint64_t sequence_us = 0;
        uint64_t decoder_destroy_us = 0;
        uint64_t pool_create_us = 0;
        uint64_t decoder_create_us = 0;
        uint64_t surface_register_us = 0;
        uint64_t pinned_alloc_us = 0;
        uint64_t decode_callback_us = 0;
        uint64_t surface_wait_us = 0;
        uint64_t decode_submit_us = 0;
        uint64_t display_callback_us = 0;
        uint64_t map_us = 0;
        uint64_t gpu_copy_us = 0;
        uint64_t fence_signal_us = 0;
        uint64_t stream_sync_us = 0;
        uint64_t unmap_us = 0;
        uint64_t deliver_us = 0;

        uint32_t sequence_callbacks = 0;
        uint32_t sequence_reuses = 0;
        uint32_t sequence_reconfigurations = 0;
        uint32_t pool_attempts = 0;
        uint32_t decode_callbacks = 0;
        uint32_t display_callbacks = 0;
        int32_t last_decode_surface = -1;
        int32_t last_display_surface = -1;
        const char* output_path = "none";
    };

    static int CUDAAPI handle_sequence(void* user, CUVIDEOFORMAT* fmt);
    static int CUDAAPI handle_decode(void* user, CUVIDPICPARAMS* pic);
    static int CUDAAPI handle_display(void* user, CUVIDPARSERDISPINFO* info);

    int on_sequence(CUVIDEOFORMAT* fmt);
    int on_decode(CUVIDPICPARAMS* pic);
    int on_display(CUVIDPARSERDISPINFO* info);
    void log_slow_packet(uint64_t total_us) const;

    CudaApi cuda_{};
    CuvidApi cuvid_{};

    std::shared_ptr<NvdecCudaInteropState> interop_;

    CUcontext cu_ctx_ = nullptr;
    CUvideoparser parser_ = nullptr;
    CUvideodecoder decoder_ = nullptr;

    VideoCodecId codec_ = VideoCodecId::AV1;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t bit_depth_ = 8;
    uint32_t num_decode_surfaces_ = 0;
    uint32_t native_texture_width_ = 0;
    uint32_t native_texture_height_ = 0;
    uint32_t native_crop_x_ = 0;
    uint32_t native_crop_y_ = 0;

    uint8_t* pinned_nv12_ = nullptr;
    size_t pinned_nv12_size_ = 0;

    bool initialized_ = false;
    bool context_lost_ = false;
    bool native_interop_active_ = false;
    bool opaque_output_active_ = false;
    bool opaque_output_capable_ = false;
    bool collect_packet_diagnostics_ = false;
    uint64_t next_packet_id_ = 0;
    PacketDiagnostics packet_diagnostics_{};
};

} // namespace parties::encdec::nvidia
