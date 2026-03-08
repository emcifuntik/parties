// NVDEC hardware decoder for AV1 using CUVID parser + CUDA context.
#include "nvdec_decoder.h"

#include <cstdio>
#include <cstring>
#include <parties/profiler.h>

namespace parties::client::nvidia {

NvdecDecoder::NvdecDecoder() = default;

NvdecDecoder::~NvdecDecoder() {
    shutdown();
}

bool NvdecDecoder::init(uint32_t width, uint32_t height) {
    ZoneScopedN("NvdecDecoder::init");
    if (initialized_) return false;

    // Load CUDA and CUVID
    if (!load_cuda(cuda_)) return false;
    if (!load_cuvid(cuvid_)) return false;

    // Create CUDA context on device 0 (required before any CUVID calls)
    CUdevice cu_device = 0;
    CUresult res = cuda_.cuDeviceGet(&cu_device, 0);
    if (res != CUDA_SUCCESS) {
        std::fprintf(stderr, "[NVDEC] cuDeviceGet failed: %d\n", res);
        return false;
    }

    res = cuda_.cuCtxCreate(&cu_ctx_, CU_CTX_SCHED_AUTO, cu_device);
    if (res != CUDA_SUCCESS) {
        std::fprintf(stderr, "[NVDEC] cuCtxCreate failed: %d\n", res);
        return false;
    }

    // Check AV1 decode capability (requires active CUDA context)
    CUVIDDECODECAPS caps{};
    caps.eCodecType = cudaVideoCodec_AV1;
    caps.eChromaFormat = cudaVideoChromaFormat_420;
    caps.nBitDepthMinus8 = 0;  // 8-bit

    res = cuvid_.cuvidGetDecoderCaps(&caps);
    if (res != CUDA_SUCCESS || !caps.bIsSupported) {
        std::fprintf(stderr, "[NVDEC] AV1 decode not supported by hardware (res=%d)\n", res);
        cuda_.cuCtxDestroy(cu_ctx_);
        cu_ctx_ = nullptr;
        return false;
    }

    if (width > caps.nMaxWidth || height > caps.nMaxHeight) {
        std::fprintf(stderr, "[NVDEC] Resolution %ux%u exceeds max %ux%u\n",
                     width, height, caps.nMaxWidth, caps.nMaxHeight);
        cuda_.cuCtxDestroy(cu_ctx_);
        cu_ctx_ = nullptr;
        return false;
    }

    width_ = width;
    height_ = height;

    // Create video parser (drives decode callbacks)
    CUVIDPARSERPARAMS parser_params{};
    parser_params.CodecType = cudaVideoCodec_AV1;
    parser_params.ulMaxNumDecodeSurfaces = 10;
    parser_params.ulMaxDisplayDelay = 1;  // Low latency
    parser_params.pUserData = this;
    parser_params.pfnSequenceCallback = handle_sequence;
    parser_params.pfnDecodePicture = handle_decode;
    parser_params.pfnDisplayPicture = handle_display;

    res = cuvid_.cuvidCreateVideoParser(&parser_, &parser_params);
    if (res != CUDA_SUCCESS) {
        std::fprintf(stderr, "[NVDEC] cuvidCreateVideoParser failed: %d\n", res);
        cuda_.cuCtxDestroy(cu_ctx_);
        cu_ctx_ = nullptr;
        return false;
    }

    std::fprintf(stderr, "[NVDEC] Initialized AV1 decoder (NVDEC engines: %u)\n",
                 caps.nNumNVDECs);

    initialized_ = true;
    return true;
}

void NvdecDecoder::shutdown() {
    if (!initialized_) return;

    if (cu_ctx_) {
        cuda_.cuCtxPushCurrent(cu_ctx_);

        if (parser_) {
            cuvid_.cuvidDestroyVideoParser(parser_);
            parser_ = nullptr;
        }

        if (decoder_) {
            cuvid_.cuvidDestroyDecoder(decoder_);
            decoder_ = nullptr;
        }

        if (pinned_nv12_) {
            cuda_.cuMemFreeHost(pinned_nv12_);
            pinned_nv12_ = nullptr;
            pinned_nv12_size_ = 0;
        }

        CUcontext dummy;
        cuda_.cuCtxPopCurrent(&dummy);
        cuda_.cuCtxDestroy(cu_ctx_);
        cu_ctx_ = nullptr;
    }

    initialized_ = false;
}

bool NvdecDecoder::decode(const uint8_t* data, size_t len, int64_t timestamp) {
    ZoneScopedN("NvdecDecoder::decode");
    if (!initialized_) return false;

    cuda_.cuCtxPushCurrent(cu_ctx_);

    CUVIDSOURCEDATAPACKET pkt{};
    pkt.flags = CUVID_PKT_TIMESTAMP;
    pkt.payload_size = static_cast<unsigned long>(len);
    pkt.payload = data;
    pkt.timestamp = timestamp;

    CUresult res = cuvid_.cuvidParseVideoData(parser_, &pkt);

    CUcontext dummy;
    cuda_.cuCtxPopCurrent(&dummy);

    if (res != CUDA_SUCCESS) {
        std::fprintf(stderr, "[NVDEC] cuvidParseVideoData failed: %d\n", res);
        return false;
    }

    return true;
}

void NvdecDecoder::flush() {
    ZoneScopedN("NvdecDecoder::flush");
    if (!initialized_) return;

    cuda_.cuCtxPushCurrent(cu_ctx_);

    CUVIDSOURCEDATAPACKET pkt{};
    pkt.flags = CUVID_PKT_ENDOFSTREAM;
    cuvid_.cuvidParseVideoData(parser_, &pkt);

    CUcontext dummy;
    cuda_.cuCtxPopCurrent(&dummy);
}

// Static parser callbacks
int NvdecDecoder::handle_sequence(void* user, CUVIDEOFORMAT* fmt) {
    return static_cast<NvdecDecoder*>(user)->on_sequence(fmt);
}

int NvdecDecoder::handle_decode(void* user, CUVIDPICPARAMS* pic) {
    return static_cast<NvdecDecoder*>(user)->on_decode(pic);
}

int NvdecDecoder::handle_display(void* user, CUVIDPARSERDISPINFO* info) {
    return static_cast<NvdecDecoder*>(user)->on_display(info);
}

int NvdecDecoder::on_sequence(CUVIDEOFORMAT* fmt) {
    ZoneScopedN("NvdecDecoder::on_sequence");

    std::fprintf(stderr, "[NVDEC] on_sequence: %ux%u, bit_depth=%u, chroma=%d, surfaces=%u\n",
                 fmt->coded_width, fmt->coded_height,
                 fmt->bit_depth_luma_minus8 + 8, fmt->chroma_format,
                 fmt->min_num_decode_surfaces);

    // (Re)create the decoder based on stream parameters
    if (decoder_) {
        cuvid_.cuvidDestroyDecoder(decoder_);
        decoder_ = nullptr;
    }

    width_ = fmt->coded_width;
    height_ = fmt->coded_height;
    bit_depth_ = fmt->bit_depth_luma_minus8 + 8;
    num_decode_surfaces_ = fmt->min_num_decode_surfaces + 4;  // Extra surfaces for display pipeline

    // 10-bit content → P016 output, 8-bit → NV12
    bool is_10bit = fmt->bit_depth_luma_minus8 > 0;
    auto output_fmt = is_10bit ? cudaVideoSurfaceFormat_P016
                               : cudaVideoSurfaceFormat_NV12;

    CUVIDDECODECREATEINFO create_info{};
    create_info.ulWidth = fmt->coded_width;
    create_info.ulHeight = fmt->coded_height;
    create_info.ulNumDecodeSurfaces = num_decode_surfaces_;
    create_info.CodecType = fmt->codec;
    create_info.ChromaFormat = fmt->chroma_format;
    create_info.ulCreationFlags = cudaVideoCreate_PreferCUVID;
    create_info.bitDepthMinus8 = fmt->bit_depth_luma_minus8;
    create_info.OutputFormat = output_fmt;
    create_info.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
    create_info.ulTargetWidth = fmt->coded_width;
    create_info.ulTargetHeight = fmt->coded_height;
    create_info.ulNumOutputSurfaces = 2;

    // Use display area if valid
    if (fmt->display_area.right > fmt->display_area.left &&
        fmt->display_area.bottom > fmt->display_area.top) {
        create_info.display_area.left = static_cast<short>(fmt->display_area.left);
        create_info.display_area.top = static_cast<short>(fmt->display_area.top);
        create_info.display_area.right = static_cast<short>(fmt->display_area.right);
        create_info.display_area.bottom = static_cast<short>(fmt->display_area.bottom);
        create_info.ulTargetWidth = fmt->display_area.right - fmt->display_area.left;
        create_info.ulTargetHeight = fmt->display_area.bottom - fmt->display_area.top;
        width_ = create_info.ulTargetWidth;
        height_ = create_info.ulTargetHeight;
    }

    CUresult res = cuvid_.cuvidCreateDecoder(&decoder_, &create_info);
    if (res != CUDA_SUCCESS) {
        std::fprintf(stderr, "[NVDEC] cuvidCreateDecoder failed: %d\n", res);
        return 0;  // Signal failure
    }

    // Allocate pinned host memory for decoded NV12 surface download
    size_t nv12_size = static_cast<size_t>(width_) * height_ * 3 / 2;
    if (nv12_size > pinned_nv12_size_) {
        if (pinned_nv12_) cuda_.cuMemFreeHost(pinned_nv12_);
        pinned_nv12_ = nullptr;
        pinned_nv12_size_ = 0;

        void* ptr = nullptr;
        res = cuda_.cuMemAllocHost(&ptr, nv12_size);
        if (res == CUDA_SUCCESS) {
            pinned_nv12_ = static_cast<uint8_t*>(ptr);
            pinned_nv12_size_ = nv12_size;
        }
    }

    return static_cast<int>(num_decode_surfaces_);
}

int NvdecDecoder::on_decode(CUVIDPICPARAMS* pic) {
    ZoneScopedN("NvdecDecoder::on_decode");
    if (!decoder_) return 0;

    CUresult res = cuvid_.cuvidDecodePicture(decoder_, pic);
    if (res != CUDA_SUCCESS) {
        std::fprintf(stderr, "[NVDEC] cuvidDecodePicture failed: %d\n", res);
        return 0;
    }
    return 1;
}

int NvdecDecoder::on_display(CUVIDPARSERDISPINFO* info) {
    ZoneScopedN("NvdecDecoder::on_display");
    if (!decoder_ || !info || !on_decoded) return 1;

    CUVIDPROCPARAMS proc{};
    proc.progressive_frame = info->progressive_frame;
    proc.top_field_first = info->top_field_first;

    unsigned long long dev_ptr = 0;
    unsigned int pitch = 0;

    CUresult res = cuvid_.cuvidMapVideoFrame64(
        decoder_, info->picture_index, &dev_ptr, &pitch, &proc);
    if (res != CUDA_SUCCESS) {
        std::fprintf(stderr, "[NVDEC] cuvidMapVideoFrame64 failed: %d\n", res);
        return 0;
    }

    // Copy decoded NV12 surface from GPU to pinned host memory
    size_t host_size = static_cast<size_t>(width_) * height_ * 3 / 2;

    {
        ZoneScopedN("nvdec::gpu_to_host");

        if (pinned_nv12_ && pinned_nv12_size_ >= host_size) {
            CUDA_MEMCPY2D copy{};

            // Copy Y plane
            copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            copy.srcDevice = dev_ptr;
            copy.srcPitch = pitch;
            copy.dstMemoryType = CU_MEMORYTYPE_HOST;
            copy.dstHost = pinned_nv12_;
            copy.dstPitch = width_;
            copy.WidthInBytes = width_;
            copy.Height = height_;
            CUresult r = cuda_.cuMemcpy2D(&copy);
            if (r != CUDA_SUCCESS) {
                std::fprintf(stderr, "[NVDEC] cuMemcpy2D Y failed: %d\n", r);
            }

            // Copy UV plane (interleaved, height/2 rows, same width in bytes as Y)
            copy.srcDevice = dev_ptr + static_cast<CUdeviceptr>(pitch) * height_;
            copy.dstHost = pinned_nv12_ + static_cast<size_t>(width_) * height_;
            copy.Height = height_ / 2;
            r = cuda_.cuMemcpy2D(&copy);
            if (r != CUDA_SUCCESS) {
                std::fprintf(stderr, "[NVDEC] cuMemcpy2D UV failed: %d\n", r);
            }
        }
    }

    cuvid_.cuvidUnmapVideoFrame64(decoder_, dev_ptr);

    // Deliver NV12 frame directly — no CPU conversion needed.
    // The GPU pixel shader handles NV12 → RGB.
    uint32_t y_size = width_ * height_;

    parties::client::DecodedFrame frame{};
    frame.y_plane = pinned_nv12_;
    frame.u_plane = pinned_nv12_ + y_size;  // interleaved UV
    frame.v_plane = nullptr;
    frame.y_stride = width_;
    frame.uv_stride = width_;               // NV12: UV plane has same width in bytes as Y
    frame.width = width_;
    frame.height = height_;
    frame.timestamp = info->timestamp;
    frame.nv12 = true;

    on_decoded(frame);
    return 1;
}

} // namespace parties::client::nvidia
