#include "nvidia/nvdec_decoder.h"
#include "nvidia/nvenc_encoder.h"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <vector>

using Microsoft::WRL::ComPtr;
using parties::VideoCodecId;
using parties::encdec::nvidia::NvdecDecoder;
using parties::encdec::nvidia::NvencEncoder;

namespace {
constexpr uint32_t width = 1920;
constexpr uint32_t height = 1080;
constexpr VideoCodecId test_codec = VideoCodecId::AV1;
// The default crosses a recurring keyframe boundary. CTest also invokes this
// executable with one frame to validate the initial surface independently.
constexpr uint32_t default_frame_count = 31;

struct EncodedFrame {
    std::vector<uint8_t> bytes;
};

bool wait_for_fence(ID3D12Fence* fence, uint64_t value) {
    if (!fence) return false;
    if (fence->GetCompletedValue() >= value) return true;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return false;
    const HRESULT result = fence->SetEventOnCompletion(value, event);
    const bool completed = SUCCEEDED(result) && WaitForSingleObject(event, 10'000) == WAIT_OBJECT_0;
    CloseHandle(event);
    return completed;
}
} // namespace

int main(int argc, char** argv) {
    const uint32_t frame_count = argc > 1
        ? (std::max)(1u, static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10)))
        : default_frame_count;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 1;
    ComPtr<IDXGIAdapter1> nvidia_adapter;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 adapter_desc{};
        candidate->GetDesc1(&adapter_desc);
        if (adapter_desc.VendorId == 0x10de) {
            nvidia_adapter = std::move(candidate);
            break;
        }
    }
    if (!nvidia_adapter) {
        std::cout << "NVIDIA adapter unavailable; skipping\n";
        return 77;
    }

    ComPtr<ID3D11Device> d3d11_device;
    ComPtr<ID3D11DeviceContext> d3d11_context;
    D3D_FEATURE_LEVEL feature_level{};
    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nvidia_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
            levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
            &d3d11_device, &feature_level, &d3d11_context)))
        return 1;

    D3D11_TEXTURE2D_DESC input_desc{};
    input_desc.Width = width;
    input_desc.Height = height;
    input_desc.MipLevels = 1;
    input_desc.ArraySize = 1;
    input_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    input_desc.SampleDesc.Count = 1;
    input_desc.Usage = D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Texture2D> input;
    if (FAILED(d3d11_device->CreateTexture2D(&input_desc, nullptr, &input))) return 1;

    NvencEncoder encoder;
    if (!encoder.init(d3d11_device.Get(), width, height, 30, 1'500'000,
                      test_codec) || encoder.info().codec != test_codec) {
        std::cout << "Requested NVENC codec unavailable; skipping\n";
        return 77;
    }
    std::vector<EncodedFrame> encoded;
    encoder.on_encoded = [&encoded](const uint8_t* data, size_t size, bool) {
        encoded.push_back({std::vector<uint8_t>(data, data + size)});
    };
    std::vector<uint32_t> bgra(static_cast<size_t>(width) * height);
    for (uint32_t frame = 0; frame < frame_count; ++frame) {
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const uint8_t red = static_cast<uint8_t>((x + frame * 11) & 0xff);
                const uint8_t green = static_cast<uint8_t>((y * 2 + frame * 7) & 0xff);
                const uint8_t blue = static_cast<uint8_t>((x + y + frame * 3) & 0xff);
                bgra[static_cast<size_t>(y) * width + x] =
                    0xff000000u | (static_cast<uint32_t>(red) << 16) |
                    (static_cast<uint32_t>(green) << 8) | blue;
            }
        }
        d3d11_context->UpdateSubresource(input.Get(), 0, nullptr, bgra.data(), width * 4, 0);
        if (!encoder.encode(input.Get(), static_cast<int64_t>(frame) * 333'333)) return 1;
    }
    if (encoded.empty()) return 1;

    ComPtr<ID3D12Device> d3d12_device;
    if (FAILED(D3D12CreateDevice(nvidia_adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                IID_PPV_ARGS(&d3d12_device))))
        return 1;

    ComPtr<ID3D12Resource> decoded_texture;
    ComPtr<ID3D12Resource> decoded_chroma_texture;
    ComPtr<ID3D12Fence> cuda_ready_fence;
    uint64_t cuda_ready_value = 0;
    std::shared_ptr<void> decoded_owner;
    uint32_t decoded_frames = 0;
    bool decoded_rgba = false;
    NvdecDecoder decoder;
    if (!decoder.init(test_codec, width, height, d3d12_device.Get())) {
        std::cout << "NVDEC unavailable; skipping\n";
        return 77;
    }
    decoder.on_decoded = [&](const parties::encdec::DecodedFrame& frame) {
            auto* resource = static_cast<ID3D12Resource*>(frame.native_d3d12_resource);
            auto* chroma = static_cast<ID3D12Resource*>(frame.native_d3d12_chroma_resource);
            auto* fence = static_cast<ID3D12Fence*>(frame.native_d3d12_fence);
            if (!resource || !fence || !frame.native_owner || frame.y_plane ||
                frame.width != width || frame.height != height)
                return;
            const auto desc = resource->GetDesc();
            const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM && frame.native_rgba;
            const bool nv12 = (desc.Format == DXGI_FORMAT_R8_UNORM ||
                desc.Format == DXGI_FORMAT_NV12) && chroma &&
                chroma->GetDesc().Format == DXGI_FORMAT_R8G8_UNORM &&
                !frame.native_rgba && frame.nv12;
            if (!rgba && !nv12) return;
            decoded_texture = resource;
            decoded_chroma_texture = chroma;
            cuda_ready_fence = fence;
            cuda_ready_value = frame.native_fence_value;
            decoded_owner = frame.native_owner;
            decoded_rgba = rgba;
            ++decoded_frames;
    };
    for (size_t index = 0; index < encoded.size(); ++index) {
        if (!decoder.decode(encoded[index].bytes.data(), encoded[index].bytes.size(),
                            static_cast<int64_t>(index) * 333'333)) {
            std::cerr << "NVDEC rejected encoded packet " << index << '/' << encoded.size() << '\n';
            return 1;
        }
    }
    decoder.flush();

    if (!decoded_texture || decoded_frames < frame_count ||
        !wait_for_fence(cuda_ready_fence.Get(), cuda_ready_value)) {
        std::cerr << "NVDEC interop produced no synchronized native frames\n";
        return 1;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(d3d12_device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))) ||
        FAILED(d3d12_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&allocator))) ||
        FAILED(d3d12_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
        return 1;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2]{};
    UINT rows[2]{};
    UINT64 row_bytes[2]{};
    UINT64 total_bytes = 0;
    const auto texture_desc = decoded_texture->GetDesc();
    const UINT subresource_count = decoded_rgba ? 1u : 2u;
    const bool split_nv12 = !decoded_rgba && decoded_chroma_texture;
    if (split_nv12) {
        UINT64 luma_bytes = 0;
        d3d12_device->GetCopyableFootprints(&texture_desc, 0, 1, 0,
                                            &footprints[0], &rows[0], &row_bytes[0],
                                            &luma_bytes);
        const UINT64 chroma_offset = (luma_bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
            ~(static_cast<UINT64>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) - 1);
        const auto chroma_desc = decoded_chroma_texture->GetDesc();
        UINT64 chroma_bytes = 0;
        d3d12_device->GetCopyableFootprints(&chroma_desc, 0, 1, chroma_offset,
                                            &footprints[1], &rows[1], &row_bytes[1],
                                            &chroma_bytes);
        total_bytes = chroma_offset + chroma_bytes;
    } else {
        d3d12_device->GetCopyableFootprints(&texture_desc, 0, subresource_count, 0,
                                            footprints, rows, row_bytes, &total_bytes);
    }
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

    for (UINT subresource = 0; subresource < subresource_count; ++subresource) {
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = split_nv12 && subresource == 1
            ? decoded_chroma_texture.Get() : decoded_texture.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = split_nv12 && subresource == 1 ? 0 : subresource;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprints[subresource];
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    if (FAILED(list->Close())) return 1;
    if (FAILED(queue->Wait(cuda_ready_fence.Get(), cuda_ready_value))) return 1;
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> copy_fence;
    if (FAILED(d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                        IID_PPV_ARGS(&copy_fence))) ||
        FAILED(queue->Signal(copy_fence.Get(), 1)) || !wait_for_fence(copy_fence.Get(), 1))
        return 1;

    void* mapped_data = nullptr;
    D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_bytes)};
    if (FAILED(readback->Map(0, &read_range, &mapped_data)))
        return 1;
    const auto* mapped = static_cast<const uint8_t*>(mapped_data);
    uint64_t checksum = 0;
    double y_mean = 0.0;
    double u_mean = 0.0;
    double v_mean = 0.0;
    double uv_zero_ratio = 0.0;
    if (decoded_rgba) {
        for (uint32_t y = 0; y < height; y += 8) {
            const uint8_t* row = mapped + footprints[0].Offset +
                static_cast<size_t>(y) * footprints[0].Footprint.RowPitch;
            for (uint32_t x = 0; x < width; x += 8)
                checksum += row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2];
        }
    } else {
        uint64_t y_sum = 0;
        uint64_t u_sum = 0;
        uint64_t v_sum = 0;
        uint64_t uv_zero_count = 0;
        for (uint32_t y = 0; y < height; y += 8) {
            const uint8_t* row = mapped + footprints[0].Offset +
                static_cast<size_t>(y) * footprints[0].Footprint.RowPitch;
            for (uint32_t x = 0; x < width; x += 8)
                checksum += row[x];
        }
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* row = mapped + footprints[0].Offset +
                static_cast<size_t>(y) * footprints[0].Footprint.RowPitch;
            for (uint32_t x = 0; x < width; ++x)
                y_sum += row[x];
        }
        for (uint32_t y = 0; y < height / 2; y += 8) {
            const uint8_t* row = mapped + footprints[1].Offset +
                static_cast<size_t>(y) * footprints[1].Footprint.RowPitch;
            for (uint32_t x = 0; x < width; x += 8)
                checksum += row[x] + row[x + 1];
        }
        for (uint32_t y = 0; y < height / 2; ++y) {
            const uint8_t* row = mapped + footprints[1].Offset +
                static_cast<size_t>(y) * footprints[1].Footprint.RowPitch;
            for (uint32_t x = 0; x < width; x += 2) {
                u_sum += row[x];
                v_sum += row[x + 1];
                uv_zero_count += row[x] == 0;
                uv_zero_count += row[x + 1] == 0;
            }
        }
        y_mean = static_cast<double>(y_sum) / (static_cast<uint64_t>(width) * height);
        u_mean = static_cast<double>(u_sum) / (static_cast<uint64_t>(width / 2) * (height / 2));
        v_mean = static_cast<double>(v_sum) / (static_cast<uint64_t>(width / 2) * (height / 2));
        uv_zero_ratio = static_cast<double>(uv_zero_count) /
            (static_cast<uint64_t>(width) * (height / 2));
    }
    D3D12_RANGE written{0, 0};
    readback->Unmap(0, &written);
    decoded_owner.reset();

    if (checksum == 0) {
        std::cerr << "NVDEC shared texture is empty\n";
        return 1;
    }
    if (!decoded_rgba && (y_mean < 16.0 || y_mean > 235.0 ||
        u_mean < 16.0 || v_mean < 16.0 || uv_zero_ratio > 0.10)) {
        std::cerr << "NVDEC opaque NV12 chroma output is incomplete: U/V mean="
                  << u_mean << '/' << v_mean << ", zero ratio=" << uv_zero_ratio << '\n';
        return 1;
    }
    std::cout << "NVDEC CUDA/D3D12 interop decoded " << decoded_frames
              << " " << (decoded_rgba ? "fallback RGBA" : "opaque NV12")
              << " frames, checksum=" << checksum;
    if (!decoded_rgba) {
        std::cout << std::fixed << std::setprecision(2)
                  << ", Y/U/V mean=" << y_mean << '/' << u_mean << '/' << v_mean
                  << ", UV zero ratio=" << uv_zero_ratio;
    }
    std::cout << '\n';
    return 0;
}
