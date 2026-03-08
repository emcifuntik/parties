// Dynamic loader for NVIDIA Video Codec SDK DLLs.
// Loads nvcuda.dll, nvEncodeAPI64.dll, nvcuvid.dll at runtime.
#pragma once

#include "cuda_drvapi.h"  // CUDA type shim (defines __cuda_cuda_h__)
#include "cuviddec.h"     // Official NVIDIA NVDEC types
#include "nvcuvid.h"      // Official NVIDIA CUVID parser types
#include "nvEncodeAPI.h"  // Official NVIDIA NVENC types

namespace parties::client::nvidia {

// CUDA driver API function pointers (loaded from nvcuda.dll)
struct CudaApi {
    CUresult (CUDAAPI *cuInit)(unsigned int flags);
    CUresult (CUDAAPI *cuDeviceGet)(CUdevice*, int);
    CUresult (CUDAAPI *cuDeviceGetCount)(int*);
    CUresult (CUDAAPI *cuCtxCreate)(CUcontext*, unsigned int, CUdevice);
    CUresult (CUDAAPI *cuCtxDestroy)(CUcontext);
    CUresult (CUDAAPI *cuCtxPushCurrent)(CUcontext);
    CUresult (CUDAAPI *cuCtxPopCurrent)(CUcontext*);
    CUresult (CUDAAPI *cuMemcpy2D)(const CUDA_MEMCPY2D*);
    CUresult (CUDAAPI *cuMemAllocHost)(void**, size_t);
    CUresult (CUDAAPI *cuMemFreeHost)(void*);
};

// CUVID (NVDEC) function pointers (loaded from nvcuvid.dll)
struct CuvidApi {
    CUresult (CUDAAPI *cuvidGetDecoderCaps)(CUVIDDECODECAPS*);
    CUresult (CUDAAPI *cuvidCreateDecoder)(CUvideodecoder*, CUVIDDECODECREATEINFO*);
    CUresult (CUDAAPI *cuvidDestroyDecoder)(CUvideodecoder);
    CUresult (CUDAAPI *cuvidDecodePicture)(CUvideodecoder, CUVIDPICPARAMS*);
    CUresult (CUDAAPI *cuvidMapVideoFrame64)(CUvideodecoder, int,
        unsigned long long*, unsigned int*, CUVIDPROCPARAMS*);
    CUresult (CUDAAPI *cuvidUnmapVideoFrame64)(CUvideodecoder, unsigned long long);
    CUresult (CUDAAPI *cuvidCreateVideoParser)(CUvideoparser*, CUVIDPARSERPARAMS*);
    CUresult (CUDAAPI *cuvidDestroyVideoParser)(CUvideoparser);
    CUresult (CUDAAPI *cuvidParseVideoData)(CUvideoparser, CUVIDSOURCEDATAPACKET*);
};

// Check if NVENC is available (nvEncodeAPI64.dll loads and entry point works)
bool load_nvenc(NV_ENCODE_API_FUNCTION_LIST& funcs);

// Check if CUDA + CUVID (NVDEC) are available
bool load_cuda(CudaApi& api);
bool load_cuvid(CuvidApi& api);

} // namespace parties::client::nvidia
