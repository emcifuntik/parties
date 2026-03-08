// Dynamic loader for NVIDIA Video Codec SDK DLLs.
#include "nvidia_loader.h"

#include <cstdio>
#include <cstring>
#include <windows.h>

namespace parties::client::nvidia {

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
            std::fprintf(stderr, "[NVENC] nvEncodeAPI64.dll not found\n");
            return false;
        }
    }

    using PFN_NvEncodeAPICreateInstance = NVENCSTATUS (NVENCAPI *)(NV_ENCODE_API_FUNCTION_LIST*);
    PFN_NvEncodeAPICreateInstance createInstance = nullptr;
    if (!load_sym(mod, "NvEncodeAPICreateInstance", createInstance)) {
        std::fprintf(stderr, "[NVENC] NvEncodeAPICreateInstance not found\n");
        return false;
    }

    std::memset(&funcs, 0, sizeof(funcs));
    funcs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS status = createInstance(&funcs);
    if (status != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "[NVENC] NvEncodeAPICreateInstance failed: %d\n", status);
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
            std::fprintf(stderr, "[CUDA] nvcuda.dll not found\n");
            return false;
        }
    }

    bool all_ok = true;
    all_ok &= load_sym(mod, "cuInit", api.cuInit);
    all_ok &= load_sym(mod, "cuDeviceGet", api.cuDeviceGet);
    all_ok &= load_sym(mod, "cuDeviceGetCount", api.cuDeviceGetCount);
    all_ok &= load_sym(mod, "cuCtxCreate_v2", api.cuCtxCreate);
    all_ok &= load_sym(mod, "cuCtxDestroy_v2", api.cuCtxDestroy);
    all_ok &= load_sym(mod, "cuCtxPushCurrent_v2", api.cuCtxPushCurrent);
    all_ok &= load_sym(mod, "cuCtxPopCurrent_v2", api.cuCtxPopCurrent);
    all_ok &= load_sym(mod, "cuMemcpy2D_v2", api.cuMemcpy2D);
    all_ok &= load_sym(mod, "cuMemAllocHost_v2", api.cuMemAllocHost);
    all_ok &= load_sym(mod, "cuMemFreeHost", api.cuMemFreeHost);

    if (!all_ok) {
        std::fprintf(stderr, "[CUDA] Failed to load some CUDA functions\n");
        return false;
    }

    CUresult res = api.cuInit(0);
    if (res != CUDA_SUCCESS) {
        std::fprintf(stderr, "[CUDA] cuInit failed: %d\n", res);
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
            std::fprintf(stderr, "[CUVID] nvcuvid.dll not found\n");
            return false;
        }
    }

    bool all_ok = true;
    all_ok &= load_sym(mod, "cuvidGetDecoderCaps", api.cuvidGetDecoderCaps);
    all_ok &= load_sym(mod, "cuvidCreateDecoder", api.cuvidCreateDecoder);
    all_ok &= load_sym(mod, "cuvidDestroyDecoder", api.cuvidDestroyDecoder);
    all_ok &= load_sym(mod, "cuvidDecodePicture", api.cuvidDecodePicture);
    all_ok &= load_sym(mod, "cuvidMapVideoFrame64", api.cuvidMapVideoFrame64);
    all_ok &= load_sym(mod, "cuvidUnmapVideoFrame64", api.cuvidUnmapVideoFrame64);
    all_ok &= load_sym(mod, "cuvidCreateVideoParser", api.cuvidCreateVideoParser);
    all_ok &= load_sym(mod, "cuvidDestroyVideoParser", api.cuvidDestroyVideoParser);
    all_ok &= load_sym(mod, "cuvidParseVideoData", api.cuvidParseVideoData);

    if (!all_ok) {
        std::fprintf(stderr, "[CUVID] Failed to load some CUVID functions\n");
        return false;
    }

    ok = true;
    return true;
}

} // namespace parties::client::nvidia
