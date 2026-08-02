#include "Dx12VideoConverter.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <d3dcompiler.h>

using Microsoft::WRL::ComPtr;

namespace {
constexpr uint32_t descriptor_count_per_frame = 4;
constexpr uint32_t threads = 16;

constexpr char compute_shader[] = R"HLSL(
Texture2D<float4> plane_y  : register(t0);
Texture2D<float4> plane_uv : register(t1);
Texture2D<float4> plane_v  : register(t2);
RWTexture2D<float4> output : register(u0);

cbuffer Parameters : register(b0) {
    uint visible_width;
    uint visible_height;
    uint layout;
    uint reserved;
};

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= visible_width || id.y >= visible_height)
        return;

    uint2 pixel = id.xy;
    uint2 chroma = pixel >> 1;
    float y = plane_y.Load(int3(pixel, 0)).r * 255.0;
    float2 uv;
    if (layout == 0) {
        uv.x = plane_uv.Load(int3(chroma, 0)).r * 255.0;
        uv.y = plane_v.Load(int3(chroma, 0)).r * 255.0;
    } else {
        uv = plane_uv.Load(int3(chroma, 0)).rg * 255.0;
    }

    float c = max(0.0, y - 16.0);
    float d = uv.x - 128.0;
    float e = uv.y - 128.0;
    float3 rgb = float3(
        (298.0 * c + 409.0 * e + 128.0) / 256.0,
        (298.0 * c - 100.0 * d - 208.0 * e + 128.0) / 256.0,
        (298.0 * c + 516.0 * d + 128.0) / 256.0);
    output[pixel] = float4(saturate(rgb / 255.0), 1.0);
}
)HLSL";

uint32_t align_pitch(uint32_t value) {
	return (value + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
		~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource,
	D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	return barrier;
}

ComPtr<ID3D12Resource> create_texture(ID3D12Device* device, uint32_t width,
	uint32_t height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
	D3D12_RESOURCE_STATES initial_state) {
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Flags = flags;
	ComPtr<ID3D12Resource> result;
	if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
		initial_state, nullptr, IID_PPV_ARGS(&result))))
		return {};
	return result;
}
} // namespace

Dx12VideoConverter::Texture::~Texture() {
	auto unmap = [](auto& planes) {
		for (auto& plane : planes) {
			if (plane.mapped && plane.resource)
				plane.resource->Unmap(0, nullptr);
			plane.mapped = nullptr;
		}
	};
	unmap(y_upload);
	unmap(uv_upload);
	unmap(v_upload);
}

bool Dx12VideoConverter::Initialize(ID3D12Device* device) {
	if (!device) return false;
	device_ = device;
	descriptor_size_ = device_->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_DESCRIPTOR_RANGE ranges[2]{};
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors = 3;
	ranges[0].BaseShaderRegister = 0;
	ranges[0].OffsetInDescriptorsFromTableStart = 0;
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	ranges[1].NumDescriptors = 1;
	ranges[1].BaseShaderRegister = 0;
	ranges[1].OffsetInDescriptorsFromTableStart = 0;

	D3D12_ROOT_PARAMETER parameters[3]{};
	parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	parameters[0].DescriptorTable.NumDescriptorRanges = 1;
	parameters[0].DescriptorTable.pDescriptorRanges = &ranges[0];
	parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	parameters[1].DescriptorTable.NumDescriptorRanges = 1;
	parameters[1].DescriptorTable.pDescriptorRanges = &ranges[1];
	parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	parameters[2].Constants.ShaderRegister = 0;
	parameters[2].Constants.Num32BitValues = 4;

	D3D12_ROOT_SIGNATURE_DESC root_desc{};
	root_desc.NumParameters = static_cast<UINT>(std::size(parameters));
	root_desc.pParameters = parameters;
	ComPtr<ID3DBlob> serialized;
	ComPtr<ID3DBlob> errors;
	if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
		&serialized, &errors)) ||
		FAILED(device_->CreateRootSignature(0, serialized->GetBufferPointer(),
			serialized->GetBufferSize(), IID_PPV_ARGS(&root_signature_)))) {
		if (errors) std::fprintf(stderr, "DX12 video root signature: %s\n",
			static_cast<const char*>(errors->GetBufferPointer()));
		return false;
	}

	ComPtr<ID3DBlob> shader;
	errors.Reset();
	if (FAILED(D3DCompile(compute_shader, sizeof(compute_shader) - 1,
		"PartiesVideoConverter", nullptr, nullptr, "main", "cs_5_1",
		D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors))) {
		if (errors) std::fprintf(stderr, "DX12 video shader: %s\n",
			static_cast<const char*>(errors->GetBufferPointer()));
		return false;
	}
	D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
	pipeline_desc.pRootSignature = root_signature_.Get();
	pipeline_desc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
	return SUCCEEDED(device_->CreateComputePipelineState(&pipeline_desc,
		IID_PPV_ARGS(&pipeline_)));
}

std::shared_ptr<Dx12VideoConverter::Texture> Dx12VideoConverter::Create(
	Layout layout, uint32_t width, uint32_t height) {
	if (!*this || width == 0 || height == 0) return {};
	auto texture = std::make_shared<Texture>();
	texture->layout = layout;
	texture->width = width;
	texture->height = height;
	if ((layout != Layout::DirectNV12 && !CreateOutput(*texture)) ||
		(layout != Layout::NativeNV12 && !CreateCpuPlanes(*texture)) ||
		(layout != Layout::DirectNV12 && !CreateDescriptors(*texture)))
		return {};
	return texture;
}

bool Dx12VideoConverter::CreateOutput(Texture& texture) {
	texture.output = create_texture(device_.Get(), texture.width, texture.height,
		DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_COMMON);
	return texture.output != nullptr;
}

bool Dx12VideoConverter::CreateCpuPlanes(Texture& texture) {
	const uint32_t chroma_width = (texture.width + 1) / 2;
	const uint32_t chroma_height = (texture.height + 1) / 2;
	texture.y = create_texture(device_.Get(), texture.width, texture.height,
		DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
	texture.uv = create_texture(device_.Get(), chroma_width, chroma_height,
		texture.layout == Layout::I420 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8_UNORM,
		D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
	if (texture.layout == Layout::I420)
		texture.v = create_texture(device_.Get(), chroma_width, chroma_height,
			DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
	if (!texture.y || !texture.uv || (texture.layout == Layout::I420 && !texture.v))
		return false;

	for (uint32_t frame = 0; frame < frame_count; ++frame) {
		if (!CreateUploadPlane(texture.y_upload[frame], texture.width, texture.height) ||
			!CreateUploadPlane(texture.uv_upload[frame],
				chroma_width * (texture.layout == Layout::I420 ? 1u : 2u), chroma_height) ||
			(texture.layout == Layout::I420 &&
				!CreateUploadPlane(texture.v_upload[frame], chroma_width, chroma_height)))
			return false;
	}
	return true;
}

bool Dx12VideoConverter::CreateUploadPlane(Texture::UploadPlane& upload,
	uint32_t width_bytes, uint32_t height) {
	upload.row_pitch = align_pitch(width_bytes);
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_UPLOAD;
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = static_cast<uint64_t>(upload.row_pitch) * height;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload.resource))))
		return false;
	D3D12_RANGE read_range{0, 0};
	return SUCCEEDED(upload.resource->Map(0, &read_range, &upload.mapped));
}

bool Dx12VideoConverter::CreateDescriptors(Texture& texture) {
	D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
	heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heap_desc.NumDescriptors = descriptor_count_per_frame * frame_count;
	heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&texture.descriptors))))
		return false;

	for (uint32_t frame = 0; frame < frame_count; ++frame) {
		if (texture.layout != Layout::NativeNV12) {
			CreatePlaneSrv(texture, frame, 0, texture.y.Get(), DXGI_FORMAT_R8_UNORM, 0);
			CreatePlaneSrv(texture, frame, 1, texture.uv.Get(),
				texture.layout == Layout::I420 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8_UNORM, 0);
			CreatePlaneSrv(texture, frame, 2, texture.v.Get(), DXGI_FORMAT_R8_UNORM, 0);
		} else {
			CreatePlaneSrv(texture, frame, 0, nullptr, DXGI_FORMAT_R8_UNORM, 0);
			CreatePlaneSrv(texture, frame, 1, nullptr, DXGI_FORMAT_R8G8_UNORM, 1);
			CreatePlaneSrv(texture, frame, 2, nullptr, DXGI_FORMAT_R8_UNORM, 0);
		}

		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		auto destination = texture.descriptors->GetCPUDescriptorHandleForHeapStart();
		destination.ptr += static_cast<SIZE_T>(frame * descriptor_count_per_frame + 3) * descriptor_size_;
		device_->CreateUnorderedAccessView(texture.output.Get(), nullptr, &uav, destination);
	}
	return true;
}

void Dx12VideoConverter::CreatePlaneSrv(Texture& texture, uint32_t frame_index,
	uint32_t descriptor, ID3D12Resource* resource, DXGI_FORMAT format, uint32_t plane_slice) {
	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Format = format;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Texture2D.MipLevels = 1;
	srv.Texture2D.PlaneSlice = plane_slice;
	auto destination = texture.descriptors->GetCPUDescriptorHandleForHeapStart();
	destination.ptr += static_cast<SIZE_T>(frame_index * descriptor_count_per_frame + descriptor) * descriptor_size_;
	device_->CreateShaderResourceView(resource, &srv, destination);
}

void Dx12VideoConverter::WritePlane(Texture::UploadPlane& upload, const uint8_t* source,
	uint32_t source_stride, uint32_t width_bytes, uint32_t height) {
	if (!upload.mapped || !source) return;
	auto* destination = static_cast<uint8_t*>(upload.mapped);
	for (uint32_t row = 0; row < height; ++row)
		std::memcpy(destination + static_cast<size_t>(row) * upload.row_pitch,
			source + static_cast<size_t>(row) * source_stride, width_bytes);
}

void Dx12VideoConverter::CopyPlane(ID3D12GraphicsCommandList* commands,
	ID3D12Resource* destination, D3D12_RESOURCE_STATES& state,
	const Texture::UploadPlane& upload, uint32_t width, uint32_t height) {
	if (state != D3D12_RESOURCE_STATE_COPY_DEST) {
		auto barrier = transition(destination, state, D3D12_RESOURCE_STATE_COPY_DEST);
		commands->ResourceBarrier(1, &barrier);
		state = D3D12_RESOURCE_STATE_COPY_DEST;
	}
	D3D12_TEXTURE_COPY_LOCATION source{};
	source.pResource = upload.resource.Get();
	source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	source.PlacedFootprint.Footprint.Format = destination->GetDesc().Format;
	source.PlacedFootprint.Footprint.Width = width;
	source.PlacedFootprint.Footprint.Height = height;
	source.PlacedFootprint.Footprint.Depth = 1;
	source.PlacedFootprint.Footprint.RowPitch = upload.row_pitch;
	D3D12_TEXTURE_COPY_LOCATION target{};
	target.pResource = destination;
	target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	commands->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
	auto barrier = transition(destination, state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
	commands->ResourceBarrier(1, &barrier);
	state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
}

bool Dx12VideoConverter::UpdateI420(Texture& texture,
	ID3D12GraphicsCommandList* commands, uint32_t frame_index,
	const uint8_t* y, uint32_t y_stride, const uint8_t* u, const uint8_t* v,
	uint32_t uv_stride) {
	if (!commands || texture.layout != Layout::I420 || frame_index >= frame_count)
		return false;
	const uint32_t chroma_width = (texture.width + 1) / 2;
	const uint32_t chroma_height = (texture.height + 1) / 2;
	WritePlane(texture.y_upload[frame_index], y, y_stride, texture.width, texture.height);
	WritePlane(texture.uv_upload[frame_index], u, uv_stride, chroma_width, chroma_height);
	WritePlane(texture.v_upload[frame_index], v, uv_stride, chroma_width, chroma_height);
	CopyPlane(commands, texture.y.Get(), texture.y_state, texture.y_upload[frame_index],
		texture.width, texture.height);
	CopyPlane(commands, texture.uv.Get(), texture.uv_state, texture.uv_upload[frame_index],
		chroma_width, chroma_height);
	CopyPlane(commands, texture.v.Get(), texture.v_state, texture.v_upload[frame_index],
		chroma_width, chroma_height);
	Dispatch(texture, commands, frame_index);
	return true;
}

bool Dx12VideoConverter::UpdateNV12(Texture& texture,
	ID3D12GraphicsCommandList* commands, uint32_t frame_index,
	const uint8_t* y, uint32_t y_stride, const uint8_t* uv, uint32_t uv_stride) {
	if (!commands || (texture.layout != Layout::NV12 && texture.layout != Layout::DirectNV12) ||
		frame_index >= frame_count)
		return false;
	const uint32_t chroma_width = (texture.width + 1) / 2;
	const uint32_t chroma_height = (texture.height + 1) / 2;
	WritePlane(texture.y_upload[frame_index], y, y_stride, texture.width, texture.height);
	WritePlane(texture.uv_upload[frame_index], uv, uv_stride, chroma_width * 2, chroma_height);
	CopyPlane(commands, texture.y.Get(), texture.y_state, texture.y_upload[frame_index],
		texture.width, texture.height);
	CopyPlane(commands, texture.uv.Get(), texture.uv_state, texture.uv_upload[frame_index],
		chroma_width, chroma_height);
	if (texture.layout == Layout::NV12)
		Dispatch(texture, commands, frame_index);
	return true;
}

bool Dx12VideoConverter::UpdateNativeNV12(Texture& texture, ID3D12CommandQueue* queue,
	ID3D12GraphicsCommandList* commands, uint32_t frame_index,
	ID3D12Resource* source, std::shared_ptr<void> owner,
	ID3D12Fence* ready_fence, uint64_t ready_value,
	D3D12_RESOURCE_STATES source_state) {
	if (!queue || !commands || !source || !owner ||
		texture.layout != Layout::NativeNV12 || frame_index >= frame_count ||
		source->GetDesc().Format != DXGI_FORMAT_NV12)
		return false;
	if (ready_fence && ready_value && FAILED(queue->Wait(ready_fence, ready_value)))
		return false;

	// This descriptor group belongs to the back buffer which BeginFrame has
	// already waited for, so replacing it cannot race an older command list.
	CreatePlaneSrv(texture, frame_index, 0, source, DXGI_FORMAT_R8_UNORM, 0);
	CreatePlaneSrv(texture, frame_index, 1, source, DXGI_FORMAT_R8G8_UNORM, 1);
	texture.native_owners[frame_index] = std::move(owner);

	if (source_state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
		auto barrier = transition(source, source_state,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		commands->ResourceBarrier(1, &barrier);
	}
	Dispatch(texture, commands, frame_index);
	if (source_state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
		auto barrier = transition(source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			source_state);
		commands->ResourceBarrier(1, &barrier);
	}
	return true;
}

void Dx12VideoConverter::Dispatch(Texture& texture,
	ID3D12GraphicsCommandList* commands, uint32_t frame_index) {
	if (texture.output_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
		auto barrier = transition(texture.output.Get(), texture.output_state,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commands->ResourceBarrier(1, &barrier);
		texture.output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	}
	commands->SetPipelineState(pipeline_.Get());
	commands->SetComputeRootSignature(root_signature_.Get());
	ID3D12DescriptorHeap* heaps[] = {texture.descriptors.Get()};
	commands->SetDescriptorHeaps(1, heaps);
	auto base = texture.descriptors->GetGPUDescriptorHandleForHeapStart();
	base.ptr += static_cast<UINT64>(frame_index * descriptor_count_per_frame) * descriptor_size_;
	commands->SetComputeRootDescriptorTable(0, base);
	auto output = base;
	output.ptr += static_cast<UINT64>(3) * descriptor_size_;
	commands->SetComputeRootDescriptorTable(1, output);
	const uint32_t parameters[4] = {
		texture.width,
		texture.height,
		texture.layout == Layout::I420 ? 0u : 1u,
		0u,
	};
	commands->SetComputeRoot32BitConstants(2, 4, parameters, 0);
	commands->Dispatch((texture.width + threads - 1) / threads,
		(texture.height + threads - 1) / threads, 1);
	auto barrier = transition(texture.output.Get(), texture.output_state,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	commands->ResourceBarrier(1, &barrier);
	texture.output_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}
