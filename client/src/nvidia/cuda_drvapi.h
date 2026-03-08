// Minimal CUDA Driver API shim for NVENC/NVDEC dynamic loading.
// Provides just enough CUDA types so that cuviddec.h / nvcuvid.h can be
// included without pulling in the full cuda.h (we load nvcuda.dll at runtime).
#pragma once

#include <cstdint>
#include <cstddef>

// Prevent cuviddec.h from trying to include <cuda.h>
#define __cuda_cuda_h__

// CUDA version — must be >= 3020 for 64-bit devptr support in cuviddec.h
#define CUDA_VERSION 12090

// Calling convention for CUDA driver API functions
#ifdef _WIN32
#define CUDAAPI __stdcall
#else
#define CUDAAPI
#endif

// Core CUDA types used by cuviddec.h / nvcuvid.h
typedef int CUresult;
typedef int CUdevice;
typedef void* CUcontext;
typedef void* CUstream;
typedef unsigned long long CUdeviceptr;

#define CUDA_SUCCESS 0
#define CU_CTX_SCHED_AUTO 0

// Memory types for CUDA_MEMCPY2D
typedef enum CUmemorytype_enum {
    CU_MEMORYTYPE_HOST    = 0x01,
    CU_MEMORYTYPE_DEVICE  = 0x02,
    CU_MEMORYTYPE_ARRAY   = 0x03,
    CU_MEMORYTYPE_UNIFIED = 0x04,
} CUmemorytype;

// Opaque array handle (unused by us, but referenced in CUDA_MEMCPY2D)
typedef void* CUarray;

// 2D memory copy descriptor — must match cuda.h layout exactly
typedef struct {
    size_t srcXInBytes;
    size_t srcY;
    CUmemorytype srcMemoryType;
    const void* srcHost;
    CUdeviceptr srcDevice;
    CUarray srcArray;
    size_t srcPitch;

    size_t dstXInBytes;
    size_t dstY;
    CUmemorytype dstMemoryType;
    void* dstHost;
    CUdeviceptr dstDevice;
    CUarray dstArray;
    size_t dstPitch;

    size_t WidthInBytes;
    size_t Height;
} CUDA_MEMCPY2D;
