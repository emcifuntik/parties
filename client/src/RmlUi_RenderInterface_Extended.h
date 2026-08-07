#pragma once

#include <RmlUi/Core/RenderInterface.h>

#include <cstdint>
#include <memory>

namespace Backend {
struct RmlRendererSettings;
} // namespace Backend

// Extended render interface adding YUV/NV12 video texture support, streaming texture updates,
// and geometry vertex updates beyond what the base Rml::RenderInterface provides.
// Windows adapts these operations over RmlUi's upstream DX12 backend; Metal
// implements the video operations in its native renderer.
class ExtendedRenderInterface : public Rml::RenderInterface {
public:
	virtual ~ExtendedRenderInterface() = default;

	// Returns true if the renderer was successfully constructed.
	virtual explicit operator bool() const { return true; }

	// The viewport should be updated whenever the window size changes.
	// The Windows adapter overrides this; Metal has its own entry point.
	virtual void SetViewport(int /*viewport_width*/, int /*viewport_height*/, bool /*force*/ = false) {}

	// Sets up GPU states for taking rendering commands from RmlUi.
	// The Windows adapter overrides this; Metal uses its own command-buffer entry point.
	virtual void BeginFrame() {}

	// Returns whether BeginFrame successfully opened a frame for recording.
	// Backends without a fallible BeginFrame can use the default.
	virtual bool IsFrameActive() const { return true; }

	// Optional, can be used to clear the active framebuffer.
	virtual void Clear() {}

	// Presents to screen and synchronizes.
	// The Windows adapter overrides this; Metal uses its own EndFrame() entry point.
	virtual void EndFrame() {}

	// Native D3D12 device when this renderer can consume decoder surfaces
	// directly. Other backends deliberately return null and keep the portable
	// CPU-plane upload path.
	virtual void* GetD3D12Device() const { return nullptr; }

	// Re-map existing VB with new vertex data (no GPU resource allocation).
	// The Windows adapter overrides this; Metal does not currently implement it.
	virtual void UpdateGeometryVertices(Rml::CompiledGeometryHandle /*geometry*/, Rml::Span<const Rml::Vertex> /*vertices*/) {}

	// Updates pixel data of an existing texture in-place (no resource/SRV reallocation).
	// The Windows adapter overrides this; Metal does not currently implement it.
	virtual void UpdateTextureData(Rml::TextureHandle /*texture_handle*/, Rml::Span<const Rml::byte> /*source_data*/, Rml::Vector2i /*source_dimensions*/) {}

	// Creates a frequently replaced texture, such as a software-decoded video
	// frame or a capture preview. Backends may keep these resources out of
	// suballocated heaps whose lifetime and aliasing rules are intended for
	// long-lived UI assets.
	virtual Rml::TextureHandle GenerateDynamicTexture(
		Rml::Span<const Rml::byte> source_data, Rml::Vector2i source_dimensions) {
		return GenerateTexture(source_data, source_dimensions);
	}

	// YUV texture support (I420: 3 x R8 planes -> RGB in pixel shader)
	virtual uintptr_t GenerateYUVTexture(
		const uint8_t* y_data, uint32_t y_stride,
		const uint8_t* u_data, const uint8_t* v_data, uint32_t uv_stride,
		uint32_t width, uint32_t height) = 0;
	virtual void UpdateYUVTexture(uintptr_t handle,
		const uint8_t* y_data, uint32_t y_stride,
		const uint8_t* u_data, const uint8_t* v_data, uint32_t uv_stride,
		uint32_t width, uint32_t height) = 0;
	virtual void ReleaseYUVTexture(uintptr_t handle) = 0;
	virtual void RenderYUVGeometry(Rml::CompiledGeometryHandle geometry,
		Rml::Vector2f translation, uintptr_t yuv_handle) = 0;

	// NV12 texture support (R8 Y + R8G8 UV -> RGB in pixel shader)
	virtual uintptr_t GenerateNV12Texture(
		const uint8_t* y_data, uint32_t y_stride,
		const uint8_t* uv_data, uint32_t uv_stride,
		uint32_t width, uint32_t height) = 0;
	virtual void UpdateNV12Texture(uintptr_t handle,
		const uint8_t* y_data, uint32_t y_stride,
		const uint8_t* uv_data, uint32_t uv_stride,
		uint32_t width, uint32_t height) = 0;
	virtual void ReleaseNV12Texture(uintptr_t handle) = 0;
	virtual void RenderNV12Geometry(Rml::CompiledGeometryHandle geometry,
		Rml::Vector2f translation, uintptr_t nv12_handle) = 0;

	// Direct decoder-resource path. The input may be packed NV12 sampled as two
	// plane SRVs or ready-to-sample RGBA. The owner keeps vendor resources
	// reserved until the render backend's per-back-buffer fence has completed.
	virtual uintptr_t GenerateNativeNV12Texture(
		void* /*d3d12_resource*/, void* /*d3d12_chroma_resource*/,
		std::shared_ptr<void> /*owner*/,
		void* /*ready_fence*/, uint64_t /*ready_value*/, uint32_t /*resource_state*/, bool /*rgba*/,
		uint32_t /*width*/, uint32_t /*height*/) { return 0; }
	virtual bool UpdateNativeNV12Texture(
		uintptr_t /*handle*/, void* /*d3d12_resource*/, void* /*d3d12_chroma_resource*/,
		std::shared_ptr<void> /*owner*/,
		void* /*ready_fence*/, uint64_t /*ready_value*/, uint32_t /*resource_state*/, bool /*rgba*/,
		uint32_t /*width*/, uint32_t /*height*/) { return false; }
};
