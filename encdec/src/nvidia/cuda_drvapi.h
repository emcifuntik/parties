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
typedef void* CUmodule;
typedef void* CUfunction;
typedef void* CUmipmappedArray;
typedef unsigned long long CUsurfObject;
typedef void* CUexternalMemory;
typedef void* CUexternalSemaphore;

#define CUDA_SUCCESS 0
#define CU_CTX_SCHED_AUTO 0

// Memory types for CUDA_MEMCPY2D
typedef enum CUmemorytype_enum {
    CU_MEMORYTYPE_HOST    = 0x01,
    CU_MEMORYTYPE_DEVICE  = 0x02,
    CU_MEMORYTYPE_ARRAY   = 0x03,
    CU_MEMORYTYPE_UNIFIED = 0x04,
} CUmemorytype;

// Opaque array handle.
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

// Subset of the CUDA external-resource ABI used by the D3D12 interop path.
// Layout and values match cuda.h from CUDA 12.9; keeping the declarations here
// preserves the existing runtime-loaded design and does not require users to
// install the CUDA Toolkit to build or run Parties.
typedef enum CUarray_format_enum {
    CU_AD_FORMAT_UNSIGNED_INT8 = 0x01,
    // Video Codec SDK 13.1 opaque NVDEC/NVENC array formats. These values
    // are part of the CUDA driver ABI (CUDA 13.1) and are intentionally
    // declared here so the runtime-loaded backend still has no CUDA Toolkit
    // build dependency.
    CU_AD_FORMAT_P016 = 0xa1,
    CU_AD_FORMAT_NV12 = 0xb0,
} CUarray_format;

typedef enum CUresourcetype_enum {
    CU_RESOURCE_TYPE_ARRAY = 0x00,
} CUresourcetype;

#define CUDA_ARRAY3D_SURFACE_LDST 0x02
#define CUDA_ARRAY3D_VIDEO_ENCODE_DECODE 0x100
#define CUDA_EXTERNAL_MEMORY_DEDICATED 0x01
#define CU_STREAM_DEFAULT 0x00

typedef struct CUDA_ARRAY3D_DESCRIPTOR_st {
    size_t Width;
    size_t Height;
    size_t Depth;
    CUarray_format Format;
    unsigned int NumChannels;
    unsigned int Flags;
} CUDA_ARRAY3D_DESCRIPTOR;

typedef enum CUexternalMemoryHandleType_enum {
    CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE = 5,
} CUexternalMemoryHandleType;

typedef struct CUDA_EXTERNAL_MEMORY_HANDLE_DESC_st {
    CUexternalMemoryHandleType type;
    union {
        int fd;
        struct {
            void* handle;
            const void* name;
        } win32;
        const void* nvSciBufObject;
    } handle;
    unsigned long long size;
    unsigned int flags;
    unsigned int reserved[16];
} CUDA_EXTERNAL_MEMORY_HANDLE_DESC;

typedef struct CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC_st {
    unsigned long long offset;
    CUDA_ARRAY3D_DESCRIPTOR arrayDesc;
    unsigned int numLevels;
    unsigned int reserved[16];
} CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC;

typedef enum CUexternalSemaphoreHandleType_enum {
    CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE = 4,
} CUexternalSemaphoreHandleType;

typedef struct CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC_st {
    CUexternalSemaphoreHandleType type;
    union {
        int fd;
        struct {
            void* handle;
            const void* name;
        } win32;
        const void* nvSciSyncObj;
    } handle;
    unsigned int flags;
    unsigned int reserved[16];
} CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC;

typedef struct CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS_st {
    struct {
        struct { unsigned long long value; } fence;
        union { void* fence; unsigned long long reserved; } nvSciSync;
        struct { unsigned long long key; } keyedMutex;
        unsigned int reserved[12];
    } params;
    unsigned int flags;
    unsigned int reserved[16];
} CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS;

typedef struct CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS_st {
    struct {
        struct { unsigned long long value; } fence;
        union { void* fence; unsigned long long reserved; } nvSciSync;
        struct {
            unsigned long long key;
            unsigned int timeoutMs;
        } keyedMutex;
        unsigned int reserved[10];
    } params;
    unsigned int flags;
    unsigned int reserved[16];
} CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS;

typedef struct CUDA_RESOURCE_DESC_st {
    CUresourcetype resType;
    union {
        struct { CUarray hArray; } array;
        struct { CUmipmappedArray hMipmappedArray; } mipmap;
        struct {
            CUdeviceptr devPtr;
            CUarray_format format;
            unsigned int numChannels;
            size_t sizeInBytes;
        } linear;
        struct {
            CUdeviceptr devPtr;
            CUarray_format format;
            unsigned int numChannels;
            size_t width;
            size_t height;
            size_t pitchInBytes;
        } pitch2D;
        struct { int reserved[32]; } reserved;
    } res;
    unsigned int flags;
} CUDA_RESOURCE_DESC;
