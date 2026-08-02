#include "amd/amf_decoder.h"
#include "openh264/openh264_encoder.h"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <iostream>
#include <vector>

using Microsoft::WRL::ComPtr;
using parties::VideoCodecId;
using parties::encdec::amd::AmfDecoder;
using parties::encdec::openh264::OpenH264Encoder;

namespace {

constexpr uint32_t width = 320;
constexpr uint32_t height = 180;
constexpr uint32_t frame_count = 24;

struct EncodedFrame {
    std::vector<uint8_t> bytes;
    bool keyframe = false;
};

bool wait_for_fence(ID3D12Fence* fence, uint64_t value) {
    if (fence->GetCompletedValue() >= value) return true;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return false;
    const bool result = SUCCEEDED(fence->SetEventOnCompletion(value, event)) &&
        WaitForSingleObject(event, 10'000) == WAIT_OBJECT_0;
    CloseHandle(event);
    return result;
}

} // namespace

int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL feature_level{};
    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
        &device, &feature_level, &context);
    if (FAILED(hr)) {
        std::cerr << "D3D11 device creation failed: " << std::hex << hr << '\n';
        return 1;
    }

    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&texture_desc, nullptr, &texture))) {
        std::cerr << "Source texture creation failed\n";
        return 1;
    }

    OpenH264Encoder encoder;
    if (!encoder.init(device.Get(), width, height, 30, 1'500'000)) {
        std::cerr << "OpenH264 fixture encoder initialization failed\n";
        return 1;
    }

    std::vector<EncodedFrame> encoded;
    encoder.on_encoded = [&encoded](const uint8_t* data, size_t size, bool keyframe) {
        encoded.push_back({std::vector<uint8_t>(data, data + size), keyframe});
    };

    std::vector<uint32_t> bgra(static_cast<size_t>(width) * height);
    for (uint32_t frame = 0; frame < frame_count; ++frame) {
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const uint8_t red = static_cast<uint8_t>((x + frame * 7) & 0xff);
                const uint8_t green = static_cast<uint8_t>((y * 2 + frame * 3) & 0xff);
                const uint8_t blue = static_cast<uint8_t>((x + y + frame * 5) & 0xff);
                bgra[static_cast<size_t>(y) * width + x] =
                    0xff000000u | (static_cast<uint32_t>(red) << 16) |
                    (static_cast<uint32_t>(green) << 8) | blue;
            }
        }
        context->UpdateSubresource(texture.Get(), 0, nullptr, bgra.data(), width * 4, 0);
        if (!encoder.encode(texture.Get(), static_cast<int64_t>(frame) * 333'333)) {
            std::cerr << "Fixture encode failed at frame " << frame << '\n';
            return 1;
        }
    }
    if (encoded.empty() || !encoded.front().keyframe) {
        std::cerr << "Fixture encoder produced no initial IDR\n";
        return 1;
    }

    {
        AmfDecoder decoder;
        if (!decoder.init(VideoCodecId::H264, width, height, nullptr)) {
            // CTest treats 77 as skipped. This keeps the deterministic suite green
            // on machines without AMD hardware while exercising real AMF on AMD CI.
            std::cout << "AMF hardware is unavailable; skipping\n";
            return 77;
        }

        uint32_t decoded = 0;
        uint64_t luma_checksum = 0;
        decoder.on_decoded = [&decoded, &luma_checksum](const parties::encdec::DecodedFrame& frame) {
            if (!frame.y_plane || frame.width != width || frame.height != height)
                return;
            ++decoded;
            for (uint32_t y = 0; y < frame.height; y += 16)
                luma_checksum += frame.y_plane[static_cast<size_t>(y) * frame.y_stride];
        };

        for (size_t index = 0; index < encoded.size(); ++index) {
            const auto& frame = encoded[index];
            if (!decoder.decode(frame.bytes.data(), frame.bytes.size(),
                                static_cast<int64_t>(index) * 333'333)) {
                std::cerr << "AMF host decode failed at access unit " << index << '\n';
                return 1;
            }
        }

        if (decoded < frame_count / 2 || luma_checksum == 0) {
            std::cerr << "AMF produced too few valid host frames: " << decoded << '\n';
            return 1;
        }
        std::cout << "AMF host decoded " << decoded << " frames, checksum="
                  << luma_checksum << '\n';
    }

    ComPtr<IDXGIFactory1> dxgi_factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory)))) {
        std::cerr << "DXGI factory creation failed\n";
        return 1;
    }
    ComPtr<IDXGIAdapter1> amd_adapter;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        if (dxgi_factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 desc{};
        candidate->GetDesc1(&desc);
        if (desc.VendorId == 0x1002) {
            amd_adapter = std::move(candidate);
            break;
        }
    }
    if (!amd_adapter) {
        std::cout << "AMD DX12 adapter is unavailable; native path skipped\n";
        return 0;
    }

    ComPtr<ID3D12Device> d3d12_device;
    if (FAILED(D3D12CreateDevice(amd_adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&d3d12_device)))) {
        std::cerr << "AMD D3D12 device creation failed\n";
        return 1;
    }

    uint32_t native_decoded = 0;
    uint32_t native_callbacks = 0;
    std::shared_ptr<void> retained_native_surface;
    ID3D12Resource* retained_resource = nullptr;
    ID3D12Fence* retained_ready_fence = nullptr;
    uint64_t retained_ready_value = 0;
    uint32_t retained_resource_state = 0;
    {
        AmfDecoder native_decoder;
        if (!native_decoder.init(VideoCodecId::H264, width, height, d3d12_device.Get())) {
            std::cerr << "AMF native D3D12 decoder initialization failed\n";
            return 1;
        }
        native_decoder.on_decoded = [&native_decoded, &native_callbacks, &retained_native_surface,
                                     &retained_resource, &retained_ready_fence,
                                     &retained_ready_value, &retained_resource_state](
                                        const parties::encdec::DecodedFrame& frame) {
        ++native_callbacks;
        auto* resource = static_cast<ID3D12Resource*>(frame.native_d3d12_resource);
        if (!resource || !frame.native_owner || frame.y_plane || !frame.nv12) {
            if (native_callbacks == 1)
                std::cerr << "Invalid native frame contract: resource=" << resource
                          << " owner=" << static_cast<bool>(frame.native_owner)
                          << " y=" << static_cast<const void*>(frame.y_plane)
                          << " nv12=" << frame.nv12 << '\n';
            return;
        }
        const D3D12_RESOURCE_DESC desc = resource->GetDesc();
        if (native_callbacks == 1)
            std::cerr << "Native resource format=" << static_cast<int>(desc.Format)
                      << " size=" << desc.Width << 'x' << desc.Height
                      << " frame=" << frame.width << 'x' << frame.height << '\n';
        auto* producer_fence = static_cast<ID3D12Fence*>(frame.native_d3d12_fence);
        if ((desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
             desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) &&
            frame.native_rgba && frame.width == width && frame.height == height &&
            producer_fence && frame.native_fence_value > 0 &&
            frame.native_d3d12_state == D3D12_RESOURCE_STATE_COMMON) {
            ++native_decoded;
            if (!retained_native_surface) {
                retained_native_surface = frame.native_owner;
                retained_resource = resource;
                retained_ready_fence = static_cast<ID3D12Fence*>(frame.native_d3d12_fence);
                retained_ready_value = frame.native_fence_value;
                retained_resource_state = frame.native_d3d12_state;
            }
        }
        };
        for (size_t index = 0; index < encoded.size(); ++index) {
            const auto& frame = encoded[index];
            if (!native_decoder.decode(frame.bytes.data(), frame.bytes.size(),
                                       static_cast<int64_t>(index) * 333'333)) {
                std::cerr << "AMF native decode failed at access unit " << index << '\n';
                return 1;
            }
        }
    }
    // Decoder teardown must remain safe while the renderer still retains the
    // submitted AMF surface for an in-flight back buffer. Exercise the shared
    // producer fence on a fresh consumer queue after decoder teardown.
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    const HRESULT queue_result = d3d12_device->CreateCommandQueue(
        &queue_desc, IID_PPV_ARGS(&queue));
    const HRESULT allocator_result = d3d12_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    const HRESULT list_result = SUCCEEDED(allocator_result)
        ? d3d12_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          allocator.Get(), nullptr, IID_PPV_ARGS(&list))
        : E_FAIL;
    if (!retained_resource || !retained_ready_fence ||
        FAILED(queue_result) || FAILED(allocator_result) || FAILED(list_result)) {
        std::cerr << "AMF DX11/DX12 interop object creation failed\n";
        return 1;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total_bytes = 0;
    const auto native_desc = retained_resource->GetDesc();
    d3d12_device->GetCopyableFootprints(&native_desc, 0, 1, 0, &footprint,
                                        &rows, &row_bytes, &total_bytes);
    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readback_desc{};
    readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_desc.Width = total_bytes;
    readback_desc.Height = 1;
    readback_desc.DepthOrArraySize = 1;
    readback_desc.MipLevels = 1;
    readback_desc.SampleDesc.Count = 1;
    readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(d3d12_device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
        return 1;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = retained_resource;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    const HRESULT close_result = list->Close();
    const HRESULT wait_result = queue && retained_ready_fence
        ? queue->Wait(retained_ready_fence, retained_ready_value)
        : E_FAIL;
    if (FAILED(wait_result) || FAILED(close_result)) {
        std::cerr << "AMF DX11/DX12 interop setup failed: resource=" << retained_resource
                  << " ready=" << retained_ready_fence
                  << " queue=0x" << std::hex << queue_result
                  << " allocator=0x" << allocator_result << " list=0x" << list_result
                  << " wait=0x" << wait_result << " close=0x" << close_result
                  << " state=0x" << retained_resource_state
                  << " removed=0x" << d3d12_device->GetDeviceRemovedReason()
                  << " fence=" << std::dec << retained_ready_value << '\n';
        return 1;
    }
    ID3D12CommandList* command_lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, command_lists);
    ComPtr<ID3D12Fence> consumer_fence;
    if (FAILED(d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                         IID_PPV_ARGS(&consumer_fence))) ||
        FAILED(queue->Signal(consumer_fence.Get(), 1)) ||
        !wait_for_fence(consumer_fence.Get(), 1) ||
        FAILED(d3d12_device->GetDeviceRemovedReason())) {
        std::cerr << "AMF native DX12 conversion execution failed\n";
        return 1;
    }
    void* mapped = nullptr;
    D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_bytes)};
    if (FAILED(readback->Map(0, &read_range, &mapped))) return 1;
    uint64_t rgba_checksum = 0;
    const auto* pixels = static_cast<const uint8_t*>(mapped) + footprint.Offset;
    for (uint32_t row = 0; row < height; row += 12) {
        const auto* scanline = pixels + static_cast<size_t>(row) * footprint.Footprint.RowPitch;
        for (uint32_t column = 0; column < width; column += 12)
            rgba_checksum += scanline[column * 4] + scanline[column * 4 + 1] +
                             scanline[column * 4 + 2];
    }
    D3D12_RANGE written{0, 0};
    readback->Unmap(0, &written);
    if (rgba_checksum == 0) {
        std::cerr << "AMF shared RGBA texture is empty\n";
        return 1;
    }
    retained_native_surface.reset();
    if (native_decoded < frame_count / 2) {
        std::cerr << "AMF produced too few native D3D12 frames: " << native_decoded
                  << " from " << native_callbacks << " callbacks\n";
        return 1;
    }
    std::cout << "AMF native D3D12 decoded " << native_decoded
              << " frames, RGBA checksum=" << rgba_checksum << "\n";
    return 0;
}
