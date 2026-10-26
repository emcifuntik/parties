#pragma once

#include <client/native_video_frame.h>

#include <RmlUi/Core/RenderInterface.h>

#include <cstdint>

namespace Backend {
struct RmlRendererSettings;
}

struct I420Planes {
	const uint8_t* y = nullptr;
	const uint8_t* u = nullptr;
	const uint8_t* v = nullptr;
	uint32_t y_stride = 0;
	uint32_t uv_stride = 0;
	uint32_t width = 0;
	uint32_t height = 0;
};

class I420TextureRenderer {
public:
	virtual ~I420TextureRenderer() = default;
	virtual uintptr_t GenerateI420Texture(const I420Planes& planes) = 0;
	virtual void UpdateI420Texture(uintptr_t handle, const I420Planes& planes) = 0;
	virtual void ReleaseI420Texture(uintptr_t handle) = 0;
	virtual void RenderI420Geometry(Rml::CompiledGeometryHandle geometry,
		Rml::Vector2f translation, uintptr_t handle) = 0;
};

class NativeVideoTextureRenderer {
public:
	virtual ~NativeVideoTextureRenderer() = default;
	virtual uintptr_t GenerateNativeVideoTexture(const NativeVideoFrame& frame) = 0;
	virtual bool UpdateNativeVideoTexture(uintptr_t handle, const NativeVideoFrame& frame) = 0;
};

class ExtendedRenderInterface : public Rml::RenderInterface {
public:
	virtual ~ExtendedRenderInterface() = default;

	virtual explicit operator bool() const { return true; }

	virtual void SetViewport(int, int) {}

	virtual void BeginFrame() {}

	virtual bool IsFrameActive() const { return true; }

	virtual void Clear() {}

	virtual void EndFrame() {}

	virtual void* GetD3D12Device() const { return nullptr; }

	virtual void ReleaseRetiredResourcesBetweenFrames() {}

	virtual void UpdateGeometryVertices(Rml::CompiledGeometryHandle, Rml::Span<const Rml::Vertex>) {}

	virtual void UpdateTextureData(Rml::TextureHandle, Rml::Span<const Rml::byte>, Rml::Vector2i) {}

	virtual Rml::TextureHandle GenerateDynamicTexture(
		Rml::Span<const Rml::byte> source_data, Rml::Vector2i source_dimensions) {
		return GenerateTexture(source_data, source_dimensions);
	}

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

	virtual I420TextureRenderer* I420Textures() { return nullptr; }
	virtual NativeVideoTextureRenderer* NativeVideoTextures() { return nullptr; }
};
