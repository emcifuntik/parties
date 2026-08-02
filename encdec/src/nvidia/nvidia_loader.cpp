// Dynamic loader for NVIDIA Video Codec SDK DLLs.
#include "nvidia_loader.h"

#include <cstring>
#include <parties/log.h>
#include <windows.h>

namespace parties::encdec::nvidia {

template<typename T>
static bool load_sym(HMODULE mod, const char* name, T& out) {
    out = reinterpret_cast<T>(GetProcAddress(mod, name));
    return out != nullptr;
}

bool load_nvenc(NV_ENCODE_API_FUNCTION_LIST& funcs) {
    static HMODULE mod = nullptr;
    static bool tried = false;
    static bool ok = false;

    if (tried) {
        if (!ok) return false;
        // Re-fill function list from cached module
    } else {
        tried = true;
        mod = LoadLibraryA("nvEncodeAPI64.dll");
        if (!mod) {
            LOG_ERROR("nvEncodeAPI64.dll not found");
            return false;
        }
    }

    using PFN_NvEncodeAPICreateInstance = NVENCSTATUS (NVENCAPI *)(NV_ENCODE_API_FUNCTION_LIST*);
    PFN_NvEncodeAPICreateInstance createInstance = nullptr;
    if (!load_sym(mod, "NvEncodeAPICreateInstance", createInstance)) {
        LOG_ERROR("NvEncodeAPICreateInstance not found");
        return false;
    }

    std::memset(&funcs, 0, sizeof(funcs));
    funcs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS status = createInstance(&funcs);
    if (status != NV_ENC_SUCCESS) {
        LOG_ERROR("NvEncodeAPICreateInstance failed: {}", (int)status);
        return false;
    }

    ok = true;
    return true;
}

bool load_cuda(CudaApi& api) {
    static HMODULE mod = nullptr;
    static bool tried = false;
    static bool ok = false;

    if (tried) {
        if (!ok) return false;
    } else {
        tried = true;
        mod = LoadLibraryA("nvcuda.dll");
        if (!mod) {
            LOG_ERROR("nvcuda.dll not found");
            return false;
        }
    }

    bool all_ok = true;
    all_ok &= load_sym(mod, "cuInit", api.cuInit);
    all_ok &= load_sym(mod, "cuDeviceGet", api.cuDeviceGet);
    all_ok &= load_sym(mod, "cuDeviceGetCount", api.cuDeviceGetCount);
    all_ok &= load_sym(mod, "cuDeviceGetLuid", api.cuDeviceGetLuid);
    all_ok &= load_sym(mod, "cuCtxCreate_v2", api.cuCtxCreate);
    all_ok &= load_sym(mod, "cuCtxDestroy_v2", api.cuCtxDestroy);
    all_ok &= load_sym(mod, "cuCtxPushCurrent_v2", api.cuCtxPushCurrent);
    all_ok &= load_sym(mod, "cuCtxPopCurrent_v2", api.cuCtxPopCurrent);
    all_ok &= load_sym(mod, "cuMemcpy2D_v2", api.cuMemcpy2D);
    all_ok &= load_sym(mod, "cuMemcpy2DAsync_v2", api.cuMemcpy2DAsync);
    all_ok &= load_sym(mod, "cuMemAllocHost_v2", api.cuMemAllocHost);
    all_ok &= load_sym(mod, "cuMemFreeHost", api.cuMemFreeHost);
    all_ok &= load_sym(mod, "cuStreamCreate", api.cuStreamCreate);
    all_ok &= load_sym(mod, "cuStreamDestroy_v2", api.cuStreamDestroy);
    all_ok &= load_sym(mod, "cuStreamSynchronize", api.cuStreamSynchronize);
    all_ok &= load_sym(mod, "cuImportExternalMemory", api.cuImportExternalMemory);
    all_ok &= load_sym(mod, "cuDestroyExternalMemory", api.cuDestroyExternalMemory);
    all_ok &= load_sym(mod, "cuExternalMemoryGetMappedMipmappedArray", api.cuExternalMemoryGetMappedMipmappedArray);
    all_ok &= load_sym(mod, "cuArray3DCreate_v2", api.cuArray3DCreate);
    all_ok &= load_sym(mod, "cuArrayDestroy", api.cuArrayDestroy);
    all_ok &= load_sym(mod, "cuMipmappedArrayGetLevel", api.cuMipmappedArrayGetLevel);
    all_ok &= load_sym(mod, "cuArrayGetPlane", api.cuArrayGetPlane);
    all_ok &= load_sym(mod, "cuMipmappedArrayDestroy", api.cuMipmappedArrayDestroy);
    all_ok &= load_sym(mod, "cuSurfObjectCreate", api.cuSurfObjectCreate);
    all_ok &= load_sym(mod, "cuSurfObjectDestroy", api.cuSurfObjectDestroy);
    all_ok &= load_sym(mod, "cuImportExternalSemaphore", api.cuImportExternalSemaphore);
    all_ok &= load_sym(mod, "cuDestroyExternalSemaphore", api.cuDestroyExternalSemaphore);
    all_ok &= load_sym(mod, "cuSignalExternalSemaphoresAsync", api.cuSignalExternalSemaphoresAsync);
    all_ok &= load_sym(mod, "cuWaitExternalSemaphoresAsync", api.cuWaitExternalSemaphoresAsync);
    all_ok &= load_sym(mod, "cuModuleLoadData", api.cuModuleLoadData);
    all_ok &= load_sym(mod, "cuModuleUnload", api.cuModuleUnload);
    all_ok &= load_sym(mod, "cuModuleGetFunction", api.cuModuleGetFunction);
    all_ok &= load_sym(mod, "cuLaunchKernel", api.cuLaunchKernel);
    all_ok &= load_sym(mod, "cuGetErrorName", api.cuGetErrorName);
    all_ok &= load_sym(mod, "cuGetErrorString", api.cuGetErrorString);

    if (!all_ok) {
        LOG_ERROR("Failed to load some CUDA functions");
        return false;
    }

    CUresult res = api.cuInit(0);
    if (res != CUDA_SUCCESS) {
        LOG_ERROR("cuInit failed: {}", res);
        return false;
    }

    ok = true;
    return true;
}

bool load_cuvid(CuvidApi& api) {
    static HMODULE mod = nullptr;
    static bool tried = false;
    static bool ok = false;

    if (tried) {
        if (!ok) return false;
    } else {
        tried = true;
        mod = LoadLibraryA("nvcuvid.dll");
        if (!mod) {
            LOG_ERROR("nvcuvid.dll not found");
            return false;
        }
    }

    bool all_ok = true;
    all_ok &= load_sym(mod, "cuvidGetDecoderCaps", api.cuvidGetDecoderCaps);
    all_ok &= load_sym(mod, "cuvidCreateDecoder", api.cuvidCreateDecoder);
    all_ok &= load_sym(mod, "cuvidDestroyDecoder", api.cuvidDestroyDecoder);
    // SDK 13.1 opaque-output functions are optional by design. Their absence
    // is a capability result, not a loader failure.
    load_sym(mod, "cuvidRegisterDecodeSurfaces", api.cuvidRegisterDecodeSurfaces);
    load_sym(mod, "cuvidDecodePictureAsync", api.cuvidDecodePictureAsync);
    all_ok &= load_sym(mod, "cuvidDecodePicture", api.cuvidDecodePicture);
    all_ok &= load_sym(mod, "cuvidMapVideoFrame64", api.cuvidMapVideoFrame64);
    all_ok &= load_sym(mod, "cuvidUnmapVideoFrame64", api.cuvidUnmapVideoFrame64);
    all_ok &= load_sym(mod, "cuvidCreateVideoParser", api.cuvidCreateVideoParser);
    all_ok &= load_sym(mod, "cuvidDestroyVideoParser", api.cuvidDestroyVideoParser);
    all_ok &= load_sym(mod, "cuvidParseVideoData", api.cuvidParseVideoData);

    if (!all_ok) {
        LOG_ERROR("Failed to load some CUVID functions");
        return false;
    }

    ok = true;
    return true;
}

} // namespace parties::encdec::nvidia
