#include "Parties_Renderer_DX12.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <Windows.h>
#include <dwmapi.h>

namespace {
bool CaptureWindowAsBmp(HWND window, const std::string& path) {
	RECT client_rect{};
	if (!window || !GetClientRect(window, &client_rect)) return false;
	const int width = client_rect.right - client_rect.left;
	const int height = client_rect.bottom - client_rect.top;
	if (width <= 0 || height <= 0) return false;

	HDC window_dc = GetDC(window);
	HDC memory_dc = window_dc ? CreateCompatibleDC(window_dc) : nullptr;
	BITMAPINFO bitmap_info{};
	bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bitmap_info.bmiHeader.biWidth = width;
	bitmap_info.bmiHeader.biHeight = -height;
	bitmap_info.bmiHeader.biPlanes = 1;
	bitmap_info.bmiHeader.biBitCount = 32;
	bitmap_info.bmiHeader.biCompression = BI_RGB;
	void* pixels = nullptr;
	HBITMAP bitmap = memory_dc ? CreateDIBSection(
		window_dc, &bitmap_info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
	HGDIOBJ old_bitmap = bitmap ? SelectObject(memory_dc, bitmap) : nullptr;

	DwmFlush();
	bool captured = bitmap && pixels &&
		PrintWindow(window, memory_dc, PW_CLIENTONLY | PW_RENDERFULLCONTENT) != FALSE;
	if (!captured && bitmap && pixels)
		captured = BitBlt(memory_dc, 0, 0, width, height, window_dc, 0, 0, SRCCOPY) != FALSE;

	bool saved = false;
	if (captured) {
		const uint32_t output_row_pitch = static_cast<uint32_t>((width * 3 + 3) & ~3);
		const uint32_t pixel_bytes = output_row_pitch * static_cast<uint32_t>(height);
		std::vector<Rml::byte> bgr(pixel_bytes, 0);
		const auto* bgra = static_cast<const Rml::byte*>(pixels);
		for (int output_y = 0; output_y < height; ++output_y) {
			const int source_y = height - 1 - output_y;
			const Rml::byte* source = bgra + static_cast<size_t>(source_y) * width * 4;
			Rml::byte* destination = bgr.data() + static_cast<size_t>(output_y) * output_row_pitch;
			for (int x = 0; x < width; ++x) {
				destination[x * 3 + 0] = source[x * 4 + 0];
				destination[x * 3 + 1] = source[x * 4 + 1];
				destination[x * 3 + 2] = source[x * 4 + 2];
			}
		}

		BITMAPFILEHEADER file_header{};
		BITMAPINFOHEADER info_header{};
		file_header.bfType = 0x4D42;
		file_header.bfOffBits = sizeof(file_header) + sizeof(info_header);
		file_header.bfSize = file_header.bfOffBits + pixel_bytes;
		info_header.biSize = sizeof(info_header);
		info_header.biWidth = width;
		info_header.biHeight = height;
		info_header.biPlanes = 1;
		info_header.biBitCount = 24;
		info_header.biCompression = BI_RGB;
		info_header.biSizeImage = pixel_bytes;

		FILE* file = std::fopen(path.c_str(), "wb");
		if (file) {
			saved = std::fwrite(&file_header, sizeof(file_header), 1, file) == 1 &&
				std::fwrite(&info_header, sizeof(info_header), 1, file) == 1 &&
				std::fwrite(bgr.data(), bgr.size(), 1, file) == 1;
			std::fclose(file);
		}
	}

	if (old_bitmap) SelectObject(memory_dc, old_bitmap);
	if (bitmap) DeleteObject(bitmap);
	if (memory_dc) DeleteDC(memory_dc);
	if (window_dc) ReleaseDC(window, window_dc);
	return saved;
}

D3D12_RESOURCE_BARRIER transition_resource(ID3D12Resource* resource,
	D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	return barrier;
}
} // namespace

struct PartiesRenderInterface_DX12::GeometryProxy {
	Rml::CompiledGeometryHandle upstream = 0;
};

struct PartiesRenderInterface_DX12::TextureProxy {
	Rml::TextureHandle upstream = 0;
	Rml::Vector2i dimensions{};
	std::shared_ptr<void> native_owner;
	void* native_resource = nullptr;
	void* native_chroma_resource = nullptr;
	void* ready_fence = nullptr;
	uint64_t ready_value = 0;
	D3D12_RESOURCE_STATES native_state = D3D12_RESOURCE_STATE_COMMON;
	uint32_t last_use_frame = uint32_t(-1);
	bool native_rgba = false;
	bool ready_wait_enqueued = false;
	Dx12VideoConverter::Texture* converted_video = nullptr;
};

PartiesRenderInterface_DX12::PartiesRenderInterface_DX12(
	void* window_handle, const Backend::RmlRendererSettings& settings)
	: upstream_(window_handle, settings), window_handle_(window_handle) {
	if (upstream_)
		video_converter_.Initialize(upstream_.Get_Device());
}

PartiesRenderInterface_DX12::~PartiesRenderInterface_DX12() {
	if (upstream_) upstream_.WaitIdle();
	for (auto& frame : retired_textures_) {
		for (auto& retired : frame)
			if (retired.upstream) upstream_.ReleaseTexture(retired.upstream);
		frame.clear();
	}
}

PartiesRenderInterface_DX12::operator bool() const { return static_cast<bool>(upstream_); }

void PartiesRenderInterface_DX12::SetViewport(int width, int height, bool force) {
	if (width <= 0 || height <= 0) return;
	// Restoring an unchanged window does not require a synthetic one-pixel
	// ResizeBuffers cycle. DWM surface refresh is handled by the window layer;
	// resizing twice here can exhaust or invalidate transient driver resources.
	(void)force;
	if (upstream_.SetViewport(width, height)) {
		viewport_width_ = width;
		viewport_height_ = height;
	}
}

void PartiesRenderInterface_DX12::BeginFrame() {
	frame_active_ = false;
	if (!upstream_.IsViewportValid()) return;
	upstream_.BeginFrame();
	frame_active_ = true;
}

void PartiesRenderInterface_DX12::Clear() { upstream_.Clear(); }

void PartiesRenderInterface_DX12::EndFrame() {
	if (!frame_active_) return;
	frame_active_ = false;
	upstream_.EndFrame();
	CollectRetiredTextures(upstream_.Get_CurrentFrameIndex());
}

void PartiesRenderInterface_DX12::CaptureNextFrame(std::string output_path) {
	// The upstream raw swap-chain helper reads the newly acquired flip buffer,
	// whose contents are undefined after Present. Capture the composed HWND
	// instead; this is deterministic for the upstream Win32 backend and keeps
	// screenshot-only synchronization out of the production render loop.
	last_capture_succeeded_ = CaptureWindowAsBmp(
		static_cast<HWND>(window_handle_), output_path);
	std::printf("[RmlUi DX12] Screenshot %s: %s\n",
		last_capture_succeeded_ ? "saved" : "failed", output_path.c_str());
}

PartiesRenderInterface_DX12::GeometryProxy* PartiesRenderInterface_DX12::Geometry(
	Rml::CompiledGeometryHandle handle) {
	return reinterpret_cast<GeometryProxy*>(handle);
}

PartiesRenderInterface_DX12::TextureProxy* PartiesRenderInterface_DX12::Texture(
	Rml::TextureHandle handle) {
	return reinterpret_cast<TextureProxy*>(handle);
}

Rml::CompiledGeometryHandle PartiesRenderInterface_DX12::CompileGeometry(
	Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) {
	auto proxy = std::make_unique<GeometryProxy>();
	proxy->upstream = upstream_.CompileGeometry(vertices, indices);
	if (!proxy->upstream) return 0;
	return reinterpret_cast<Rml::CompiledGeometryHandle>(proxy.release());
}

void PartiesRenderInterface_DX12::RenderGeometry(Rml::CompiledGeometryHandle geometry,
	Rml::Vector2f translation, Rml::TextureHandle texture) {
	auto* proxy = Geometry(geometry);
	auto* texture_proxy = (texture &&
		texture != RenderInterface_DX12::TextureEnableWithoutBinding &&
		texture != RenderInterface_DX12::TexturePostprocess) ? Texture(texture) : nullptr;
	if (texture_proxy)
		texture_proxy->last_use_frame = upstream_.Get_CurrentFrameIndex();
	if (texture_proxy && texture_proxy->native_resource) {
		if (!texture_proxy->ready_wait_enqueued && texture_proxy->ready_fence && texture_proxy->ready_value) {
			auto* queue = upstream_.Get_CommandQueue();
			if (queue && SUCCEEDED(queue->Wait(
				static_cast<ID3D12Fence*>(texture_proxy->ready_fence), texture_proxy->ready_value)))
				texture_proxy->ready_wait_enqueued = true;
		}
	}
	if (proxy && proxy->upstream) {
		const bool explicit_native_transition = texture_proxy && texture_proxy->native_resource &&
			!texture_proxy->native_rgba && texture_proxy->native_state != D3D12_RESOURCE_STATE_COMMON &&
			texture_proxy->native_state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE &&
			texture_proxy->native_state != D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
		if (explicit_native_transition) {
			auto barrier = transition_resource(
				static_cast<ID3D12Resource*>(texture_proxy->native_resource),
				texture_proxy->native_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			upstream_.Get_CommandList()->ResourceBarrier(1, &barrier);
		}
		upstream_.RenderGeometry(proxy->upstream, translation, UnwrapTexture(texture));
		if (explicit_native_transition) {
			auto barrier = transition_resource(
				static_cast<ID3D12Resource*>(texture_proxy->native_resource),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, texture_proxy->native_state);
			upstream_.Get_CommandList()->ResourceBarrier(1, &barrier);
		}
	}
}

void PartiesRenderInterface_DX12::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) {
	std::unique_ptr<GeometryProxy> proxy(Geometry(geometry));
	if (proxy && proxy->upstream) upstream_.ReleaseGeometry(proxy->upstream);
}

void PartiesRenderInterface_DX12::UpdateGeometryVertices(
	Rml::CompiledGeometryHandle geometry, Rml::Span<const Rml::Vertex> vertices) {
	auto* proxy = Geometry(geometry);
	if (!proxy || !proxy->upstream) return;
	auto* upstream_geometry = reinterpret_cast<RenderInterface_DX12::GeometryHandleType*>(proxy->upstream);
	if (upstream_geometry->Get_NumVertices() != static_cast<int>(vertices.size())) return;
	const auto& info = upstream_geometry->Get_InfoVertex();
	void* destination = upstream_.Get_BufferManager().Get_WritableMemoryFromBufferByOffset(info);
	if (destination)
		std::memcpy(destination, vertices.data(), vertices.size() * sizeof(Rml::Vertex));
}

void PartiesRenderInterface_DX12::EnableScissorRegion(bool enable) {
	upstream_.EnableScissorRegion(enable);
}
void PartiesRenderInterface_DX12::SetScissorRegion(Rml::Rectanglei region) {
	upstream_.SetScissorRegion(region);
}
void PartiesRenderInterface_DX12::EnableClipMask(bool enable) { upstream_.EnableClipMask(enable); }

void PartiesRenderInterface_DX12::RenderToClipMask(Rml::ClipMaskOperation operation,
	Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation) {
	auto* proxy = Geometry(geometry);
	if (proxy && proxy->upstream)
		upstream_.RenderToClipMask(operation, proxy->upstream, translation);
}

Rml::TextureHandle PartiesRenderInterface_DX12::WrapTexture(Rml::TextureHandle handle) {
	if (!handle) return 0;
	auto proxy = std::make_unique<TextureProxy>();
	proxy->upstream = handle;
	return reinterpret_cast<Rml::TextureHandle>(proxy.release());
}

Rml::TextureHandle PartiesRenderInterface_DX12::UnwrapTexture(Rml::TextureHandle handle) const {
	if (!handle || handle == RenderInterface_DX12::TextureEnableWithoutBinding ||
		handle == RenderInterface_DX12::TexturePostprocess)
		return handle;
	auto* proxy = Texture(handle);
	return proxy ? proxy->upstream : 0;
}

Rml::TextureHandle PartiesRenderInterface_DX12::LoadTexture(
	Rml::Vector2i& dimensions, const Rml::String& source) {
	Rml::TextureHandle handle = upstream_.LoadTexture(dimensions, source);
	Rml::TextureHandle wrapped = WrapTexture(handle);
	if (wrapped) Texture(wrapped)->dimensions = dimensions;
	return wrapped;
}

Rml::TextureHandle PartiesRenderInterface_DX12::GenerateTexture(
	Rml::Span<const Rml::byte> data, Rml::Vector2i dimensions) {
	Rml::TextureHandle wrapped = WrapTexture(upstream_.GenerateTexture(data, dimensions));
	if (wrapped) Texture(wrapped)->dimensions = dimensions;
	return wrapped;
}

Rml::TextureHandle PartiesRenderInterface_DX12::GenerateDynamicTexture(
	Rml::Span<const Rml::byte> data, Rml::Vector2i dimensions) {
	Rml::TextureHandle wrapped = WrapTexture(upstream_.GenerateTextureCommitted(data, dimensions));
	if (wrapped) Texture(wrapped)->dimensions = dimensions;
	return wrapped;
}

void PartiesRenderInterface_DX12::ReleaseTexture(Rml::TextureHandle handle) {
	if (!handle) return;
	std::unique_ptr<TextureProxy> proxy(Texture(handle));
	if (proxy && proxy->upstream)
		RetireTexture(proxy->upstream, std::move(proxy->native_owner), proxy->last_use_frame);
}

void PartiesRenderInterface_DX12::RetireTexture(Rml::TextureHandle handle,
	std::shared_ptr<void> owner, uint32_t last_use_frame) {
	if (!handle) return;
	if (last_use_frame == uint32_t(-1)) {
		upstream_.ReleaseTexture(handle);
		return;
	}
	retired_textures_[last_use_frame].push_back({handle, std::move(owner)});
}

void PartiesRenderInterface_DX12::CollectRetiredTextures(uint32_t completed_frame) {
	if (completed_frame >= retired_textures_.size()) return;
	auto& retired = retired_textures_[completed_frame];
	for (auto& texture : retired)
		if (texture.upstream) upstream_.ReleaseTexture(texture.upstream);
	retired.clear();
}

void PartiesRenderInterface_DX12::ReplaceTexture(TextureProxy& proxy,
	Rml::Span<const Rml::byte> data, Rml::Vector2i dimensions) {
	Rml::TextureHandle replacement = upstream_.GenerateTextureCommitted(data, dimensions);
	if (!replacement) return;
	if (proxy.upstream)
		RetireTexture(proxy.upstream, std::move(proxy.native_owner), proxy.last_use_frame);
	proxy.upstream = replacement;
	proxy.dimensions = dimensions;
	proxy.native_resource = nullptr;
	proxy.native_chroma_resource = nullptr;
	proxy.ready_fence = nullptr;
	proxy.ready_value = 0;
	proxy.native_state = D3D12_RESOURCE_STATE_COMMON;
	proxy.last_use_frame = uint32_t(-1);
	proxy.native_rgba = false;
	proxy.ready_wait_enqueued = false;
	proxy.converted_video = nullptr;
}

void PartiesRenderInterface_DX12::UpdateTextureData(Rml::TextureHandle handle,
	Rml::Span<const Rml::byte> data, Rml::Vector2i dimensions) {
	auto* proxy = Texture(handle);
	if (proxy) ReplaceTexture(*proxy, data, dimensions);
}

void PartiesRenderInterface_DX12::SetTransform(const Rml::Matrix4f* transform) {
	upstream_.SetTransform(transform);
}
Rml::LayerHandle PartiesRenderInterface_DX12::PushLayer() { return upstream_.PushLayer(); }
void PartiesRenderInterface_DX12::CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination,
	Rml::BlendMode mode, Rml::Span<const Rml::CompiledFilterHandle> filters) {
	upstream_.CompositeLayers(source, destination, mode, filters);
}
void PartiesRenderInterface_DX12::PopLayer() { upstream_.PopLayer(); }
Rml::TextureHandle PartiesRenderInterface_DX12::SaveLayerAsTexture() {
	return WrapTexture(upstream_.SaveLayerAsTexture());
}
Rml::CompiledFilterHandle PartiesRenderInterface_DX12::SaveLayerAsMaskImage() {
	return upstream_.SaveLayerAsMaskImage();
}
Rml::CompiledFilterHandle PartiesRenderInterface_DX12::CompileFilter(
	const Rml::String& name, const Rml::Dictionary& parameters) {
	return upstream_.CompileFilter(name, parameters);
}
void PartiesRenderInterface_DX12::ReleaseFilter(Rml::CompiledFilterHandle filter) { upstream_.ReleaseFilter(filter); }
Rml::CompiledShaderHandle PartiesRenderInterface_DX12::CompileShader(
	const Rml::String& name, const Rml::Dictionary& parameters) {
	return upstream_.CompileShader(name, parameters);
}
void PartiesRenderInterface_DX12::RenderShader(Rml::CompiledShaderHandle shader,
	Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) {
	auto* proxy = Geometry(geometry);
	if (proxy && proxy->upstream)
		upstream_.RenderShader(shader, proxy->upstream, translation, UnwrapTexture(texture));
}
void PartiesRenderInterface_DX12::ReleaseShader(Rml::CompiledShaderHandle shader) { upstream_.ReleaseShader(shader); }

namespace {
inline uint8_t clamp_channel(int value) {
	return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

inline void yuv_to_rgba(uint8_t* destination, int y, int u, int v) {
	const int c = std::max(0, y - 16);
	const int d = u - 128;
	const int e = v - 128;
	destination[0] = clamp_channel((298 * c + 409 * e + 128) >> 8);
	destination[1] = clamp_channel((298 * c - 100 * d - 208 * e + 128) >> 8);
	destination[2] = clamp_channel((298 * c + 516 * d + 128) >> 8);
	destination[3] = 255;
}
} // namespace

void PartiesRenderInterface_DX12::ConvertI420ToRgba(std::vector<Rml::byte>& rgba,
	const uint8_t* y_data, uint32_t y_stride, const uint8_t* u_data, const uint8_t* v_data,
	uint32_t uv_stride, uint32_t width, uint32_t height) {
	rgba.resize(static_cast<size_t>(width) * height * 4);
	for (uint32_t row = 0; row < height; ++row) {
		for (uint32_t column = 0; column < width; ++column) {
			yuv_to_rgba(rgba.data() + (static_cast<size_t>(row) * width + column) * 4,
				y_data[static_cast<size_t>(row) * y_stride + column],
				u_data[static_cast<size_t>(row / 2) * uv_stride + column / 2],
				v_data[static_cast<size_t>(row / 2) * uv_stride + column / 2]);
		}
	}
}

void PartiesRenderInterface_DX12::ConvertNV12ToRgba(std::vector<Rml::byte>& rgba,
	const uint8_t* y_data, uint32_t y_stride, const uint8_t* uv_data, uint32_t uv_stride,
	uint32_t width, uint32_t height) {
	rgba.resize(static_cast<size_t>(width) * height * 4);
	for (uint32_t row = 0; row < height; ++row) {
		for (uint32_t column = 0; column < width; ++column) {
			const size_t uv_offset = static_cast<size_t>(row / 2) * uv_stride + (column & ~1u);
			yuv_to_rgba(rgba.data() + (static_cast<size_t>(row) * width + column) * 4,
				y_data[static_cast<size_t>(row) * y_stride + column], uv_data[uv_offset], uv_data[uv_offset + 1]);
		}
	}
}

uintptr_t PartiesRenderInterface_DX12::GenerateYUVTexture(const uint8_t* y_data, uint32_t y_stride,
	const uint8_t* u_data, const uint8_t* v_data, uint32_t uv_stride, uint32_t width, uint32_t height) {
	if (video_converter_ && frame_active_) {
		auto video = video_converter_.Create(Dx12VideoConverter::Layout::I420, width, height);
		if (video && video_converter_.UpdateI420(*video, upstream_.Get_CommandList(),
			upstream_.Get_CurrentFrameIndex(), y_data, y_stride, u_data, v_data, uv_stride)) {
			auto upstream_handle = upstream_.RegisterExternalTexture(
				video->output.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
			auto wrapped = WrapTexture(upstream_handle);
			if (wrapped) {
				auto* proxy = Texture(wrapped);
				proxy->dimensions = {static_cast<int>(width), static_cast<int>(height)};
				proxy->native_owner = video;
				proxy->native_resource = video->output.Get();
				proxy->native_rgba = true;
				proxy->converted_video = video.get();
				return wrapped;
			}
		}
	}
	std::vector<Rml::byte> rgba;
	ConvertI420ToRgba(rgba, y_data, y_stride, u_data, v_data, uv_stride, width, height);
	return GenerateDynamicTexture(rgba, {static_cast<int>(width), static_cast<int>(height)});
}

void PartiesRenderInterface_DX12::UpdateYUVTexture(uintptr_t handle, const uint8_t* y_data,
	uint32_t y_stride, const uint8_t* u_data, const uint8_t* v_data, uint32_t uv_stride,
	uint32_t width, uint32_t height) {
	auto* proxy = Texture(handle);
	if (proxy && proxy->converted_video && frame_active_ &&
		proxy->converted_video->layout == Dx12VideoConverter::Layout::I420) {
		video_converter_.UpdateI420(*proxy->converted_video, upstream_.Get_CommandList(),
			upstream_.Get_CurrentFrameIndex(), y_data, y_stride, u_data, v_data, uv_stride);
		return;
	}
	std::vector<Rml::byte> rgba;
	ConvertI420ToRgba(rgba, y_data, y_stride, u_data, v_data, uv_stride, width, height);
	UpdateTextureData(handle, rgba, {static_cast<int>(width), static_cast<int>(height)});
}

void PartiesRenderInterface_DX12::ReleaseYUVTexture(uintptr_t handle) { ReleaseTexture(handle); }
void PartiesRenderInterface_DX12::RenderYUVGeometry(Rml::CompiledGeometryHandle geometry,
	Rml::Vector2f translation, uintptr_t handle) { RenderGeometry(geometry, translation, handle); }

uintptr_t PartiesRenderInterface_DX12::GenerateNV12Texture(const uint8_t* y_data, uint32_t y_stride,
	const uint8_t* uv_data, uint32_t uv_stride, uint32_t width, uint32_t height) {
	if (video_converter_ && frame_active_) {
		auto video = video_converter_.Create(Dx12VideoConverter::Layout::DirectNV12, width, height);
		if (video && video_converter_.UpdateNV12(*video, upstream_.Get_CommandList(),
			upstream_.Get_CurrentFrameIndex(), y_data, y_stride, uv_data, uv_stride)) {
			auto upstream_handle = upstream_.RegisterExternalNV12Texture(video->Luma(), video->Chroma());
			auto wrapped = WrapTexture(upstream_handle);
			if (wrapped) {
				auto* proxy = Texture(wrapped);
				proxy->dimensions = {static_cast<int>(width), static_cast<int>(height)};
				proxy->native_owner = video;
				proxy->native_rgba = false;
				proxy->converted_video = video.get();
				return wrapped;
			}
		}
	}
	std::vector<Rml::byte> rgba;
	ConvertNV12ToRgba(rgba, y_data, y_stride, uv_data, uv_stride, width, height);
	return GenerateDynamicTexture(rgba, {static_cast<int>(width), static_cast<int>(height)});
}

void PartiesRenderInterface_DX12::UpdateNV12Texture(uintptr_t handle, const uint8_t* y_data,
	uint32_t y_stride, const uint8_t* uv_data, uint32_t uv_stride, uint32_t width, uint32_t height) {
	auto* proxy = Texture(handle);
	if (proxy && proxy->converted_video && frame_active_ &&
		proxy->converted_video->layout == Dx12VideoConverter::Layout::DirectNV12) {
		video_converter_.UpdateNV12(*proxy->converted_video, upstream_.Get_CommandList(),
			upstream_.Get_CurrentFrameIndex(), y_data, y_stride, uv_data, uv_stride);
		return;
	}
	std::vector<Rml::byte> rgba;
	ConvertNV12ToRgba(rgba, y_data, y_stride, uv_data, uv_stride, width, height);
	UpdateTextureData(handle, rgba, {static_cast<int>(width), static_cast<int>(height)});
}

void PartiesRenderInterface_DX12::ReleaseNV12Texture(uintptr_t handle) { ReleaseTexture(handle); }
void PartiesRenderInterface_DX12::RenderNV12Geometry(Rml::CompiledGeometryHandle geometry,
	Rml::Vector2f translation, uintptr_t handle) { RenderGeometry(geometry, translation, handle); }

uintptr_t PartiesRenderInterface_DX12::GenerateNativeNV12Texture(
	void* d3d12_resource, void* d3d12_chroma_resource,
	std::shared_ptr<void> owner,
	void* ready_fence, uint64_t ready_value, uint32_t resource_state, bool rgba,
	uint32_t width, uint32_t height) {
	if (!d3d12_resource || !owner) return 0;
	auto* resource = static_cast<ID3D12Resource*>(d3d12_resource);
	auto* chroma_resource = static_cast<ID3D12Resource*>(d3d12_chroma_resource);
	auto upstream_handle = rgba
		? upstream_.RegisterExternalTexture(resource, DXGI_FORMAT_R8G8B8A8_UNORM)
		: upstream_.RegisterExternalNV12Texture(resource, chroma_resource);
	if (!upstream_handle) return 0;
	auto wrapped = WrapTexture(upstream_handle);
	auto* proxy = Texture(wrapped);
	proxy->dimensions = {static_cast<int>(width), static_cast<int>(height)};
	proxy->native_owner = std::move(owner);
	proxy->native_resource = d3d12_resource;
	proxy->native_chroma_resource = d3d12_chroma_resource;
	proxy->ready_fence = ready_fence;
	proxy->ready_value = ready_value;
	proxy->native_state = static_cast<D3D12_RESOURCE_STATES>(resource_state);
	proxy->native_rgba = rgba;
	return wrapped;
}

bool PartiesRenderInterface_DX12::UpdateNativeNV12Texture(
	uintptr_t handle, void* d3d12_resource, void* d3d12_chroma_resource,
	std::shared_ptr<void> owner,
	void* ready_fence, uint64_t ready_value, uint32_t resource_state, bool rgba,
	uint32_t width, uint32_t height) {
	auto* proxy = Texture(handle);
	if (!proxy || !d3d12_resource || !owner) return false;
	if (proxy->native_resource != d3d12_resource ||
		proxy->native_chroma_resource != d3d12_chroma_resource ||
		proxy->native_rgba != rgba) {
		auto* resource = static_cast<ID3D12Resource*>(d3d12_resource);
		auto* chroma_resource = static_cast<ID3D12Resource*>(d3d12_chroma_resource);
		auto replacement = rgba
			? upstream_.RegisterExternalTexture(resource, DXGI_FORMAT_R8G8B8A8_UNORM)
			: upstream_.RegisterExternalNV12Texture(resource, chroma_resource);
		if (!replacement) return false;
		RetireTexture(proxy->upstream, std::move(proxy->native_owner), proxy->last_use_frame);
		proxy->upstream = replacement;
		proxy->last_use_frame = uint32_t(-1);
	}
	proxy->dimensions = {static_cast<int>(width), static_cast<int>(height)};
	proxy->native_owner = std::move(owner);
	proxy->native_resource = d3d12_resource;
	proxy->native_chroma_resource = d3d12_chroma_resource;
	proxy->ready_fence = ready_fence;
	proxy->ready_value = ready_value;
	proxy->native_state = static_cast<D3D12_RESOURCE_STATES>(resource_state);
	proxy->native_rgba = rgba;
	proxy->ready_wait_enqueued = false;
	return true;
}

bool PartiesRenderInterface_DX12::CaptureScreen(int& width, int& height, int& components,
	Rml::UniquePtr<Rml::byte[]>& data) {
	return upstream_.CaptureScreen(width, height, components, data);
}
