#pragma once

#include "cuda_drvapi.h"
#include "cuviddec.h"
#include "nvcuvid.h"
#include "nvEncodeAPI.h"

namespace parties::encdec::nvidia {

struct CudaApi {
    CUresult (CUDAAPI *cuInit)(unsigned int flags);
    CUresult (CUDAAPI *cuDeviceGet)(CUdevice*, int);
    CUresult (CUDAAPI *cuDeviceGetCount)(int*);
    CUresult (CUDAAPI *cuDeviceGetLuid)(char*, unsigned int*, CUdevice);
    CUresult (CUDAAPI *cuCtxCreate)(CUcontext*, unsigned int, CUdevice);
    CUresult (CUDAAPI *cuCtxDestroy)(CUcontext);
    CUresult (CUDAAPI *cuCtxPushCurrent)(CUcontext);
    CUresult (CUDAAPI *cuCtxPopCurrent)(CUcontext*);
    CUresult (CUDAAPI *cuMemcpy2D)(const CUDA_MEMCPY2D*);
    CUresult (CUDAAPI *cuMemcpy2DAsync)(const CUDA_MEMCPY2D*, CUstream);
    CUresult (CUDAAPI *cuMemAllocHost)(void**, size_t);
    CUresult (CUDAAPI *cuMemFreeHost)(void*);
    CUresult (CUDAAPI *cuStreamCreate)(CUstream*, unsigned int);
    CUresult (CUDAAPI *cuStreamDestroy)(CUstream);
    CUresult (CUDAAPI *cuStreamSynchronize)(CUstream);
    CUresult (CUDAAPI *cuImportExternalMemory)(CUexternalMemory*, const CUDA_EXTERNAL_MEMORY_HANDLE_DESC*);
    CUresult (CUDAAPI *cuDestroyExternalMemory)(CUexternalMemory);
    CUresult (CUDAAPI *cuExternalMemoryGetMappedMipmappedArray)(
        CUmipmappedArray*, CUexternalMemory, const CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC*);
    CUresult (CUDAAPI *cuArray3DCreate)(CUarray*, const CUDA_ARRAY3D_DESCRIPTOR*);
    CUresult (CUDAAPI *cuArrayDestroy)(CUarray);
    CUresult (CUDAAPI *cuMipmappedArrayGetLevel)(CUarray*, CUmipmappedArray, unsigned int);
    CUresult (CUDAAPI *cuArrayGetPlane)(CUarray*, CUarray, unsigned int);
    CUresult (CUDAAPI *cuMipmappedArrayDestroy)(CUmipmappedArray);
    CUresult (CUDAAPI *cuSurfObjectCreate)(CUsurfObject*, const CUDA_RESOURCE_DESC*);
    CUresult (CUDAAPI *cuSurfObjectDestroy)(CUsurfObject);
    CUresult (CUDAAPI *cuImportExternalSemaphore)(
        CUexternalSemaphore*, const CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC*);
    CUresult (CUDAAPI *cuDestroyExternalSemaphore)(CUexternalSemaphore);
    CUresult (CUDAAPI *cuSignalExternalSemaphoresAsync)(
        const CUexternalSemaphore*, const CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS*, unsigned int, CUstream);
    CUresult (CUDAAPI *cuWaitExternalSemaphoresAsync)(
        const CUexternalSemaphore*, const CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS*, unsigned int, CUstream);
    CUresult (CUDAAPI *cuModuleLoadData)(CUmodule*, const void*);
    CUresult (CUDAAPI *cuModuleUnload)(CUmodule);
    CUresult (CUDAAPI *cuModuleGetFunction)(CUfunction*, CUmodule, const char*);
    CUresult (CUDAAPI *cuLaunchKernel)(CUfunction,
        unsigned int, unsigned int, unsigned int,
        unsigned int, unsigned int, unsigned int,
        unsigned int, CUstream, void**, void**);
    CUresult (CUDAAPI *cuGetErrorName)(CUresult, const char**);
    CUresult (CUDAAPI *cuGetErrorString)(CUresult, const char**);
};

struct CuvidApi {
    CUresult (CUDAAPI *cuvidGetDecoderCaps)(CUVIDDECODECAPS*);
    CUresult (CUDAAPI *cuvidCreateDecoder)(CUvideodecoder*, CUVIDDECODECREATEINFO*);
    CUresult (CUDAAPI *cuvidDestroyDecoder)(CUvideodecoder);
    CUresult (CUDAAPI *cuvidRegisterDecodeSurfaces)(
        CUvideodecoder, CUVIDREGISTERDECODESURFACESINFO*);
    CUresult (CUDAAPI *cuvidDecodePictureAsync)(
        CUvideodecoder, CUVIDPICPARAMS*, CUstream);
    CUresult (CUDAAPI *cuvidDecodePicture)(CUvideodecoder, CUVIDPICPARAMS*);
    CUresult (CUDAAPI *cuvidMapVideoFrame64)(CUvideodecoder, int,
        unsigned long long*, unsigned int*, CUVIDPROCPARAMS*);
    CUresult (CUDAAPI *cuvidUnmapVideoFrame64)(CUvideodecoder, unsigned long long);
    CUresult (CUDAAPI *cuvidCreateVideoParser)(CUvideoparser*, CUVIDPARSERPARAMS*);
    CUresult (CUDAAPI *cuvidDestroyVideoParser)(CUvideoparser);
    CUresult (CUDAAPI *cuvidParseVideoData)(CUvideoparser, CUVIDSOURCEDATAPACKET*);
};

bool load_nvenc(NV_ENCODE_API_FUNCTION_LIST& funcs);

bool load_cuda(CudaApi& api);
bool load_cuvid(CuvidApi& api);

}
