#pragma once

#include <RmlUi/Core/RenderInterface.h>

#include <cstdint>
#include <memory>

namespace Backend {
struct RmlRendererSettings;
}

class ExtendedRenderInterface : public Rml::RenderInterface {
public:
	virtual ~ExtendedRenderInterface() = default;

	virtual explicit operator bool() const { return true; }

	virtual void SetViewport(int, int, bool = false) {}

	virtual void BeginFrame() {}

	virtual bool IsFrameActive() const { return true; }

	virtual void Clear() {}

	virtual void EndFrame() {}

	virtual void* GetD3D12Device() const { return nullptr; }

	virtual void ReleaseRetiredResources() {}

	virtual void UpdateGeometryVertices(Rml::CompiledGeometryHandle, Rml::Span<const Rml::Vertex>) {}

	virtual void UpdateTextureData(Rml::TextureHandle, Rml::Span<const Rml::byte>, Rml::Vector2i) {}

	virtual Rml::TextureHandle GenerateDynamicTexture(
		Rml::Span<const Rml::byte> source_data, Rml::Vector2i source_dimensions) {
		return GenerateTexture(source_data, source_dimensions);
	}

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

	virtual uintptr_t GenerateNativeNV12Texture(
		void*, void*,
		std::shared_ptr<void>,
		void*, uint64_t, uint32_t, bool,
		uint32_t, uint32_t) { return 0; }
	virtual bool UpdateNativeNV12Texture(
		uintptr_t, void*, void*,
		std::shared_ptr<void>,
		void*, uint64_t, uint32_t, bool,
		uint32_t, uint32_t) { return false; }
};
