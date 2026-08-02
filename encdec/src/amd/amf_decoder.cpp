#include "amf_decoder.h"
#include "amf_decode_state.h"
#include "amf_loader.h"

#include <AMF/components/VideoDecoderUVD.h>
#include <AMF/core/Surface.h>
#include <AMF/core/Plane.h>

#include <parties/profiler.h>
#include <parties/log.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <d3d11_4.h>
#include <dxgi1_3.h>
#include <dxgi.h>
#include <thread>

namespace parties::encdec::amd {

struct AmfNativeLifetime {
    amf::AMFComponent* decoder = nullptr;
    amf::AMFContext* context = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    bool context_lost = false;

    ~AmfNativeLifetime() {
        if (decoder) {
            if (!context_lost) decoder->Terminate();
            decoder->Release();
        }
        if (context) {
            if (!context_lost) context->Terminate();
            context->Release();
        }
    }
};

struct AmfD3D11InteropState : std::enable_shared_from_this<AmfD3D11InteropState> {
    enum class SlotCreationResult {
        Ready,
        Busy,
        Failed,
    };

    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> processor_texture;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> d3d11_texture;
        Microsoft::WRL::ComPtr<ID3D12Resource> d3d12_texture;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> output_view;
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyed_mutex;
        std::atomic<bool> in_use{false};
    };

    struct Lease {
        std::shared_ptr<AmfD3D11InteropState> state;
        std::shared_ptr<AmfNativeLifetime> lifetime;
        amf::AMFSurface* surface = nullptr;
        Slot* slot = nullptr;

        ~Lease() {
            if (surface) surface->Release();
            if (slot) slot->in_use.store(false, std::memory_order_release);
        }
    };

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> video_enumerator;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> video_processor;
    Microsoft::WRL::ComPtr<ID3D11Fence> d3d11_fence;
    Microsoft::WRL::ComPtr<ID3D12Device> d3d12_device;
    Microsoft::WRL::ComPtr<ID3D12Fence> d3d12_fence;
    std::array<std::unique_ptr<Slot>, 4> slots;
    D3D11_TEXTURE2D_DESC texture_desc{};
    uint64_t next_fence_value = 0;

    bool initialize(ID3D11Device* source_device, ID3D11DeviceContext* source_context,
                    ID3D12Device* destination_device) {
        if (!source_device || !source_context || !destination_device) return false;
        if (source_device->QueryInterface(IID_PPV_ARGS(&device)) != S_OK ||
            source_context->QueryInterface(IID_PPV_ARGS(&context4)) != S_OK)
            return false;
        context = source_context;
        d3d12_device = destination_device;

        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC source_adapter{};
        const LUID destination_luid = destination_device->GetAdapterLuid();
        if (FAILED(source_device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) ||
            FAILED(dxgi_device->GetAdapter(&adapter)) ||
            FAILED(adapter->GetDesc(&source_adapter)) ||
            std::memcmp(&source_adapter.AdapterLuid, &destination_luid,
                        sizeof(LUID)) != 0)
            return false;

        Microsoft::WRL::ComPtr<ID3D11Device5> device5;
        if (FAILED(source_device->QueryInterface(IID_PPV_ARGS(&device5))) ||
            FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                                        IID_PPV_ARGS(&d3d11_fence))))
            return false;
        HANDLE shared_fence = nullptr;
        if (FAILED(d3d11_fence->CreateSharedHandle(nullptr, GENERIC_ALL,
                                                   nullptr, &shared_fence)))
            return false;
        const HRESULT open_result = destination_device->OpenSharedHandle(
            shared_fence, IID_PPV_ARGS(&d3d12_fence));
        CloseHandle(shared_fence);
        return SUCCEEDED(open_result);
    }

    SlotCreationResult create_slots(ID3D11Texture2D* source,
                                    uint32_t visible_width,
                                    uint32_t visible_height) {
        if (!source) return SlotCreationResult::Failed;
        D3D11_TEXTURE2D_DESC source_desc{};
        source->GetDesc(&source_desc);
        if (source_desc.Format != DXGI_FORMAT_NV12 ||
            visible_width == 0 || visible_height == 0)
            return SlotCreationResult::Failed;
        if (slots[0] && visible_width == texture_desc.Width &&
            visible_height == texture_desc.Height)
            return SlotCreationResult::Ready;
        for (const auto& slot : slots)
            if (slot && slot->in_use.load(std::memory_order_acquire))
                return SlotCreationResult::Busy;
        slots = {};

        video_enumerator.Reset();
        video_processor.Reset();
        if (!video_device &&
            (FAILED(device.As(&video_device)) || FAILED(context.As(&video_context))))
            return SlotCreationResult::Failed;
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputWidth = source_desc.Width;
        content.InputHeight = source_desc.Height;
        content.OutputWidth = visible_width;
        content.OutputHeight = visible_height;
        content.InputFrameRate = {60, 1};
        content.OutputFrameRate = {60, 1};
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (FAILED(video_device->CreateVideoProcessorEnumerator(
                &content, &video_enumerator)))
            return SlotCreationResult::Failed;
        constexpr DXGI_FORMAT output_format = DXGI_FORMAT_R8G8B8A8_UNORM;
        UINT format_support = 0;
        if (FAILED(video_enumerator->CheckVideoProcessorFormat(output_format, &format_support)) ||
            !(format_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
            return SlotCreationResult::Failed;
        if (FAILED(video_device->CreateVideoProcessor(
                video_enumerator.Get(), 0, &video_processor)))
            return SlotCreationResult::Failed;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = visible_width;
        desc.Height = visible_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = output_format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                         D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        for (auto& destination : slots) {
            auto slot = std::make_unique<Slot>();
            HRESULT result = device->CreateTexture2D(
                &desc, nullptr, &slot->d3d11_texture);
            if (FAILED(result)) {
                LOG_WARN("AMF interop CreateTexture2D failed: 0x{:08x}",
                         static_cast<uint32_t>(result));
                return SlotCreationResult::Failed;
            }
            if (FAILED(slot->d3d11_texture.As(&slot->keyed_mutex)))
                return SlotCreationResult::Failed;
            Microsoft::WRL::ComPtr<IDXGIResource1> shared_resource;
            if (FAILED(slot->d3d11_texture.As(&shared_resource)))
                return SlotCreationResult::Failed;
            HANDLE shared_texture = nullptr;
            result = shared_resource->CreateSharedHandle(
                nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                nullptr, &shared_texture);
            if (FAILED(result)) {
                LOG_WARN("AMF interop CreateSharedHandle failed: 0x{:08x}",
                         static_cast<uint32_t>(result));
                return SlotCreationResult::Failed;
            }
            const HRESULT open_result = d3d12_device->OpenSharedHandle(
                shared_texture, IID_PPV_ARGS(&slot->d3d12_texture));
            CloseHandle(shared_texture);
            if (FAILED(open_result)) {
                LOG_WARN("AMF interop OpenSharedResource1 failed: 0x{:08x}",
                         static_cast<uint32_t>(open_result));
                return SlotCreationResult::Failed;
            }
            D3D11_TEXTURE2D_DESC processor_desc = desc;
            processor_desc.MiscFlags = 0;
            if (FAILED(device->CreateTexture2D(
                    &processor_desc, nullptr, &slot->processor_texture)))
                return SlotCreationResult::Failed;
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_view_desc{};
            output_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            if (FAILED(video_device->CreateVideoProcessorOutputView(
                    slot->processor_texture.Get(), video_enumerator.Get(),
                    &output_view_desc, &slot->output_view)))
                return SlotCreationResult::Failed;
            destination = std::move(slot);
        }
        texture_desc = desc;
        LOG_INFO("AMF DX11/DX12 interop allocated {} shared RGBA surfaces at {}x{}",
                 slots.size(), desc.Width, desc.Height);
        return SlotCreationResult::Ready;
    }

    Slot* acquire() {
        for (auto& slot : slots) {
            if (!slot) continue;
            bool expected = false;
            if (slot->in_use.compare_exchange_strong(expected, true,
                                                     std::memory_order_acq_rel))
                return slot.get();
        }
        return nullptr;
    }

    bool copy(ID3D11Texture2D* source, Slot& slot, uint64_t& fence_value) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
        input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view;
        HRESULT result = video_device->CreateVideoProcessorInputView(
            source, video_enumerator.Get(), &input_desc, &input_view);
        if (FAILED(result)) {
            LOG_WARN("AMF interop CreateVideoProcessorInputView failed: 0x{:08x}",
                     static_cast<uint32_t>(result));
            return false;
        }
        const RECT rect{0, 0, static_cast<LONG>(texture_desc.Width),
                        static_cast<LONG>(texture_desc.Height)};
        video_context->VideoProcessorSetStreamSourceRect(
            video_processor.Get(), 0, TRUE, &rect);
        video_context->VideoProcessorSetStreamDestRect(
            video_processor.Get(), 0, TRUE, &rect);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = input_view.Get();
        result = video_context->VideoProcessorBlt(
            video_processor.Get(), slot.output_view.Get(), 0, 1, &stream);
        if (FAILED(result)) {
            LOG_WARN("AMF interop VideoProcessorBlt failed: 0x{:08x}",
                     static_cast<uint32_t>(result));
            return false;
        }
        result = slot.keyed_mutex->AcquireSync(0, 1'000);
        if (FAILED(result)) {
            LOG_WARN("AMF interop AcquireSync failed: 0x{:08x}",
                     static_cast<uint32_t>(result));
            return false;
        }
        context->CopyResource(slot.d3d11_texture.Get(), slot.processor_texture.Get());
        fence_value = ++next_fence_value;
        result = context4->Signal(d3d11_fence.Get(), fence_value);
        const HRESULT release_result = slot.keyed_mutex->ReleaseSync(0);
        if (FAILED(result)) {
            LOG_WARN("AMF interop ID3D11DeviceContext4::Signal failed: 0x{:08x}",
                     static_cast<uint32_t>(result));
            return false;
        }
        if (FAILED(release_result)) {
            LOG_WARN("AMF interop ReleaseSync failed: 0x{:08x}",
                     static_cast<uint32_t>(release_result));
            return false;
        }
        return true;
    }

    std::shared_ptr<void> make_owner(Slot* slot, amf::AMFSurface* surface,
                                     std::shared_ptr<AmfNativeLifetime> lifetime) {
        auto owner = std::make_shared<Lease>();
        owner->state = shared_from_this();
        owner->lifetime = std::move(lifetime);
        owner->surface = surface;
        owner->slot = slot;
        return owner;
    }
};

static const wchar_t* decoder_component_id(VideoCodecId codec) {
    switch (codec) {
    case VideoCodecId::AV1:  return AMFVideoDecoderHW_AV1;
    case VideoCodecId::H265: return AMFVideoDecoderHW_H265_HEVC;
    case VideoCodecId::H264: return AMFVideoDecoderUVD_H264_AVC;
    default:                 return AMFVideoDecoderHW_AV1;
    }
}

AmfDecoder::AmfDecoder() = default;

AmfDecoder::~AmfDecoder() {
    if (native_lifetime_) {
        // Native frames may still be queued in RmlUI. Transfer component and
        // context teardown to their shared lifetime so AMF cannot invalidate
        // an in-flight surface when the stream object is replaced.
        native_lifetime_->context_lost = context_lost_;
        decoder_ = nullptr;
        context_ = nullptr;
        d3d12_device_.Reset();
        native_lifetime_.reset();
    }
    if (decoder_) {
        // Terminate is the AMF cleanup boundary. Draining during destruction is
        // both unnecessary and dangerous: no output consumer remains and a
        // full surface pool can make the driver wait indefinitely.
        if (!context_lost_) decoder_->Terminate();
        decoder_->Release();
        decoder_ = nullptr;
    }

    if (context_) {
        if (!context_lost_)
            context_->Terminate();
        context_->Release();
        context_ = nullptr;
    }

    device_.Reset();
    device_context_.Reset();
    interop_.reset();
    d3d12_device_.Reset();
    initialized_ = false;
    context_lost_ = false;
}

bool AmfDecoder::check_device_health() {
    HRESULT reason = S_OK;
    if (d3d12_device_)
        reason = d3d12_device_->GetDeviceRemovedReason();
    else if (device_)
        reason = device_->GetDeviceRemovedReason();
    else
        return false;
    if (reason != S_OK) {
        LOG_ERROR("AMF graphics device removed (reason=0x{:08x})", (unsigned)reason);
        context_lost_ = true;
        return false;
    }
    return true;
}

void AmfDecoder::mark_context_lost(const char* operation, AMF_RESULT result) {
    LOG_ERROR("AMF decoder {} failed: {}", operation, static_cast<int>(result));
    context_lost_ = true;
}

bool AmfDecoder::init(VideoCodecId codec, uint32_t width, uint32_t height,
                      ID3D12Device* render_device) {
    ZoneScopedN("AmfDecoder::init");
    if (initialized_) return false;

    if (!load_amf(factory_)) return false;

    D3D_FEATURE_LEVEL selected_feature_level{};

    // AMF hardware decode is a DX11 path on Windows. Native InitDX12 may appear
    // to initialize on some drivers but invalidates the application device as
    // soon as decoded resources are consumed. Decode on the matching AMD DX11
    // adapter and bridge to RmlUI with shared resources and a shared fence.
    Microsoft::WRL::ComPtr<IDXGIFactory1> dxgi_factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory)))) {
        LOG_ERROR("CreateDXGIFactory1 failed");
        return false;
    }

    const LUID render_luid = render_device ? render_device->GetAdapterLuid() : LUID{};
    Microsoft::WRL::ComPtr<IDXGIAdapter1> amd_adapter;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> matching_adapter;
    for (UINT index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
        if (dxgi_factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        candidate->GetDesc1(&desc);
        if (desc.VendorId != 0x1002) continue;
        if (!amd_adapter) amd_adapter = candidate;
        if (render_device && std::memcmp(&desc.AdapterLuid, &render_luid, sizeof(LUID)) == 0) {
            matching_adapter = candidate;
            break;
        }
    }
    if (matching_adapter) amd_adapter = matching_adapter;

    constexpr std::array feature_levels{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    const HRESULT device_result = D3D11CreateDevice(
        amd_adapter.Get(), amd_adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        feature_levels.data(), static_cast<UINT>(feature_levels.size()),
        D3D11_SDK_VERSION, &device_, &selected_feature_level, &device_context_);
    if (FAILED(device_result)) {
        LOG_ERROR("D3D11CreateDevice for AMF decoder failed: 0x{:08x}",
                  static_cast<unsigned>(device_result));
        return false;
    }

    const AMF_RESULT create_result = factory_->CreateContext(&context_);
    if (create_result != AMF_OK || !context_) {
        LOG_ERROR("Decoder CreateContext failed: {}", static_cast<int>(create_result));
        return false;
    }
    AMF_RESULT res = context_->InitDX11(device_.Get());
    if (res != AMF_OK) {
        LOG_ERROR("Decoder InitDX11 failed: {}", static_cast<int>(res));
        context_->Release();
        context_ = nullptr;
        return false;
    }

    if (render_device && matching_adapter) {
        auto interop = std::make_shared<AmfD3D11InteropState>();
        if (interop->initialize(device_.Get(), device_context_.Get(), render_device)) {
            interop_ = std::move(interop);
            d3d12_device_ = render_device;
            native_d3d12_output_ = true;
        } else {
            LOG_WARN("AMF DX11/DX12 interop initialization failed; using host NV12 output");
        }
    }

    const wchar_t* comp_id = decoder_component_id(codec);
    res = factory_->CreateComponent(context_, comp_id, &decoder_);
    if (res != AMF_OK || !decoder_) {
        LOG_ERROR("Decoder CreateComponent({}) failed: {}",
                  codec_name(codec), (int)res);
        context_->Release();
        context_ = nullptr;
        return false;
    }

    res = decoder_->SetProperty(AMF_VIDEO_DECODER_REORDER_MODE,
        static_cast<amf_int64>(AMF_VIDEO_DECODER_MODE_LOW_LATENCY));
    if (res != AMF_OK)
        LOG_WARN("AMF low-latency reorder mode is unavailable: {}", static_cast<int>(res));
    res = decoder_->SetProperty(AMF_VIDEO_DECODER_LOW_LATENCY, true);
    if (res != AMF_OK)
        LOG_WARN("AMF low-latency decode is unavailable: {}", static_cast<int>(res));

    uint32_t w = width > 0 ? width : 1920;
    uint32_t h = height > 0 ? height : 1080;

    res = decoder_->Init(amf::AMF_SURFACE_NV12, w, h);
    if (res != AMF_OK) {
        LOG_ERROR("Decoder Init({}, {}x{}) failed: {}",
                  codec_name(codec), w, h, (int)res);
        decoder_->Release();
        decoder_ = nullptr;
        context_->Release();
        context_ = nullptr;
        return false;
    }

    codec_ = codec;
    width_ = w;
    height_ = h;

    LOG_INFO("Initialized {} AMF decoder ({}x{}, {} output)",
             codec_name(codec), w, h,
             native_d3d12_output_ ? "shared DX11/DX12 RGBA" : "host NV12");
    if (native_d3d12_output_) {
        native_lifetime_ = std::make_shared<AmfNativeLifetime>();
        native_lifetime_->decoder = decoder_;
        native_lifetime_->context = context_;
        native_lifetime_->device = device_;
    }
    initialized_ = true;
    return true;
}

bool AmfDecoder::decode(const uint8_t* data, size_t len, int64_t timestamp) {
    ZoneScopedN("AmfDecoder::decode");
    if (!initialized_ || context_lost_) return false;
    if (!data || len == 0) return false;
    if (!decoder_ || !context_ || !check_device_health())
        return false;

    amf::AMFBuffer* buffer = nullptr;
    AMF_RESULT res = context_->AllocBuffer(amf::AMF_MEMORY_HOST, len, &buffer);
    if (res != AMF_OK || !buffer) {
        LOG_ERROR("AllocBuffer failed: {}", (int)res);
        if (res == AMF_FAIL || res == AMF_NOT_INITIALIZED)
            context_lost_ = true;
        return false;
    }

    void* native = buffer->GetNative();
    if (!native) {
        LOG_ERROR("AMFBuffer::GetNative() returned null");
        buffer->Release();
        return false;
    }

    std::memcpy(native, data, len);
    buffer->SetPts(timestamp);

    // AMF explicitly requires retrying the *same* input when the queue or
    // decoder surface pool is full. Releasing it here loses a compressed frame
    // (often a reference frame) and was the source of the later driver AV that
    // the old SEH wrappers merely hid.
    constexpr int max_backpressure_retries = 64;
    bool submit_without_input = false;
    bool accepted = false;
    for (int retry = 0; retry < max_backpressure_retries; ++retry) {
        res = decoder_->SubmitInput(submit_without_input ? nullptr : buffer);
        switch (classify_amf_submit_result(res)) {
        case AmfSubmitDisposition::Accepted:
            accepted = true;
            retry = max_backpressure_retries;
            break;
        case AmfSubmitDisposition::RetryWithoutInput:
            // AMF_REPEAT means the accepted compressed buffer contains more
            // than one frame. Continue the component with a null input.
            submit_without_input = true;
            poll_output(true);
            break;
        case AmfSubmitDisposition::RetrySameInput: {
            const size_t produced = poll_output(true);
            if (produced == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            break;
        }
        case AmfSubmitDisposition::ResolutionChanged:
            LOG_WARN("AMF decoder reported a resolution change; recreating it at the packet boundary");
            context_lost_ = true;
            retry = max_backpressure_retries;
            break;
        case AmfSubmitDisposition::Fatal:
            mark_context_lost("SubmitInput", res);
            retry = max_backpressure_retries;
            break;
        }
    }
    buffer->Release();

    if (!accepted) {
        if (!context_lost_)
            LOG_WARN("AMF decoder remained backpressured after {} retries", max_backpressure_retries);
        return false;
    }

    poll_output(true);
    return !context_lost_;
}

bool AmfDecoder::deliver_output(amf::AMFData* data) {
    if (!data) return false;

    amf::AMFSurface* surface = nullptr;
    const AMF_RESULT query_result = data->QueryInterface(
        amf::AMFSurface::IID(), reinterpret_cast<void**>(&surface));
    if (query_result != AMF_OK || !surface) {
        LOG_ERROR("AMF decoder output is not a surface: {}", static_cast<int>(query_result));
        return false;
    }

    if (native_d3d12_output_) {
        amf::AMFPlane* packed = surface->GetPlane(amf::AMF_PLANE_PACKED);
        auto* resource = packed ? static_cast<ID3D11Texture2D*>(packed->GetNative()) : nullptr;
        if (!resource) {
            LOG_ERROR("AMF DX11 decoder returned no packed native resource");
            surface->Release();
            return false;
        }
        D3D11_TEXTURE2D_DESC desc{};
        resource->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_NV12) {
            LOG_ERROR("AMF DX11 decoder returned unexpected format {}", static_cast<int>(desc.Format));
            surface->Release();
            return false;
        }

        // D3D12 decoder allocations are commonly padded (for example 180
        // visible lines in a 192-line resource). AMFPlane reports the visible
        // image while ID3D12Resource::GetDesc reports the allocation extent.
        width_ = static_cast<uint32_t>(packed->GetWidth());
        height_ = static_cast<uint32_t>(packed->GetHeight());
        if (width_ == 0 || height_ == 0 || width_ > desc.Width || height_ > desc.Height) {
            LOG_ERROR("AMF DX11 decoder returned invalid visible extent {}x{} in {}x{} resource",
                      width_, height_, desc.Width, desc.Height);
            surface->Release();
            return false;
        }
        if (!on_decoded) {
            surface->Release();
            return true;
        }

        const auto slot_result = interop_
            ? interop_->create_slots(resource, width_, height_)
            : AmfD3D11InteropState::SlotCreationResult::Failed;
        if (slot_result == AmfD3D11InteropState::SlotCreationResult::Busy) {
            // A resolution change cannot replace resources still referenced by
            // a renderer back buffer. Drop this display callback and retry once
            // those owners retire instead of stalling the decode queue.
            surface->Release();
            return true;
        }
        if (slot_result == AmfD3D11InteropState::SlotCreationResult::Ready) {
            auto* slot = interop_->acquire();
            if (!slot) {
                // Every shared texture is still referenced by a renderer frame.
                // Drop instead of blocking decode and growing live latency.
                surface->Release();
                return true;
            }
            uint64_t fence_value = 0;
            if (!interop_->copy(resource, *slot, fence_value)) {
                slot->in_use.store(false, std::memory_order_release);
                surface->Release();
                mark_context_lost("DX11/DX12 shared RGBA conversion", AMF_DIRECTX_FAILED);
                return false;
            }

            DecodedFrame frame{};
            frame.width = width_;
            frame.height = height_;
            frame.timestamp = data->GetPts();
            frame.nv12 = true;
            frame.native_d3d12_resource = slot->d3d12_texture.Get();
            frame.native_d3d12_fence = interop_->d3d12_fence.Get();
            frame.native_fence_value = fence_value;
            frame.native_d3d12_state = static_cast<uint32_t>(D3D12_RESOURCE_STATE_COMMON);
            frame.native_rgba = true;
            frame.native_owner = interop_->make_owner(slot, surface, native_lifetime_);
            on_decoded(frame);
            return true;
        }

        // Interop is optional. Disable it after a permanent allocation or
        // format failure and deliver this same decoded surface through the
        // portable host-NV12 path below instead of dropping every future frame.
        LOG_WARN("AMF shared RGBA interop unavailable; switching to host NV12 output");
        native_d3d12_output_ = false;
        interop_.reset();
        d3d12_device_.Reset();
    }

    const AMF_RESULT convert_result = surface->Convert(amf::AMF_MEMORY_HOST);
    if (convert_result != AMF_OK) {
        LOG_ERROR("AMF decoder GPU-to-host conversion failed: {}", static_cast<int>(convert_result));
        surface->Release();
        return false;
    }

    amf::AMFPlane* y_plane = surface->GetPlane(amf::AMF_PLANE_Y);
    amf::AMFPlane* uv_plane = surface->GetPlane(amf::AMF_PLANE_UV);
    if (!y_plane || !uv_plane) {
        LOG_ERROR("AMF decoder returned an incomplete NV12 surface");
        surface->Release();
        return false;
    }

    width_ = static_cast<uint32_t>(y_plane->GetWidth());
    height_ = static_cast<uint32_t>(y_plane->GetHeight());
    if (on_decoded) {
        DecodedFrame frame{};
        frame.y_plane = static_cast<const uint8_t*>(y_plane->GetNative());
        frame.u_plane = static_cast<const uint8_t*>(uv_plane->GetNative());
        frame.v_plane = nullptr;
        frame.y_stride = static_cast<uint32_t>(y_plane->GetHPitch());
        frame.uv_stride = static_cast<uint32_t>(uv_plane->GetHPitch());
        frame.width = width_;
        frame.height = height_;
        frame.timestamp = data->GetPts();
        frame.nv12 = true;
        on_decoded(frame);
    }

    surface->Release();
    return true;
}

size_t AmfDecoder::poll_output(bool deliver_frames) {
    size_t produced = 0;
    while (decoder_ && !context_lost_) {
        amf::AMFData* data = nullptr;
        const AMF_RESULT result = decoder_->QueryOutput(&data);
        if (result == AMF_OK && data) {
            if (!deliver_frames || deliver_output(data)) ++produced;
            data->Release();
            continue;
        }
        if (data) data->Release();
        if (result == AMF_OK || result == AMF_REPEAT ||
            result == AMF_NEED_MORE_INPUT || result == AMF_EOF) {
            break;
        }
        if (result == AMF_RESOLUTION_CHANGED) {
            LOG_WARN("AMF output resolution changed; decoder recreation requested");
            context_lost_ = true;
            break;
        }
        mark_context_lost("QueryOutput", result);
        break;
    }
    return produced;
}

void AmfDecoder::flush() {
    if (!initialized_ || !decoder_ || context_lost_) return;
    if (!check_device_health()) return;

    // Flush discards queued pictures while keeping component configuration.
    // Drain+ReInit is an end-of-stream transition and races the next network
    // packet against asynchronous decoder teardown on some AMD drivers.
    const AMF_RESULT res = decoder_->Flush();
    if (res != AMF_OK) {
        mark_context_lost("Flush", res);
    }
}

DecoderInfo AmfDecoder::info() const {
    return {Backend::AMF, codec_};
}

} // namespace parties::encdec::amd
