#include "nvdec_decoder.h"

#include <cstdio>
#include <cstring>
#include <parties/profiler.h>

#include <windows.h>

namespace parties::encdec::nvidia {

static CUresult seh_cuvidParseVideoData(
        decltype(CuvidApi::cuvidParseVideoData) fn,
        CUvideoparser parser, CUVIDSOURCEDATAPACKET* pkt) {
    __try {
        return fn(parser, pkt);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        return static_cast<CUresult>(999);
    }
}

NvdecDecoder::NvdecDecoder() = default;

NvdecDecoder::~NvdecDecoder() {
    if (!initialized_ && !context_lost_) return;

    if (cu_ctx_) {
        if (!context_lost_) {
            cuda_.cuCtxPushCurrent(cu_ctx_);

            if (parser_) cuvid_.cuvidDestroyVideoParser(parser_);
            if (decoder_) cuvid_.cuvidDestroyDecoder(decoder_);
            if (pinned_nv12_) cuda_.cuMemFreeHost(pinned_nv12_);

            CUcontext dummy;
            cuda_.cuCtxPopCurrent(&dummy);
        }
        cuda_.cuCtxDestroy(cu_ctx_);
    }

    cu_ctx_ = nullptr;
    parser_ = nullptr;
    decoder_ = nullptr;
    pinned_nv12_ = nullptr;
    pinned_nv12_size_ = 0;
    initialized_ = false;
    context_lost_ = false;
}

static cudaVideoCodec to_cuvid_codec(VideoCodecId id) {
    switch (id) {
    case VideoCodecId::H264: return cudaVideoCodec_H264;
    case VideoCodecId::H265: return cudaVideoCodec_HEVC;
    case VideoCodecId::AV1:  return cudaVideoCodec_AV1;
    default:                 return cudaVideoCodec_AV1;
    }
}

bool NvdecDecoder::init(VideoCodecId codec, uint32_t width, uint32_t height) {
    ZoneScopedN("NvdecDecoder::init");
    if (initialized_) return false;

    if (!load_cuda(cuda_)) return false;
    if (!load_cuvid(cuvid_)) return false;

    codec_ = codec;
    cudaVideoCodec cuvid_codec = to_cuvid_codec(codec);

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

    CUVIDDECODECAPS caps{};
    caps.eCodecType = cuvid_codec;
    caps.eChromaFormat = cudaVideoChromaFormat_420;
    caps.nBitDepthMinus8 = 0;

    res = cuvid_.cuvidGetDecoderCaps(&caps);
    if (res != CUDA_SUCCESS || !caps.bIsSupported) {
        std::fprintf(stderr, "[NVDEC] %s not supported (res=%d, supported=%d)\n",
                     codec_name(codec), res, caps.bIsSupported);
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

    CUVIDPARSERPARAMS parser_params{};
    parser_params.CodecType = cuvid_codec;
    parser_params.ulMaxNumDecodeSurfaces = 10;
    parser_params.ulMaxDisplayDelay = 0;  // No B-frames in our stream, display immediately
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

    CUcontext dummy;
    cuda_.cuCtxPopCurrent(&dummy);

    std::fprintf(stderr, "[NVDEC] Initialized %s decoder (%ux%u)\n",
                 codec_name(codec), width_, height_);
    initialized_ = true;
    return true;
}

bool NvdecDecoder::decode(const uint8_t* data, size_t len, int64_t timestamp) {
    ZoneScopedN("NvdecDecoder::decode");
    if (!initialized_ || context_lost_) return false;

    CUresult res = cuda_.cuCtxPushCurrent(cu_ctx_);
    if (res != CUDA_SUCCESS) {
        std::fprintf(stderr, "[NVDEC] CUDA context lost (cuCtxPushCurrent=%d)\n", res);
        context_lost_ = true;
        initialized_ = false;
        return false;
    }

    CUVIDSOURCEDATAPACKET pkt{};
    pkt.flags = CUVID_PKT_TIMESTAMP;
    pkt.payload_size = static_cast<unsigned long>(len);
    pkt.payload = data;
    pkt.timestamp = timestamp;

    res = seh_cuvidParseVideoData(cuvid_.cuvidParseVideoData, parser_, &pkt);

    CUcontext dummy;
    cuda_.cuCtxPopCurrent(&dummy);

    if (res != CUDA_SUCCESS) {
        std::fprintf(stderr, "[NVDEC] cuvidParseVideoData failed: %d (GPU context invalidated)\n", res);
        context_lost_ = true;
        initialized_ = false;
        return false;
    }

    return true;
}

void NvdecDecoder::flush() {
    ZoneScopedN("NvdecDecoder::flush");
    if (!initialized_ || context_lost_) return;

    if (cuda_.cuCtxPushCurrent(cu_ctx_) != CUDA_SUCCESS) return;

    CUVIDSOURCEDATAPACKET pkt{};
    pkt.flags = CUVID_PKT_ENDOFSTREAM;
    seh_cuvidParseVideoData(cuvid_.cuvidParseVideoData, parser_, &pkt);

    CUcontext dummy;
    cuda_.cuCtxPopCurrent(&dummy);
}

DecoderInfo NvdecDecoder::info() const {
    return {Backend::NVDEC, codec_};
}

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

    if (fmt->coded_width == 0 || fmt->coded_height == 0 ||
        fmt->chroma_format > cudaVideoChromaFormat_444) {
        std::fprintf(stderr, "[NVDEC] Ignoring invalid sequence header\n");
        return 1;
    }

    // Compute the actual output dimensions (may differ from coded_width/height
    // if the stream specifies a display/crop area).
    uint32_t target_w = fmt->coded_width;
    uint32_t target_h = fmt->coded_height;
    if (fmt->display_area.right > fmt->display_area.left &&
        fmt->display_area.bottom > fmt->display_area.top) {
        target_w = fmt->display_area.right - fmt->display_area.left;
        target_h = fmt->display_area.bottom - fmt->display_area.top;
    }

    // If the format hasn't changed, reuse the existing decoder.
    // Recreating on every keyframe's sequence header is expensive (GPU alloc)
    // and drops any frame buffered in the parser's display pipeline.
    if (decoder_ &&
        target_w == width_ && target_h == height_ &&
        fmt->bit_depth_luma_minus8 + 8 == bit_depth_ &&
        fmt->min_num_decode_surfaces + 4 <= num_decode_surfaces_) {
        return num_decode_surfaces_;
    }

    std::fprintf(stderr, "[NVDEC] on_sequence: recreating decoder (%ux%u bd=%u surfaces=%u -> %ux%u bd=%u surfaces=%u)\n",
                 width_, height_, bit_depth_, num_decode_surfaces_,
                 target_w, target_h, fmt->bit_depth_luma_minus8 + 8,
                 fmt->min_num_decode_surfaces + 4);

    if (decoder_) {
        cuvid_.cuvidDestroyDecoder(decoder_);
        decoder_ = nullptr;
    }

    bit_depth_ = fmt->bit_depth_luma_minus8 + 8;
    num_decode_surfaces_ = fmt->min_num_decode_surfaces + 4;
    width_ = target_w;
    height_ = target_h;

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
    create_info.ulTargetWidth = target_w;
    create_info.ulTargetHeight = target_h;
    create_info.ulNumOutputSurfaces = 2;

    if (fmt->display_area.right > fmt->display_area.left &&
        fmt->display_area.bottom > fmt->display_area.top) {
        create_info.display_area.left = static_cast<short>(fmt->display_area.left);
        create_info.display_area.top = static_cast<short>(fmt->display_area.top);
        create_info.display_area.right = static_cast<short>(fmt->display_area.right);
        create_info.display_area.bottom = static_cast<short>(fmt->display_area.bottom);
    }

    CUresult res = cuvid_.cuvidCreateDecoder(&decoder_, &create_info);
    if (res != CUDA_SUCCESS) {
        std::fprintf(stderr, "[NVDEC] cuvidCreateDecoder failed: %d\n", res);
        return 0;
    }

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

int NvdecDecoder::on_display(CUVIDPARSERDISPINFO* disp_info) {
    ZoneScopedN("NvdecDecoder::on_display");
    if (!decoder_ || !disp_info || !on_decoded) return 1;

    CUVIDPROCPARAMS proc{};
    proc.progressive_frame = disp_info->progressive_frame;
    proc.top_field_first = disp_info->top_field_first;

    unsigned long long dev_ptr = 0;
    unsigned int pitch = 0;

    CUresult res = cuvid_.cuvidMapVideoFrame64(
        decoder_, disp_info->picture_index, &dev_ptr, &pitch, &proc);
    if (res != CUDA_SUCCESS) {
        std::fprintf(stderr, "[NVDEC] cuvidMapVideoFrame64 failed: %d\n", res);
        return 0;
    }

    size_t host_size = static_cast<size_t>(width_) * height_ * 3 / 2;

    {
        ZoneScopedN("nvdec::gpu_to_host");

        if (pinned_nv12_ && pinned_nv12_size_ >= host_size) {
            CUDA_MEMCPY2D copy{};

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

    uint32_t y_size = width_ * height_;

    DecodedFrame frame{};
    frame.y_plane = pinned_nv12_;
    frame.u_plane = pinned_nv12_ + y_size;
    frame.v_plane = nullptr;
    frame.y_stride = width_;
    frame.uv_stride = width_;
    frame.width = width_;
    frame.height = height_;
    frame.timestamp = disp_info->timestamp;
    frame.nv12 = true;

    on_decoded(frame);
    return 1;
}

} // namespace parties::encdec::nvidia
