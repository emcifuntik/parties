#include "dx12/Dx12VideoConverter.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
constexpr uint32_t width = 64;
constexpr uint32_t height = 32;

bool wait_for_fence(ID3D12Fence* fence, uint64_t value) {
	if (fence->GetCompletedValue() >= value) return true;
	HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!event) return false;
	const bool result = SUCCEEDED(fence->SetEventOnCompletion(value, event)) &&
		WaitForSingleObject(event, 10'000) == WAIT_OBJECT_0;
	CloseHandle(event);
	return result;
}

std::array<uint8_t, 4> convert(ID3D12Device* device, ID3D12CommandQueue* queue,
	Dx12VideoConverter& converter, Dx12VideoConverter::Layout layout) {
	auto texture = converter.Create(layout, width, height);
	if (!texture) return {};

	ComPtr<ID3D12CommandAllocator> allocator;
	ComPtr<ID3D12GraphicsCommandList> list;
	if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
		IID_PPV_ARGS(&allocator))) ||
		FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
			allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
		return {};

	std::vector<uint8_t> y(static_cast<size_t>(width) * height, 81);
	std::vector<uint8_t> u(static_cast<size_t>(width / 2) * (height / 2), 90);
	std::vector<uint8_t> v(u.size(), 240);
	std::vector<uint8_t> uv(u.size() * 2);
	for (size_t index = 0; index < u.size(); ++index) {
		uv[index * 2] = u[index];
		uv[index * 2 + 1] = v[index];
	}
	const bool updated = layout == Dx12VideoConverter::Layout::I420
		? converter.UpdateI420(*texture, list.Get(), 0, y.data(), width,
			u.data(), v.data(), width / 2)
		: converter.UpdateNV12(*texture, list.Get(), 0, y.data(), width,
			uv.data(), width);
	if (!updated) return {};

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
	UINT rows = 0;
	UINT64 row_bytes = 0;
	UINT64 total_bytes = 0;
	const auto output_desc = texture->output->GetDesc();
	device->GetCopyableFootprints(&output_desc, 0, 1, 0, &footprint,
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
	if (FAILED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
		&readback_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
		IID_PPV_ARGS(&readback))))
		return {};

	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = texture->output.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	list->ResourceBarrier(1, &barrier);
	D3D12_TEXTURE_COPY_LOCATION source{};
	source.pResource = texture->output.Get();
	source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	D3D12_TEXTURE_COPY_LOCATION destination{};
	destination.pResource = readback.Get();
	destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	destination.PlacedFootprint = footprint;
	list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
	if (FAILED(list->Close())) return {};
	ID3D12CommandList* lists[] = {list.Get()};
	queue->ExecuteCommandLists(1, lists);
	ComPtr<ID3D12Fence> fence;
	if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) ||
		FAILED(queue->Signal(fence.Get(), 1)) || !wait_for_fence(fence.Get(), 1))
		return {};

	void* mapped = nullptr;
	D3D12_RANGE range{0, static_cast<SIZE_T>(total_bytes)};
	if (FAILED(readback->Map(0, &range, &mapped))) return {};
	const auto* pixel = static_cast<const uint8_t*>(mapped) + footprint.Offset +
		static_cast<size_t>(height / 2) * footprint.Footprint.RowPitch + width / 2 * 4;
	std::array<uint8_t, 4> result{pixel[0], pixel[1], pixel[2], pixel[3]};
	D3D12_RANGE written{0, 0};
	readback->Unmap(0, &written);
	return result;
}

bool upload_direct_nv12(ID3D12Device* device, ID3D12CommandQueue* queue,
	Dx12VideoConverter& converter) {
	auto texture = converter.Create(Dx12VideoConverter::Layout::DirectNV12, width, height);
	if (!texture || texture->output || !texture->Luma() || !texture->Chroma()) return false;
	if (texture->Luma()->GetDesc().Format != DXGI_FORMAT_R8_UNORM ||
		texture->Chroma()->GetDesc().Format != DXGI_FORMAT_R8G8_UNORM)
		return false;

	ComPtr<ID3D12CommandAllocator> allocator;
	ComPtr<ID3D12GraphicsCommandList> list;
	if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
		IID_PPV_ARGS(&allocator))) ||
		FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
			allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
		return false;

	std::vector<uint8_t> y(static_cast<size_t>(width) * height, 81);
	std::vector<uint8_t> uv(static_cast<size_t>(width) * (height / 2));
	for (size_t index = 0; index < uv.size(); index += 2) {
		uv[index] = 90;
		uv[index + 1] = 240;
	}
	if (!converter.UpdateNV12(*texture, list.Get(), 0, y.data(), width,
		uv.data(), width))
		return false;

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2]{};
	UINT rows[2]{};
	UINT64 row_bytes[2]{};
	UINT64 sizes[2]{};
	const auto y_desc = texture->Luma()->GetDesc();
	const auto uv_desc = texture->Chroma()->GetDesc();
	device->GetCopyableFootprints(&y_desc, 0, 1, 0, &footprints[0],
		&rows[0], &row_bytes[0], &sizes[0]);
	device->GetCopyableFootprints(&uv_desc, 0, 1, 0, &footprints[1],
		&rows[1], &row_bytes[1], &sizes[1]);

	ComPtr<ID3D12Resource> readbacks[2];
	D3D12_HEAP_PROPERTIES readback_heap{};
	readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
	for (size_t index = 0; index < 2; ++index) {
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = sizes[index];
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
			&desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			IID_PPV_ARGS(&readbacks[index]))))
			return false;
	}

	ID3D12Resource* planes[] = {texture->Luma(), texture->Chroma()};
	for (size_t index = 0; index < 2; ++index) {
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = planes[index];
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		list->ResourceBarrier(1, &barrier);

		D3D12_TEXTURE_COPY_LOCATION source{};
		source.pResource = planes[index];
		source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		D3D12_TEXTURE_COPY_LOCATION destination{};
		destination.pResource = readbacks[index].Get();
		destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		destination.PlacedFootprint = footprints[index];
		list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
	}
	if (FAILED(list->Close())) return false;
	ID3D12CommandList* lists[] = {list.Get()};
	queue->ExecuteCommandLists(1, lists);
	ComPtr<ID3D12Fence> fence;
	if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) ||
		FAILED(queue->Signal(fence.Get(), 1)) || !wait_for_fence(fence.Get(), 1))
		return false;

	uint8_t samples[3]{};
	for (size_t index = 0; index < 2; ++index) {
		void* mapped = nullptr;
		D3D12_RANGE range{0, static_cast<SIZE_T>(sizes[index])};
		if (FAILED(readbacks[index]->Map(0, &range, &mapped))) return false;
		const auto* data = static_cast<const uint8_t*>(mapped) + footprints[index].Offset;
		if (index == 0) samples[0] = data[0];
		else { samples[1] = data[0]; samples[2] = data[1]; }
		D3D12_RANGE written{0, 0};
		readbacks[index]->Unmap(0, &written);
	}
	return samples[0] == 81 && samples[1] == 90 && samples[2] == 240;
}
} // namespace

int main() {
	ComPtr<ID3D12Device> device;
	if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
		IID_PPV_ARGS(&device))))
		return 77;
	D3D12_COMMAND_QUEUE_DESC queue_desc{};
	queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	ComPtr<ID3D12CommandQueue> queue;
	if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))))
		return 1;
	Dx12VideoConverter converter;
	if (!converter.Initialize(device.Get())) return 1;

	for (const auto layout : {Dx12VideoConverter::Layout::I420,
		Dx12VideoConverter::Layout::NV12}) {
		const auto pixel = convert(device.Get(), queue.Get(), converter, layout);
		if (pixel[0] < 220 || pixel[1] > 40 || pixel[2] > 40 || pixel[3] < 250) {
			std::cerr << "Unexpected converted pixel: " << static_cast<int>(pixel[0]) << ','
				<< static_cast<int>(pixel[1]) << ',' << static_cast<int>(pixel[2]) << ','
				<< static_cast<int>(pixel[3]) << '\n';
			return 1;
		}
	}
	if (!upload_direct_nv12(device.Get(), queue.Get(), converter)) {
		std::cerr << "Direct planar NV12 upload failed\n";
		return 1;
	}
	std::cout << "DX12 I420/NV12 conversion and direct planar upload passed\n";
	return 0;
}
