#include "dx12/Parties_Renderer_DX12.h"

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <memory>
#include <array>
#include <vector>

namespace {
constexpr wchar_t kWindowClass[] = L"PartiesRmlUiDx12MultiwindowTest";

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
	return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace

int main() {
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
	settings.msaa_sample_count = 1;
	auto first = std::make_unique<PartiesRenderInterface_DX12>(first_window, settings);
	auto second = std::make_unique<PartiesRenderInterface_DX12>(second_window, settings);
	if (!*first || !*second) return 3;

	// Parties' in-tree backend must be able to bind a packed NV12 resource as
	// two plane SRVs without allocating an intermediate RGBA texture.
	auto* device = static_cast<ID3D12Device*>(first->GetD3D12Device());
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

	DestroyWindow(second_window);
	DestroyWindow(first_window);
	UnregisterClassW(kWindowClass, instance);
	return 0;
}
