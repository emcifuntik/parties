#include "dx12/Parties_Renderer_DX12.h"

#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/MeshUtilities.h>

#include <memory>
#include <array>
#include <vector>
#include <cstdio>

namespace {
constexpr wchar_t kWindowClass[] = L"PartiesRmlUiDx12MultiwindowTest";

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
	return DefWindowProcW(window, message, wparam, lparam);
}

struct GpuDiagnostics {
	Microsoft::WRL::ComPtr<ID3D12InfoQueue> queue;
	bool check() {
		bool passed = true;
		if (!queue) return true;
		for (UINT64 index = 0; index < queue->GetNumStoredMessages(); ++index) {
			SIZE_T size = 0;
			queue->GetMessage(index, nullptr, &size);
			std::vector<unsigned char> bytes(size);
			auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
			if (SUCCEEDED(queue->GetMessage(index, message, &size)) &&
				message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
				std::fprintf(stderr, "D3D12: %s\n", message->pDescription);
				passed = false;
			}
		}
		queue->ClearStoredMessages();
		return passed;
	}
};
} // namespace

int main(int argc, char**) {
	Microsoft::WRL::ComPtr<ID3D12Debug> debug;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
	HINSTANCE instance = GetModuleHandleW(nullptr);
	WNDCLASSEXW window_class{};
	window_class.cbSize = sizeof(window_class);
	window_class.hInstance = instance;
	window_class.lpfnWndProc = WindowProcedure;
	window_class.lpszClassName = kWindowClass;
	if (!RegisterClassExW(&window_class)) return 1;

	HWND first_window = CreateWindowExW(0, kWindowClass, L"RmlUi DX12 first",
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 320, 200,
		nullptr, nullptr, instance, nullptr);
	HWND second_window = CreateWindowExW(0, kWindowClass, L"RmlUi DX12 second",
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 320, 200,
		nullptr, nullptr, instance, nullptr);
	if (!first_window || !second_window) return 2;

	Backend::RmlRendererSettings settings{};
	settings.vsync = false;
	settings.msaa_sample_count = argc > 2 ? 2 : 1;
	auto first = std::make_unique<PartiesRenderInterface_DX12>(first_window, settings);
	auto second = std::make_unique<PartiesRenderInterface_DX12>(second_window, settings);
	if (!*first || !*second) return 3;

	// Repeated minimize/restore notifications can report unchanged dimensions.
	// They must not be implemented as two synthetic ResizeBuffers calls, and a
	// real resize must always leave a complete renderable depth-stencil state.
	first->SetViewport(320, 200);
	if (!first->IsViewportValid()) return 8;
	for (int pass = 0; pass < 256; ++pass) {
		first->SetViewport(320, 200, true);
		if (!first->IsViewportValid()) return 8;
	}
	first->SetViewport(321, 201);
	if (!first->IsViewportValid()) return 8;
	first->SetViewport(320, 200);
	if (!first->IsViewportValid()) return 8;

	// Parties' in-tree backend must be able to bind a packed NV12 resource as
	// two plane SRVs without allocating an intermediate RGBA texture.
	auto* device = static_cast<ID3D12Device*>(first->GetD3D12Device());
	GpuDiagnostics diagnostics;
	device->QueryInterface(IID_PPV_ARGS(&diagnostics.queue));
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;
	D3D12_RESOURCE_DESC nv12_desc{};
	nv12_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	nv12_desc.Width = 64;
	nv12_desc.Height = 64;
	nv12_desc.DepthOrArraySize = 1;
	nv12_desc.MipLevels = 1;
	nv12_desc.Format = DXGI_FORMAT_NV12;
	nv12_desc.SampleDesc.Count = 1;
	Microsoft::WRL::ComPtr<ID3D12Resource> nv12;
	if (!device || FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
		&nv12_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&nv12))))
		return 4;
	auto owner = std::make_shared<int>(1);
	const uintptr_t texture = first->GenerateNativeNV12Texture(
		nv12.Get(), nullptr, owner, nullptr, 0,
		D3D12_RESOURCE_STATE_COMMON, false, 64, 64);
	if (!texture) return 5;
	first->ReleaseNV12Texture(texture);

	if (argc > 1) {
		// Exhaust the shared SRV heap after the resized layer was created but
		// before EndFrame creates its postprocess framebuffer.
		first->BeginFrame();
		first->Clear();
		std::vector<uintptr_t> held;
		while (auto handle = first->GenerateNativeNV12Texture(
			nv12.Get(), nullptr, owner, nullptr, 0,
			D3D12_RESOURCE_STATE_COMMON, false, 64, 64)) {
			held.push_back(handle);
			if (held.size() > 10000) return 9;
		}
		std::printf("Held %zu NV12 descriptors before EndFrame\n", held.size());
		std::fflush(stdout);
		const size_t full_capacity = held.size();
		const std::array<Rml::byte, 4> pixel = {255, 255, 255, 255};
		if (first->GenerateDynamicTexture({pixel.data(), pixel.size()}, {1, 1})) return 11;
		first->EndFrame();
		if (FAILED(device->GetDeviceRemovedReason())) return 10;
		if (!diagnostics.check()) return 12;

		// Leave a small working set for two streams and their retired frames,
		// keeping the rest of the texture descriptor pool occupied during resize.
		for (int index = 0; index < 16; ++index) {
			if (held.empty()) return 13;
			first->ReleaseNV12Texture(held.back());
			held.pop_back();
		}
		std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 8> video_frames;
		for (auto& frame : video_frames) {
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
				&nv12_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&frame)))) return 14;
		}
		std::array<uintptr_t, 2> streams{};
		Rml::Mesh quad;
		Rml::MeshUtilities::GenerateQuad(quad, {0, 0}, {64, 64}, {255, 255, 255, 255});
		const auto geometry = first->CompileGeometry(quad.vertices, quad.indices);
		if (!geometry) return 15;
		for (int pass = 0; pass < 256; ++pass) {
			first->SetViewport(320 + pass % 127, 200 + pass % 71);
			first->BeginFrame();
			if (!first->IsFrameActive()) return 16;
			first->Clear();
			for (size_t stream = 0; stream < streams.size(); ++stream) {
				auto* resource = video_frames[stream * 4 + pass % 4].Get();
				if (!streams[stream]) {
					streams[stream] = first->GenerateNativeNV12Texture(resource, nullptr, owner,
						nullptr, 0, D3D12_RESOURCE_STATE_COMMON, false, 64, 64);
					if (!streams[stream]) return 17;
				} else if (!first->UpdateNativeNV12Texture(streams[stream], resource, nullptr, owner,
					nullptr, 0, D3D12_RESOURCE_STATE_COMMON, false, 64, 64)) return 18;
				first->RenderGeometry(geometry, {10.0f + 80.0f * stream, 10.0f}, streams[stream]);
			}
			first->EndFrame();
			if (FAILED(device->GetDeviceRemovedReason()) || !diagnostics.check()) return 19;
		}
		for (auto stream : streams) first->ReleaseNV12Texture(stream);
		first->ReleaseGeometry(geometry);
		for (auto handle : held) first->ReleaseNV12Texture(handle);
		held.clear();

		// Exhaust RTVs too. Failed layers must remain balanced and the frame
		// must still complete, allowing the next resized frame to recover.
		first->SetViewport(333, 222);
		first->BeginFrame();
		if (!first->IsFrameActive()) return 20;
		first->Clear();
		for (int layer = 0; layer < 16; ++layer) first->PushLayer();
		for (int layer = 0; layer < 16; ++layer) first->PopLayer();
		first->EndFrame();
		for (int pass = 0; pass < 4; ++pass) {
			first->SetViewport(320, 200);
			first->BeginFrame();
			if (!first->IsFrameActive()) return 21;
			first->Clear();
			first->EndFrame();
		}
		if (FAILED(device->GetDeviceRemovedReason()) || !diagnostics.check()) return 22;
		while (auto handle = first->GenerateNativeNV12Texture(nv12.Get(), nullptr, owner,
			nullptr, 0, D3D12_RESOURCE_STATE_COMMON, false, 64, 64)) {
			held.push_back(handle);
			if (held.size() > full_capacity) return 23;
		}
		if (held.size() != full_capacity) return 24;
		for (auto handle : held) first->ReleaseNV12Texture(handle);
		std::printf("256 resizes with two streams passed; descriptor capacity recovered\n");
	}

	// Exercise the RGBA upload path used by application previews. In particular,
	// very narrow thumbnails have less than 64 KiB of pixel data but are not
	// guaranteed to qualify for D3D12's 4 KiB small-resource alignment. The
	// renderer must retry those allocations with the default alignment instead
	// of passing an invalid placed resource to GetRequiredIntermediateSize.
	const std::array<Rml::Vector2i, 8> preview_sizes = {{
		{1, 1}, {3, 17}, {64, 64}, {127, 7},
		{128, 128}, {129, 1}, {320, 8}, {320, 180},
	}};
	for (int pass = 0; pass < 8; ++pass) {
		for (const Rml::Vector2i dimensions : preview_sizes) {
			std::vector<Rml::byte> rgba(
				static_cast<size_t>(dimensions.x) * static_cast<size_t>(dimensions.y) * 4,
				static_cast<Rml::byte>(0x80 + pass));
			const Rml::TextureHandle rgba_texture = first->GenerateDynamicTexture(rgba, dimensions);
			if (!rgba_texture) return 6;
			first->ReleaseTexture(rgba_texture);
		}
	}

	// The two backend instances can share the COM device. Closing either HWND
	// must release only its own resources and must not assert on the device's
	// still-live references held by the other renderer.
	first.reset();
	first = std::make_unique<PartiesRenderInterface_DX12>(first_window, settings);
	if (!*first) return 7;
	first.reset();
	second.reset();
	if (!diagnostics.check()) return 25;

	DestroyWindow(second_window);
	DestroyWindow(first_window);
	UnregisterClassW(kWindowClass, instance);
	return 0;
}
