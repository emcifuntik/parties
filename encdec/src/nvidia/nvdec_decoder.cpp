#include "nvdec_decoder.h"
#include "nv12_to_rgba_ptx.h"
#include "nvdec_sequence_policy.h"
#include "nvdec_surface_wait.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
#include <parties/log.h>
#include <parties/profiler.h>

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

namespace parties::encdec::nvidia {

using Microsoft::WRL::ComPtr;

namespace {
constexpr size_t kFallbackSurfaceCount = 4;
constexpr uint32_t kOpaqueSurfaceHeadroom = 16;
constexpr uint64_t kSlowPacketThresholdUs = 20'000;

using DiagnosticClock = std::chrono::steady_clock;

uint64_t elapsed_us(DiagnosticClock::time_point start) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        DiagnosticClock::now() - start).count());
}

double milliseconds(uint64_t microseconds) {
    return static_cast<double>(microseconds) / 1000.0;
}

class AccumulatedTimer {
public:
    AccumulatedTimer(bool enabled, uint64_t* destination)
        : enabled_(enabled), destination_(destination), start_(DiagnosticClock::now()) {}

    ~AccumulatedTimer() {
        if (enabled_ && destination_)
            *destination_ += elapsed_us(start_);
    }

private:
    bool enabled_ = false;
    uint64_t* destination_ = nullptr;
    DiagnosticClock::time_point start_;
};

enum class InteropSurfaceMode {
    None,
    RgbaFallback,
    OpaqueNV12,
};

const char* interop_surface_mode_name(InteropSurfaceMode mode) {
    switch (mode) {
    case InteropSurfaceMode::RgbaFallback: return "rgba-fallback";
    case InteropSurfaceMode::OpaqueNV12: return "opaque-nv12";
    default: return "none";
    }
}

const char* cuda_error(const CudaApi& api, CUresult result) {
    const char* text = nullptr;
    if (api.cuGetErrorName && api.cuGetErrorName(result, &text) == CUDA_SUCCESS && text)
        return text;
    return "CUDA_ERROR_UNKNOWN";
}
} // namespace

struct NvdecCudaInteropState : std::enable_shared_from_this<NvdecCudaInteropState> {
    struct Slot {
        // The opaque NVDEC backing is a native CUDA array. In particular, it is
        // not a D3D12 NV12 texture imported into CUDA: the opaque decoder layout
        // is driver-private and is not required to match any DXGI plane layout.
        ComPtr<ID3D12Resource> resource;
        ComPtr<ID3D12Resource> luma_resource;
        ComPtr<ID3D12Resource> chroma_resource;
        CUexternalMemory external_memory = nullptr;
        CUmipmappedArray mipmapped_array = nullptr;
        CUarray array = nullptr;
        bool owns_cuda_array = false;
        CUarray decoded_luma_array = nullptr;
        CUarray decoded_chroma_array = nullptr;
        CUexternalMemory luma_external_memory = nullptr;
        CUmipmappedArray luma_mipmapped_array = nullptr;
        CUarray luma_array = nullptr;
        CUexternalMemory chroma_external_memory = nullptr;
        CUmipmappedArray chroma_mipmapped_array = nullptr;
        CUarray chroma_array = nullptr;
        CUsurfObject surface = 0;
        std::atomic<bool> in_use{false};
    };

    struct Lease {
        std::shared_ptr<NvdecCudaInteropState> state;
        Slot* slot = nullptr;
        Lease(std::shared_ptr<NvdecCudaInteropState> state_value, Slot* slot_value)
            : state(std::move(state_value)), slot(slot_value) {}
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        ~Lease() {
            if (slot) state->release_slot(slot);
        }
    };

    CudaApi api{};
    CUcontext context = nullptr;
    CUstream stream = nullptr;
    CUmodule module = nullptr;
    CUfunction kernel = nullptr;
    ComPtr<ID3D12Device> d3d12_device;
    ComPtr<ID3D12Fence> ready_fence;
    CUexternalSemaphore ready_semaphore = nullptr;
    std::vector<std::unique_ptr<Slot>> slots;
    std::mutex slot_mutex;
    std::condition_variable_any slot_released;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t next_fence_value = 0;
    InteropSurfaceMode surface_mode = InteropSurfaceMode::None;
    bool owns_context = false;

    ~NvdecCudaInteropState() {
        if (!context) return;
        if (api.cuCtxPushCurrent(context) == CUDA_SUCCESS) {
            if (stream) api.cuStreamSynchronize(stream);
            destroy_slots();
            if (ready_semaphore) api.cuDestroyExternalSemaphore(ready_semaphore);
            if (module) api.cuModuleUnload(module);
            if (stream) api.cuStreamDestroy(stream);
            CUcontext previous = nullptr;
            api.cuCtxPopCurrent(&previous);
        }
        ready_semaphore = nullptr;
        module = nullptr;
        stream = nullptr;
        ready_fence.Reset();
        d3d12_device.Reset();
        if (owns_context) api.cuCtxDestroy(context);
        context = nullptr;
    }

    void destroy_slots() {
        for (auto& slot : slots) {
            if (!slot) continue;
            if (slot->surface) api.cuSurfObjectDestroy(slot->surface);
            if (slot->luma_mipmapped_array)
                api.cuMipmappedArrayDestroy(slot->luma_mipmapped_array);
            if (slot->luma_external_memory)
                api.cuDestroyExternalMemory(slot->luma_external_memory);
            if (slot->chroma_mipmapped_array)
                api.cuMipmappedArrayDestroy(slot->chroma_mipmapped_array);
            if (slot->chroma_external_memory)
                api.cuDestroyExternalMemory(slot->chroma_external_memory);
            if (slot->owns_cuda_array && slot->array)
                api.cuArrayDestroy(slot->array);
            else if (slot->mipmapped_array)
                api.cuMipmappedArrayDestroy(slot->mipmapped_array);
            if (slot->external_memory) api.cuDestroyExternalMemory(slot->external_memory);
            slot.reset();
        }
        slots.clear();
        width = 0;
        height = 0;
        surface_mode = InteropSurfaceMode::None;
    }

    bool initialize(ID3D12Device* device) {
        if (!device || !context) return false;
        d3d12_device = device;

        CUresult result = api.cuStreamCreate(&stream, CU_STREAM_DEFAULT);
        if (result != CUDA_SUCCESS) {
            LOG_WARN("CUDA/D3D12 interop: cuStreamCreate failed: {} ({})",
                     static_cast<int>(result), cuda_error(api, result));
            return false;
        }
        HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                                         IID_PPV_ARGS(&ready_fence));
        if (FAILED(hr)) {
            LOG_WARN("CUDA/D3D12 interop: CreateFence failed: 0x{:08x}",
                     static_cast<unsigned int>(hr));
            return false;
        }
        HANDLE shared_fence = nullptr;
        hr = device->CreateSharedHandle(ready_fence.Get(), nullptr, GENERIC_ALL,
                                       nullptr, &shared_fence);
        if (FAILED(hr)) return false;

        CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC semaphore_desc{};
        semaphore_desc.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE;
        semaphore_desc.handle.win32.handle = shared_fence;
        result = api.cuImportExternalSemaphore(&ready_semaphore, &semaphore_desc);
        CloseHandle(shared_fence);
        if (result != CUDA_SUCCESS) {
            LOG_WARN("CUDA/D3D12 interop: fence import failed: {} ({})",
                     static_cast<int>(result), cuda_error(api, result));
            return false;
        }
        return true;
    }

    bool ensure_conversion_kernel() {
        if (kernel) return true;
        CUresult result = api.cuModuleLoadData(&module, kNv12ToRgbaPtx);
        if (result == CUDA_SUCCESS)
            result = api.cuModuleGetFunction(&kernel, module, "parties_nv12_to_rgba");
        if (result == CUDA_SUCCESS) return true;
        LOG_WARN("CUDA/D3D12 interop: PTX load failed: {} ({})",
                 static_cast<int>(result), cuda_error(api, result));
        if (module) api.cuModuleUnload(module);
        module = nullptr;
        kernel = nullptr;
        return false;
    }

    bool create_slots(uint32_t new_width, uint32_t new_height, size_t count,
                      InteropSurfaceMode requested_mode) {
        if (count == 0 || requested_mode == InteropSurfaceMode::None) return false;
        if (width == new_width && height == new_height && slots.size() == count &&
            surface_mode == requested_mode && !slots.empty())
            return true;
        const auto create_start = DiagnosticClock::now();
        uint64_t destroy_us = 0;
        for (const auto& slot : slots) {
            if (slot && slot->in_use.load(std::memory_order_acquire)) {
                LOG_WARN("CUDA/D3D12 interop: delaying resize while a render surface is in flight");
                return false;
            }
        }
        const auto destroy_start = DiagnosticClock::now();
        destroy_slots();
        destroy_us = elapsed_us(destroy_start);

        if (requested_mode == InteropSurfaceMode::RgbaFallback &&
            !ensure_conversion_kernel())
            return false;

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;

        const bool opaque_nv12 = requested_mode == InteropSurfaceMode::OpaqueNV12;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = new_width;
        desc.Height = new_height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        if (!opaque_nv12)
            desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        slots.resize(count);
        for (auto& destination : slots) {
            auto slot = std::make_unique<Slot>();
            CUresult result = CUDA_SUCCESS;
            if (opaque_nv12) {
                CUDA_ARRAY3D_DESCRIPTOR decode_desc{};
                decode_desc.Width = new_width;
                decode_desc.Height = new_height;
                decode_desc.Depth = 0;
                decode_desc.Format = CU_AD_FORMAT_NV12;
                decode_desc.NumChannels = 3;
                decode_desc.Flags = CUDA_ARRAY3D_SURFACE_LDST |
                                    CUDA_ARRAY3D_VIDEO_ENCODE_DECODE;
                result = api.cuArray3DCreate(&slot->array, &decode_desc);
                if (result == CUDA_SUCCESS) {
                    slot->owns_cuda_array = true;
                    result = api.cuArrayGetPlane(
                        &slot->decoded_luma_array, slot->array, 0);
                }
                if (result == CUDA_SUCCESS) {
                    result = api.cuArrayGetPlane(
                        &slot->decoded_chroma_array, slot->array, 1);
                }
            } else {
                const auto allocation =
                    d3d12_device->GetResourceAllocationInfo(0, 1, &desc);
                HRESULT hr = d3d12_device->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_SHARED, &desc,
                    D3D12_RESOURCE_STATE_COMMON, nullptr,
                    IID_PPV_ARGS(&slot->resource));
                if (FAILED(hr)) {
                    LOG_WARN("CUDA/D3D12 interop: output texture creation failed: 0x{:08x}",
                             static_cast<unsigned int>(hr));
                    destroy_slots();
                    return false;
                }

                HANDLE shared_resource = nullptr;
                hr = d3d12_device->CreateSharedHandle(
                    slot->resource.Get(), nullptr, GENERIC_ALL, nullptr,
                    &shared_resource);
                if (FAILED(hr)) {
                    destroy_slots();
                    return false;
                }

                CUDA_EXTERNAL_MEMORY_HANDLE_DESC memory_desc{};
                memory_desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE;
                memory_desc.handle.win32.handle = shared_resource;
                memory_desc.size = allocation.SizeInBytes;
                memory_desc.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;
                result = api.cuImportExternalMemory(
                    &slot->external_memory, &memory_desc);
                CloseHandle(shared_resource);

                CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC array_desc{};
                array_desc.arrayDesc.Width = new_width;
                array_desc.arrayDesc.Height = new_height;
                array_desc.arrayDesc.Depth = 0;
                array_desc.arrayDesc.Format = CU_AD_FORMAT_UNSIGNED_INT8;
                array_desc.arrayDesc.NumChannels = 4;
                array_desc.arrayDesc.Flags = CUDA_ARRAY3D_SURFACE_LDST;
                array_desc.numLevels = 1;
                if (result == CUDA_SUCCESS) {
                    result = api.cuExternalMemoryGetMappedMipmappedArray(
                        &slot->mipmapped_array, slot->external_memory, &array_desc);
                }
                if (result == CUDA_SUCCESS) {
                    result = api.cuMipmappedArrayGetLevel(
                        &slot->array, slot->mipmapped_array, 0);
                }
                if (result == CUDA_SUCCESS) {
                    CUDA_RESOURCE_DESC surface_desc{};
                    surface_desc.resType = CU_RESOURCE_TYPE_ARRAY;
                    surface_desc.res.array.hArray = slot->array;
                    result = api.cuSurfObjectCreate(&slot->surface, &surface_desc);
                }
            }
            if (result != CUDA_SUCCESS) {
                LOG_WARN("CUDA/D3D12 interop: CUDA surface mapping failed: {} ({})",
                         static_cast<int>(result), cuda_error(api, result));
                destroy_slots();
                return false;
            }

            if (opaque_nv12) {
                auto create_plane = [&](uint32_t plane_width, uint32_t plane_height,
                                        DXGI_FORMAT format, unsigned int channels,
                                        ComPtr<ID3D12Resource>& plane_resource,
                                        CUexternalMemory& plane_external_memory,
                                        CUmipmappedArray& plane_mipmapped_array,
                                        CUarray& plane_array, const char* plane_name) {
                    D3D12_RESOURCE_DESC plane_desc = desc;
                    plane_desc.Width = plane_width;
                    plane_desc.Height = plane_height;
                    plane_desc.Format = format;
                    plane_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
                    const auto plane_allocation =
                        d3d12_device->GetResourceAllocationInfo(0, 1, &plane_desc);

                    HRESULT plane_hr = d3d12_device->CreateCommittedResource(
                        &heap, D3D12_HEAP_FLAG_SHARED, &plane_desc,
                        D3D12_RESOURCE_STATE_COMMON, nullptr,
                        IID_PPV_ARGS(&plane_resource));
                    if (FAILED(plane_hr)) {
                        LOG_WARN("CUDA/D3D12 interop: {} texture creation failed: 0x{:08x}",
                                 plane_name, static_cast<unsigned int>(plane_hr));
                        return false;
                    }

                    HANDLE shared_plane = nullptr;
                    plane_hr = d3d12_device->CreateSharedHandle(
                        plane_resource.Get(), nullptr, GENERIC_ALL, nullptr, &shared_plane);
                    if (FAILED(plane_hr)) return false;

                    CUDA_EXTERNAL_MEMORY_HANDLE_DESC plane_memory_desc{};
                    plane_memory_desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE;
                    plane_memory_desc.handle.win32.handle = shared_plane;
                    plane_memory_desc.size = plane_allocation.SizeInBytes;
                    plane_memory_desc.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;
                    CUresult plane_result = api.cuImportExternalMemory(
                        &plane_external_memory, &plane_memory_desc);
                    CloseHandle(shared_plane);

                    CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC plane_array_desc{};
                    plane_array_desc.arrayDesc.Width = plane_width;
                    plane_array_desc.arrayDesc.Height = plane_height;
                    plane_array_desc.arrayDesc.Depth = 0;
                    plane_array_desc.arrayDesc.Format = CU_AD_FORMAT_UNSIGNED_INT8;
                    plane_array_desc.arrayDesc.NumChannels = channels;
                    plane_array_desc.arrayDesc.Flags = CUDA_ARRAY3D_SURFACE_LDST;
                    plane_array_desc.numLevels = 1;
                    if (plane_result == CUDA_SUCCESS) {
                        plane_result = api.cuExternalMemoryGetMappedMipmappedArray(
                            &plane_mipmapped_array, plane_external_memory,
                            &plane_array_desc);
                    }
                    if (plane_result == CUDA_SUCCESS) {
                        plane_result = api.cuMipmappedArrayGetLevel(
                            &plane_array, plane_mipmapped_array, 0);
                    }
                    if (plane_result != CUDA_SUCCESS) {
                        LOG_WARN("CUDA/D3D12 interop: {} surface mapping failed: {} ({})",
                                 plane_name, static_cast<int>(plane_result),
                                 cuda_error(api, plane_result));
                        return false;
                    }
                    return true;
                };

                if (!create_plane(new_width, new_height, DXGI_FORMAT_R8_UNORM, 1,
                                  slot->luma_resource, slot->luma_external_memory,
                                  slot->luma_mipmapped_array, slot->luma_array, "luma") ||
                    !create_plane((new_width + 1u) / 2u, (new_height + 1u) / 2u,
                                  DXGI_FORMAT_R8G8_UNORM, 2,
                                  slot->chroma_resource, slot->chroma_external_memory,
                                  slot->chroma_mipmapped_array, slot->chroma_array,
                                  "chroma")) {
                    destroy_slots();
                    return false;
                }
            }
            destination = std::move(slot);
        }
        width = new_width;
        height = new_height;
        surface_mode = requested_mode;
        LOG_INFO("NVDEC_POOL mode={} size={}x{} slots={} total_ms={:.2f} destroy_ms={:.2f}",
                 interop_surface_mode_name(requested_mode), width, height, slots.size(),
                 milliseconds(elapsed_us(create_start)), milliseconds(destroy_us));
        return true;
    }

    Slot* acquire_slot() {
        for (auto& slot : slots) {
            if (!slot) continue;
            bool expected = false;
            if (slot->in_use.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel))
                return slot.get();
        }
        return nullptr;
    }

    Slot* acquire_slot(size_t index) {
        if (index >= slots.size() || !slots[index]) return nullptr;
        bool expected = false;
        return slots[index]->in_use.compare_exchange_strong(
                   expected, true, std::memory_order_acq_rel)
            ? slots[index].get() : nullptr;
    }

    detail::SurfaceWaitResult wait_until_available(
            size_t index, std::stop_token stop_token) {
        if (index >= slots.size() || !slots[index])
            return detail::SurfaceWaitResult::InvalidSlot;
        auto available = [&] {
            return !slots[index]->in_use.load(std::memory_order_acquire);
        };
        return detail::wait_for_surface_release(
            slot_released, slot_mutex, stop_token, std::chrono::seconds(2), available);
    }

    void release_slot(Slot* slot) {
        if (!slot) return;
        {
            std::lock_guard lock(slot_mutex);
            slot->in_use.store(false, std::memory_order_release);
        }
        slot_released.notify_all();
    }

    std::vector<CUarray> arrays() const {
        std::vector<CUarray> result;
        result.reserve(slots.size());
        for (const auto& slot : slots)
            result.push_back(slot ? slot->array : nullptr);
        return result;
    }

    std::shared_ptr<void> make_owner(Slot* slot) {
        return std::make_shared<Lease>(shared_from_this(), slot);
    }
};

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
    if (!cu_ctx_ && !parser_ && !decoder_ && !pinned_nv12_) return;

    if (cu_ctx_) {
        if (!context_lost_) {
            cuda_.cuCtxPushCurrent(cu_ctx_);

            if (parser_) cuvid_.cuvidDestroyVideoParser(parser_);
            if (decoder_) cuvid_.cuvidDestroyDecoder(decoder_);
            if (pinned_nv12_) cuda_.cuMemFreeHost(pinned_nv12_);

            CUcontext dummy;
            cuda_.cuCtxPopCurrent(&dummy);
        }
        if (interop_) {
            // Frame owners can outlive the decoder. The interop state keeps the
            // CUDA context, shared resources, and imported fence alive until
            // the last renderer-held surface is released.
            interop_->owns_context = true;
        } else {
            cuda_.cuCtxDestroy(cu_ctx_);
        }
    }

    cu_ctx_ = nullptr;
    parser_ = nullptr;
    decoder_ = nullptr;
    pinned_nv12_ = nullptr;
    pinned_nv12_size_ = 0;
    interop_.reset();
    initialized_ = false;
    context_lost_ = false;
    native_interop_active_ = false;
    opaque_output_active_ = false;
    opaque_output_capable_ = false;
}

static cudaVideoCodec to_cuvid_codec(VideoCodecId id) {
    switch (id) {
    case VideoCodecId::H264: return cudaVideoCodec_H264;
    case VideoCodecId::H265: return cudaVideoCodec_HEVC;
    case VideoCodecId::AV1:  return cudaVideoCodec_AV1;
    default:                 return cudaVideoCodec_AV1;
    }
}

bool NvdecDecoder::init(VideoCodecId codec, uint32_t width, uint32_t height,
                        ID3D12Device* render_device) {
    ZoneScopedN("NvdecDecoder::init");
    if (initialized_) return false;

    if (!load_cuda(cuda_)) return false;
    if (!load_cuvid(cuvid_)) return false;

    codec_ = codec;
    cudaVideoCodec cuvid_codec = to_cuvid_codec(codec);

    CUdevice cu_device = 0;
    CUresult res = CUDA_SUCCESS;
    if (render_device) {
        const LUID adapter_luid = render_device->GetAdapterLuid();
        int device_count = 0;
        bool matched = false;
        if (cuda_.cuDeviceGetCount(&device_count) == CUDA_SUCCESS) {
            for (int ordinal = 0; ordinal < device_count; ++ordinal) {
                CUdevice candidate = 0;
                char cuda_luid[8]{};
                unsigned int node_mask = 0;
                if (cuda_.cuDeviceGet(&candidate, ordinal) == CUDA_SUCCESS &&
                    cuda_.cuDeviceGetLuid(cuda_luid, &node_mask, candidate) == CUDA_SUCCESS &&
                    std::memcmp(cuda_luid, &adapter_luid, sizeof(cuda_luid)) == 0) {
                    cu_device = candidate;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            LOG_WARN("CUDA/D3D12 interop: no CUDA device matches the renderer adapter LUID");
            render_device = nullptr;
        }
    }
    if (!render_device)
        res = cuda_.cuDeviceGet(&cu_device, 0);
    if (res != CUDA_SUCCESS) {
        LOG_ERROR("cuDeviceGet failed: {}", (int)res);
        return false;
    }

    res = cuda_.cuCtxCreate(&cu_ctx_, CU_CTX_SCHED_AUTO, cu_device);
    if (res != CUDA_SUCCESS) {
        LOG_ERROR("cuCtxCreate failed: {}", (int)res);
        return false;
    }

    if (render_device) {
        auto interop = std::make_shared<NvdecCudaInteropState>();
        interop->api = cuda_;
        interop->context = cu_ctx_;
        if (interop->initialize(render_device)) {
            interop_ = std::move(interop);
            LOG_INFO("NVDEC CUDA/D3D12 external-memory interop enabled");
        } else {
            LOG_WARN("NVDEC CUDA/D3D12 interop unavailable; using pinned-host fallback");
        }
    }

    CUVIDDECODECAPS caps{};
    caps.eCodecType = cuvid_codec;
    caps.eChromaFormat = cudaVideoChromaFormat_420;
    caps.nBitDepthMinus8 = 0;

    res = cuvid_.cuvidGetDecoderCaps(&caps);
    if (res != CUDA_SUCCESS || !caps.bIsSupported) {
        LOG_ERROR("{} not supported (res={}, supported={})",
                  codec_name(codec), (int)res, (int)caps.bIsSupported);
        interop_.reset();
        cuda_.cuCtxDestroy(cu_ctx_);
        cu_ctx_ = nullptr;
        return false;
    }

    if (width > caps.nMaxWidth || height > caps.nMaxHeight) {
        LOG_ERROR("Resolution {}x{} exceeds max {}x{}",
                  width, height, caps.nMaxWidth, caps.nMaxHeight);
        interop_.reset();
        cuda_.cuCtxDestroy(cu_ctx_);
        cu_ctx_ = nullptr;
        return false;
    }

    opaque_output_capable_ = cuvid_.cuvidRegisterDecodeSurfaces &&
        cuvid_.cuvidDecodePictureAsync &&
        (caps.nOutputFormatMask &
         (1u << static_cast<unsigned int>(cudaVideoSurfaceFormat_NV12_Opaque))) != 0;
    if (render_device && !opaque_output_capable_) {
        LOG_INFO("NVDEC SDK 13.1 opaque output unavailable in the installed NVIDIA driver; "
                 "using the synchronized conversion fallback");
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
        LOG_ERROR("cuvidCreateVideoParser failed: {}", (int)res);
        interop_.reset();
        cuda_.cuCtxDestroy(cu_ctx_);
        cu_ctx_ = nullptr;
        return false;
    }

    CUcontext dummy;
    cuda_.cuCtxPopCurrent(&dummy);

    LOG_INFO("Initialized {} decoder ({}x{})",
             codec_name(codec), width_, height_);
    initialized_ = true;
    return true;
}

bool NvdecDecoder::decode(const uint8_t* data, size_t len, int64_t timestamp) {
    ZoneScopedN("NvdecDecoder::decode");
    if (!initialized_ || context_lost_) return false;

    const auto total_start = DiagnosticClock::now();
    packet_diagnostics_ = {};
    packet_diagnostics_.packet_id = ++next_packet_id_;
    packet_diagnostics_.payload_size = len;
    packet_diagnostics_.timestamp = timestamp;
    collect_packet_diagnostics_ = true;

    const auto context_push_start = DiagnosticClock::now();
    CUresult res = cuda_.cuCtxPushCurrent(cu_ctx_);
    packet_diagnostics_.context_push_us = elapsed_us(context_push_start);
    if (res != CUDA_SUCCESS) {
        collect_packet_diagnostics_ = false;
        LOG_ERROR("CUDA context lost (cuCtxPushCurrent={})", (int)res);
        context_lost_ = true;
        initialized_ = false;
        return false;
    }

    CUVIDSOURCEDATAPACKET pkt{};
    pkt.flags = CUVID_PKT_TIMESTAMP;
    pkt.payload_size = static_cast<unsigned long>(len);
    pkt.payload = data;
    pkt.timestamp = timestamp;

    const auto parser_start = DiagnosticClock::now();
    res = seh_cuvidParseVideoData(cuvid_.cuvidParseVideoData, parser_, &pkt);
    packet_diagnostics_.parser_us = elapsed_us(parser_start);

    CUcontext dummy;
    const auto context_pop_start = DiagnosticClock::now();
    cuda_.cuCtxPopCurrent(&dummy);
    packet_diagnostics_.context_pop_us = elapsed_us(context_pop_start);
    collect_packet_diagnostics_ = false;

    const uint64_t total_us = elapsed_us(total_start);
    if (total_us >= kSlowPacketThresholdUs)
        log_slow_packet(total_us);

    if (res != CUDA_SUCCESS) {
        LOG_ERROR("cuvidParseVideoData failed: {} (GPU context invalidated)", (int)res);
        context_lost_ = true;
        initialized_ = false;
        return false;
    }

    return true;
}

void NvdecDecoder::log_slow_packet(uint64_t total_us) const {
    const auto& d = packet_diagnostics_;
    const auto remainder = [](uint64_t total, uint64_t accounted) {
        return total > accounted ? total - accounted : uint64_t{0};
    };
    const uint64_t callback_us = d.sequence_us + d.decode_callback_us +
                                 d.display_callback_us;
    const uint64_t parser_own_us = remainder(d.parser_us, callback_us);
    const uint64_t sequence_own_us = remainder(
        d.sequence_us, d.decoder_destroy_us + d.pool_create_us +
        d.decoder_create_us + d.surface_register_us + d.pinned_alloc_us);
    const uint64_t decode_own_us = remainder(
        d.decode_callback_us, d.surface_wait_us + d.decode_submit_us);
    const uint64_t display_own_us = remainder(
        d.display_callback_us, d.map_us + d.gpu_copy_us + d.fence_signal_us +
        d.stream_sync_us + d.unmap_us + d.deliver_us);

    LOG_WARN(
        "NVDEC_SLOW packet={} total_ms={:.2f} codec={} size={}x{} bytes={} ts={} "
        "path={} callbacks=seq:{}/reuse:{}/reconfig:{},decode:{},display:{} "
        "surfaces=decode:{}/display:{}",
        d.packet_id, milliseconds(total_us), codec_name(codec_), width_, height_,
        d.payload_size, d.timestamp, d.output_path, d.sequence_callbacks,
        d.sequence_reuses, d.sequence_reconfigurations, d.decode_callbacks,
        d.display_callbacks, d.last_decode_surface, d.last_display_surface);
    LOG_WARN(
        "NVDEC_SLOW packet={} phase_ms=context_push:{:.2f},parser:{:.2f},"
        "parser_own:{:.2f},sequence:{:.2f}/own:{:.2f},"
        "decoder_destroy:{:.2f},pool:{:.2f}/{}x,"
        "decoder_create:{:.2f},register:{:.2f},pinned_alloc:{:.2f},"
        "decode_cb:{:.2f}/own:{:.2f},surface_wait:{:.2f},decode_submit:{:.2f},"
        "display_cb:{:.2f}/own:{:.2f},map:{:.2f},gpu_copy:{:.2f},fence_signal:{:.2f},"
        "stream_sync:{:.2f},unmap:{:.2f},deliver:{:.2f},context_pop:{:.2f}",
        d.packet_id, milliseconds(d.context_push_us), milliseconds(d.parser_us),
        milliseconds(parser_own_us), milliseconds(d.sequence_us),
        milliseconds(sequence_own_us),
        milliseconds(d.decoder_destroy_us), milliseconds(d.pool_create_us),
        d.pool_attempts, milliseconds(d.decoder_create_us),
        milliseconds(d.surface_register_us), milliseconds(d.pinned_alloc_us),
        milliseconds(d.decode_callback_us), milliseconds(decode_own_us),
        milliseconds(d.surface_wait_us),
        milliseconds(d.decode_submit_us), milliseconds(d.display_callback_us),
        milliseconds(display_own_us), milliseconds(d.map_us),
        milliseconds(d.gpu_copy_us),
        milliseconds(d.fence_signal_us), milliseconds(d.stream_sync_us),
        milliseconds(d.unmap_us), milliseconds(d.deliver_us),
        milliseconds(d.context_pop_us));
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
    if (collect_packet_diagnostics_)
        ++packet_diagnostics_.sequence_callbacks;
    AccumulatedTimer sequence_timer(
        collect_packet_diagnostics_, &packet_diagnostics_.sequence_us);

    if (fmt->coded_width == 0 || fmt->coded_height == 0 ||
        fmt->chroma_format > cudaVideoChromaFormat_444) {
        LOG_ERROR("Ignoring invalid sequence header");
        return 1;
    }

    // Compute the actual output dimensions (may differ from coded_width/height
    // if the stream specifies a display/crop area).
    uint32_t target_w = fmt->coded_width;
    uint32_t target_h = fmt->coded_height;
    uint32_t crop_x = 0;
    uint32_t crop_y = 0;
    if (fmt->display_area.right > fmt->display_area.left &&
        fmt->display_area.bottom > fmt->display_area.top) {
        crop_x = fmt->display_area.left > 0
            ? static_cast<uint32_t>(fmt->display_area.left) : 0u;
        crop_y = fmt->display_area.top > 0
            ? static_cast<uint32_t>(fmt->display_area.top) : 0u;
        target_w = fmt->display_area.right - fmt->display_area.left;
        target_h = fmt->display_area.bottom - fmt->display_area.top;
    }

    // Repeated sequence headers are normal, particularly for AV1 keyframes.
    // The parser only requires at least min_num_decode_surfaces. Our optional
    // headroom is not part of that contract and can be truncated by NVDEC's
    // 32-surface registration limit. Requiring min + headroom here made reuse
    // permanently impossible near that limit and rebuilt the decoder every
    // frame, turning a sub-millisecond parse into a ~250 ms stall.
    const detail::NvdecSequenceState state{
        decoder_ != nullptr, width_, height_, native_texture_width_,
        native_texture_height_, native_crop_x_, native_crop_y_, bit_depth_,
        num_decode_surfaces_};
    const detail::NvdecSequenceFormat format{
        target_w, target_h, fmt->coded_width, fmt->coded_height,
        crop_x, crop_y, static_cast<uint32_t>(fmt->bit_depth_luma_minus8 + 8),
        fmt->min_num_decode_surfaces};
    if (detail::can_reuse_decoder(state, format)) {
        if (collect_packet_diagnostics_)
            ++packet_diagnostics_.sequence_reuses;
        return num_decode_surfaces_;
    }

    if (collect_packet_diagnostics_)
        ++packet_diagnostics_.sequence_reconfigurations;

    LOG_INFO("NVDEC_RECONFIG packet={} visible={}x{}->{}x{} coded={}x{}->{}x{} "
             "crop={},{}->{},{} depth={}->{} surfaces={}->min:{}",
             packet_diagnostics_.packet_id, width_, height_, target_w, target_h,
             native_texture_width_, native_texture_height_, fmt->coded_width,
             fmt->coded_height, native_crop_x_, native_crop_y_, crop_x, crop_y,
             bit_depth_, fmt->bit_depth_luma_minus8 + 8, num_decode_surfaces_,
             fmt->min_num_decode_surfaces);

    if (decoder_) {
        const auto destroy_start = DiagnosticClock::now();
        cuvid_.cuvidDestroyDecoder(decoder_);
        if (collect_packet_diagnostics_)
            packet_diagnostics_.decoder_destroy_us += elapsed_us(destroy_start);
        decoder_ = nullptr;
    }
    opaque_output_active_ = false;
    native_interop_active_ = false;

    bit_depth_ = fmt->bit_depth_luma_minus8 + 8;
    width_ = target_w;
    height_ = target_h;
    native_texture_width_ = fmt->coded_width;
    native_texture_height_ = fmt->coded_height;
    native_crop_x_ = crop_x;
    native_crop_y_ = crop_y;

    bool is_10bit = fmt->bit_depth_luma_minus8 > 0;
    if (is_10bit) {
        // DecodedFrame currently exposes byte NV12 and the CUDA conversion
        // kernel is deliberately 8-bit. Never reinterpret P016 as NV12.
        LOG_ERROR("NVDEC 10-bit output is not supported by the current frame contract");
        native_interop_active_ = false;
        return 0;
    }
    const uint32_t legacy_surface_count = fmt->min_num_decode_surfaces + 4;
    const bool opaque_api_available = interop_ && opaque_output_capable_ &&
        fmt->chroma_format == cudaVideoChromaFormat_420;
    const uint32_t opaque_surface_count =
        (std::min)(fmt->min_num_decode_surfaces + kOpaqueSurfaceHeadroom,
                   static_cast<uint32_t>(MAX_NUM_REGISTERED_DECODE_SURFACES));

    CUVIDDECODECREATEINFO create_info{};
    create_info.ulWidth = fmt->coded_width;
    create_info.ulHeight = fmt->coded_height;
    create_info.CodecType = fmt->codec;
    create_info.ChromaFormat = fmt->chroma_format;
    create_info.ulCreationFlags = cudaVideoCreate_PreferCUVID;
    create_info.bitDepthMinus8 = fmt->bit_depth_luma_minus8;
    create_info.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
    create_info.ulTargetWidth = target_w;
    create_info.ulTargetHeight = target_h;

    if (fmt->display_area.right > fmt->display_area.left &&
        fmt->display_area.bottom > fmt->display_area.top) {
        create_info.display_area.left = static_cast<short>(fmt->display_area.left);
        create_info.display_area.top = static_cast<short>(fmt->display_area.top);
        create_info.display_area.right = static_cast<short>(fmt->display_area.right);
        create_info.display_area.bottom = static_cast<short>(fmt->display_area.bottom);
    }

    CUresult res = CUDA_SUCCESS;
    bool opaque_slots_ready = false;
    if (opaque_api_available) {
        const auto pool_start = DiagnosticClock::now();
        opaque_slots_ready = interop_->create_slots(
            fmt->coded_width, fmt->coded_height, opaque_surface_count,
            InteropSurfaceMode::OpaqueNV12);
        if (collect_packet_diagnostics_) {
            packet_diagnostics_.pool_create_us += elapsed_us(pool_start);
            ++packet_diagnostics_.pool_attempts;
        }
    }
    if (opaque_api_available && opaque_slots_ready) {
        // Opaque output bypasses NVDEC's post-processing/output stage and writes
        // straight into the registered coded-size CUarrays. In particular, the
        // decoder must not be asked to crop to display_area here: doing so makes
        // its idea of the NV12 plane layout differ from the D3D12 resource whose
        // plane offsets are derived from coded_width/coded_height.
        create_info.ulTargetWidth = fmt->coded_width;
        create_info.ulTargetHeight = fmt->coded_height;
        create_info.display_area = {};
        create_info.ulNumDecodeSurfaces = opaque_surface_count;
        create_info.OutputFormat = cudaVideoSurfaceFormat_NV12_Opaque;
        create_info.ulNumOutputSurfaces = 0;
        const auto create_start = DiagnosticClock::now();
        res = cuvid_.cuvidCreateDecoder(&decoder_, &create_info);
        if (collect_packet_diagnostics_)
            packet_diagnostics_.decoder_create_us += elapsed_us(create_start);
        if (res == CUDA_SUCCESS) {
            auto decode_arrays = interop_->arrays();
            CUVIDREGISTERDECODESURFACESINFO register_info{};
            register_info.ulNumDecodeSurfaces = static_cast<unsigned int>(decode_arrays.size());
            register_info.pDecodeSurfaces = decode_arrays.data();
            const auto register_start = DiagnosticClock::now();
            res = cuvid_.cuvidRegisterDecodeSurfaces(decoder_, &register_info);
            if (collect_packet_diagnostics_)
                packet_diagnostics_.surface_register_us += elapsed_us(register_start);
        }
        if (res == CUDA_SUCCESS) {
            num_decode_surfaces_ = opaque_surface_count;
            opaque_output_active_ = true;
            native_interop_active_ = true;
            LOG_INFO("NVDEC SDK 13.1 opaque-output path enabled ({} registered NV12 surfaces)",
                     num_decode_surfaces_);
        } else {
            LOG_WARN("NVDEC opaque-output setup failed: {} ({}); falling back to mapped output",
                     static_cast<int>(res), cuda_error(cuda_, res));
            if (decoder_) {
                const auto destroy_start = DiagnosticClock::now();
                cuvid_.cuvidDestroyDecoder(decoder_);
                if (collect_packet_diagnostics_)
                    packet_diagnostics_.decoder_destroy_us += elapsed_us(destroy_start);
            }
            decoder_ = nullptr;
        }
    }

    if (!decoder_) {
        // The mapped legacy path still goes through NVDEC post-processing, so
        // retain its visible-size crop/scaling behavior.
        create_info.ulTargetWidth = target_w;
        create_info.ulTargetHeight = target_h;
        create_info.display_area = {};
        if (fmt->display_area.right > fmt->display_area.left &&
            fmt->display_area.bottom > fmt->display_area.top) {
            create_info.display_area.left = static_cast<short>(fmt->display_area.left);
            create_info.display_area.top = static_cast<short>(fmt->display_area.top);
            create_info.display_area.right = static_cast<short>(fmt->display_area.right);
            create_info.display_area.bottom = static_cast<short>(fmt->display_area.bottom);
        }
        create_info.ulNumDecodeSurfaces = legacy_surface_count;
        create_info.OutputFormat = cudaVideoSurfaceFormat_NV12;
        create_info.ulNumOutputSurfaces = 2;
        const auto create_start = DiagnosticClock::now();
        res = cuvid_.cuvidCreateDecoder(&decoder_, &create_info);
        if (collect_packet_diagnostics_)
            packet_diagnostics_.decoder_create_us += elapsed_us(create_start);
        if (res != CUDA_SUCCESS) {
            LOG_ERROR("cuvidCreateDecoder failed: {}", static_cast<int>(res));
            return 0;
        }
        num_decode_surfaces_ = legacy_surface_count;
        if (interop_) {
            const auto pool_start = DiagnosticClock::now();
            native_interop_active_ = interop_->create_slots(
                width_, height_, kFallbackSurfaceCount,
                InteropSurfaceMode::RgbaFallback);
            if (collect_packet_diagnostics_) {
                packet_diagnostics_.pool_create_us += elapsed_us(pool_start);
                ++packet_diagnostics_.pool_attempts;
            }
        }
    }

    size_t nv12_size = static_cast<size_t>(width_) * height_ * 3 / 2;
    if (!native_interop_active_ && nv12_size > pinned_nv12_size_) {
        const auto allocation_start = DiagnosticClock::now();
        if (pinned_nv12_) cuda_.cuMemFreeHost(pinned_nv12_);
        pinned_nv12_ = nullptr;
        pinned_nv12_size_ = 0;

        void* ptr = nullptr;
        res = cuda_.cuMemAllocHost(&ptr, nv12_size);
        if (res == CUDA_SUCCESS) {
            pinned_nv12_ = static_cast<uint8_t*>(ptr);
            pinned_nv12_size_ = nv12_size;
        }
        if (collect_packet_diagnostics_)
            packet_diagnostics_.pinned_alloc_us += elapsed_us(allocation_start);
    }

    return static_cast<int>(num_decode_surfaces_);
}

int NvdecDecoder::on_decode(CUVIDPICPARAMS* pic) {
    ZoneScopedN("NvdecDecoder::on_decode");
    if (collect_packet_diagnostics_) {
        ++packet_diagnostics_.decode_callbacks;
        packet_diagnostics_.last_decode_surface = pic ? pic->CurrPicIdx : -1;
    }
    AccumulatedTimer callback_timer(
        collect_packet_diagnostics_, &packet_diagnostics_.decode_callback_us);
    if (!decoder_) return 0;

    if (opaque_output_active_ && interop_) {
        const auto wait_start = DiagnosticClock::now();
        const auto wait_result = interop_->wait_until_available(
            static_cast<size_t>(pic->CurrPicIdx), stop_token_);
        if (collect_packet_diagnostics_)
            packet_diagnostics_.surface_wait_us += elapsed_us(wait_start);
        if (wait_result == detail::SurfaceWaitResult::Cancelled)
            return 0;
        if (wait_result == detail::SurfaceWaitResult::InvalidSlot) {
            LOG_ERROR("NVDEC opaque surface index {} is outside the registered pool",
                      pic->CurrPicIdx);
            return 0;
        }
        if (wait_result == detail::SurfaceWaitResult::TimedOut) {
            LOG_ERROR("NVDEC opaque surface {} remained owned by the renderer for over 2 seconds",
                      pic->CurrPicIdx);
            return 0;
        }
    }

    const auto submit_start = DiagnosticClock::now();
    CUresult res = opaque_output_active_
        ? cuvid_.cuvidDecodePictureAsync(decoder_, pic, interop_->stream)
        : cuvid_.cuvidDecodePicture(decoder_, pic);
    if (collect_packet_diagnostics_)
        packet_diagnostics_.decode_submit_us += elapsed_us(submit_start);
    if (res != CUDA_SUCCESS) {
        LOG_ERROR("{} failed: {}", opaque_output_active_
                  ? "cuvidDecodePictureAsync" : "cuvidDecodePicture", (int)res);
        return 0;
    }
    return 1;
}

int NvdecDecoder::on_display(CUVIDPARSERDISPINFO* disp_info) {
    ZoneScopedN("NvdecDecoder::on_display");
    if (collect_packet_diagnostics_) {
        ++packet_diagnostics_.display_callbacks;
        packet_diagnostics_.last_display_surface = disp_info
            ? disp_info->picture_index : -1;
    }
    AccumulatedTimer callback_timer(
        collect_packet_diagnostics_, &packet_diagnostics_.display_callback_us);
    if (!decoder_ || !disp_info || !on_decoded) return 1;

    CUVIDPROCPARAMS proc{};
    proc.progressive_frame = disp_info->progressive_frame;
    proc.top_field_first = disp_info->top_field_first;

    if (opaque_output_active_) {
        if (collect_packet_diagnostics_)
            packet_diagnostics_.output_path = "opaque-nv12";
        if (!interop_ || interop_->surface_mode != InteropSurfaceMode::OpaqueNV12 ||
            interop_->slots.empty()) {
            LOG_ERROR("NVDEC opaque display callback has no registered surface pool");
            return 0;
        }

        auto* slot = interop_->acquire_slot(
            static_cast<size_t>(disp_info->picture_index));
        if (!slot) {
            LOG_ERROR("NVDEC opaque surface {} was presented while still in use",
                      disp_info->picture_index);
            return 0;
        }

        // SDK 13.1 opaque output is a private block-linear CUDA layout, not a
        // portable DXGI multi-plane layout. Copy the two decoded planes with the
        // GPU copy engine into ordinary R8/RG8 shared textures. RmlUI samples
        // these as NV12 directly; there is still no RGB conversion or host copy.
        CUDA_MEMCPY2D luma_copy{};
        luma_copy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
        luma_copy.srcArray = slot->decoded_luma_array;
        luma_copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
        luma_copy.dstArray = slot->luma_array;
        luma_copy.WidthInBytes = interop_->width;
        luma_copy.Height = interop_->height;
        const auto copy_start = DiagnosticClock::now();
        CUresult copy_result = interop_->api.cuMemcpy2DAsync(
            &luma_copy, interop_->stream);

        CUDA_MEMCPY2D chroma_copy{};
        chroma_copy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
        chroma_copy.srcArray = slot->decoded_chroma_array;
        chroma_copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
        chroma_copy.dstArray = slot->chroma_array;
        chroma_copy.WidthInBytes = interop_->width;
        chroma_copy.Height = (interop_->height + 1u) / 2u;
        if (copy_result == CUDA_SUCCESS) {
            copy_result = interop_->api.cuMemcpy2DAsync(
                &chroma_copy, interop_->stream);
        }
        if (collect_packet_diagnostics_)
            packet_diagnostics_.gpu_copy_us += elapsed_us(copy_start);
        if (copy_result != CUDA_SUCCESS) {
            interop_->release_slot(slot);
            LOG_ERROR("NVDEC opaque chroma copy failed: {} ({})",
                      static_cast<int>(copy_result),
                      cuda_error(interop_->api, copy_result));
            return 0;
        }

        const uint64_t ready_value = ++interop_->next_fence_value;
        CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS signal{};
        signal.params.fence.value = ready_value;
        const auto signal_start = DiagnosticClock::now();
        const CUresult signal_result = interop_->api.cuSignalExternalSemaphoresAsync(
            &interop_->ready_semaphore, &signal, 1, interop_->stream);
        if (collect_packet_diagnostics_)
            packet_diagnostics_.fence_signal_us += elapsed_us(signal_start);
        if (signal_result != CUDA_SUCCESS) {
            interop_->release_slot(slot);
            LOG_ERROR("NVDEC opaque D3D12 fence signal failed: {} ({})",
                      static_cast<int>(signal_result),
                      cuda_error(interop_->api, signal_result));
            return 0;
        }

        DecodedFrame frame{};
        frame.width = width_;
        frame.height = height_;
        frame.timestamp = disp_info->timestamp;
        frame.nv12 = true;
        frame.native_rgba = false;
        frame.native_d3d12_resource = slot->luma_resource.Get();
        frame.native_d3d12_chroma_resource = slot->chroma_resource.Get();
        frame.native_d3d12_fence = interop_->ready_fence.Get();
        frame.native_fence_value = ready_value;
        frame.native_d3d12_state = D3D12_RESOURCE_STATE_COMMON;
        frame.native_texture_width = native_texture_width_;
        frame.native_texture_height = native_texture_height_;
        frame.native_crop_x = native_crop_x_;
        frame.native_crop_y = native_crop_y_;
        frame.native_owner = interop_->make_owner(slot);
        const auto deliver_start = DiagnosticClock::now();
        on_decoded(frame);
        if (collect_packet_diagnostics_)
            packet_diagnostics_.deliver_us += elapsed_us(deliver_start);
        return 1;
    }

    unsigned long long dev_ptr = 0;
    unsigned int pitch = 0;

    const auto map_start = DiagnosticClock::now();
    CUresult res = cuvid_.cuvidMapVideoFrame64(
        decoder_, disp_info->picture_index, &dev_ptr, &pitch, &proc);
    if (collect_packet_diagnostics_)
        packet_diagnostics_.map_us += elapsed_us(map_start);
    if (res != CUDA_SUCCESS) {
        LOG_ERROR("cuvidMapVideoFrame64 failed: {}", (int)res);
        return 0;
    }

    if (native_interop_active_ && interop_ &&
        interop_->surface_mode == InteropSurfaceMode::RgbaFallback &&
        interop_->width == width_ && interop_->height == height_ &&
        !interop_->slots.empty() && interop_->slots[0]) {
        if (collect_packet_diagnostics_)
            packet_diagnostics_.output_path = "mapped-rgba";
        auto* slot = interop_->acquire_slot();
        if (!slot) {
            // The render queue still owns every ring slot. Dropping this frame
            // keeps decode latency bounded instead of stalling the receive path.
            const auto unmap_start = DiagnosticClock::now();
            cuvid_.cuvidUnmapVideoFrame64(decoder_, dev_ptr);
            if (collect_packet_diagnostics_)
                packet_diagnostics_.unmap_us += elapsed_us(unmap_start);
            return 1;
        }

        CUsurfObject surface = slot->surface;
        CUdeviceptr source = static_cast<CUdeviceptr>(dev_ptr);
        uint32_t source_pitch = pitch;
        uint32_t output_width = width_;
        uint32_t output_height = height_;
        void* arguments[] = {
            &surface, &source, &source_pitch, &output_width, &output_height
        };
        const unsigned int block_width = 16;
        const unsigned int block_height = 16;
        const auto copy_start = DiagnosticClock::now();
        res = interop_->api.cuLaunchKernel(
            interop_->kernel,
            (width_ + block_width - 1) / block_width,
            (height_ + block_height - 1) / block_height,
            1, block_width, block_height, 1, 0, interop_->stream, arguments, nullptr);
        if (collect_packet_diagnostics_)
            packet_diagnostics_.gpu_copy_us += elapsed_us(copy_start);

        const uint64_t ready_value = ++interop_->next_fence_value;
        if (res == CUDA_SUCCESS) {
            CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS signal{};
            signal.params.fence.value = ready_value;
            const auto signal_start = DiagnosticClock::now();
            res = interop_->api.cuSignalExternalSemaphoresAsync(
                &interop_->ready_semaphore, &signal, 1, interop_->stream);
            if (collect_packet_diagnostics_)
                packet_diagnostics_.fence_signal_us += elapsed_us(signal_start);
        }
        if (res == CUDA_SUCCESS) {
            const auto sync_start = DiagnosticClock::now();
            res = interop_->api.cuStreamSynchronize(interop_->stream);
            if (collect_packet_diagnostics_)
                packet_diagnostics_.stream_sync_us += elapsed_us(sync_start);
        }

        const auto unmap_start = DiagnosticClock::now();
        cuvid_.cuvidUnmapVideoFrame64(decoder_, dev_ptr);
        if (collect_packet_diagnostics_)
            packet_diagnostics_.unmap_us += elapsed_us(unmap_start);
        if (res != CUDA_SUCCESS) {
            interop_->release_slot(slot);
            LOG_ERROR("NVDEC CUDA/D3D12 conversion failed: {} ({})",
                      static_cast<int>(res), cuda_error(interop_->api, res));
            context_lost_ = true;
            initialized_ = false;
            return 0;
        }

        DecodedFrame frame{};
        frame.width = width_;
        frame.height = height_;
        frame.timestamp = disp_info->timestamp;
        frame.nv12 = false;
        frame.native_rgba = true;
        frame.native_d3d12_resource = slot->resource.Get();
        frame.native_d3d12_fence = interop_->ready_fence.Get();
        frame.native_fence_value = ready_value;
        frame.native_owner = interop_->make_owner(slot);
        const auto deliver_start = DiagnosticClock::now();
        on_decoded(frame);
        if (collect_packet_diagnostics_)
            packet_diagnostics_.deliver_us += elapsed_us(deliver_start);
        return 1;
    }

    if (collect_packet_diagnostics_)
        packet_diagnostics_.output_path = "mapped-host-nv12";
    size_t host_size = static_cast<size_t>(width_) * height_ * 3 / 2;

    bool copied_to_host = false;
    {
        ZoneScopedN("nvdec::gpu_to_host");

        if (pinned_nv12_ && pinned_nv12_size_ >= host_size) {
            const auto copy_start = DiagnosticClock::now();
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
                LOG_ERROR("cuMemcpy2D Y failed: {}", (int)r);
            }

            if (r == CUDA_SUCCESS) {
                copy.srcDevice = dev_ptr + static_cast<CUdeviceptr>(pitch) * height_;
                copy.dstHost = pinned_nv12_ + static_cast<size_t>(width_) * height_;
                copy.Height = height_ / 2;
                r = cuda_.cuMemcpy2D(&copy);
            }
            if (r != CUDA_SUCCESS) {
                LOG_ERROR("cuMemcpy2D UV failed: {}", (int)r);
            } else {
                copied_to_host = true;
            }
            if (collect_packet_diagnostics_)
                packet_diagnostics_.gpu_copy_us += elapsed_us(copy_start);
        }
    }

    const auto unmap_start = DiagnosticClock::now();
    cuvid_.cuvidUnmapVideoFrame64(decoder_, dev_ptr);
    if (collect_packet_diagnostics_)
        packet_diagnostics_.unmap_us += elapsed_us(unmap_start);

    if (!copied_to_host) {
        LOG_ERROR("NVDEC pinned-host fallback has no complete output frame");
        return 0;
    }

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

    const auto deliver_start = DiagnosticClock::now();
    on_decoded(frame);
    if (collect_packet_diagnostics_)
        packet_diagnostics_.deliver_us += elapsed_us(deliver_start);
    return 1;
}

} // namespace parties::encdec::nvidia
