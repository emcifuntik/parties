#pragma once

#include "RmlUi_RenderInterface_Extended.h"
#include "Dx12VideoConverter.h"

// This is the renderer shipped by the pinned mikke89/RmlUi revision. The
// include directory is supplied by the rmlui vcpkg port's Backends bundle.
#include <RmlUi_Renderer_DX12.h>

#include <memory>
#include <string>
#include <array>
#include <vector>

// Parties only adapts its application-specific streaming operations to the
// upstream renderer. Device creation, swap chain management, command recording,
// clipping, layers, filters, shaders, and presentation all remain owned by
// RmlUi's built-in Win32/DX12 implementation.
class PartiesRenderInterface_DX12 final : public ExtendedRenderInterface {
public:
	PartiesRenderInterface_DX12(void* window_handle, const Backend::RmlRendererSettings& settings);
	~PartiesRenderInterface_DX12() override;

	explicit operator bool() const override;
	void SetViewport(int viewport_width, int viewport_height, bool force = false) override;
	void BeginFrame() override;
	bool IsFrameActive() const override { return frame_active_; }
	void Clear() override;
	void EndFrame() override;
	void CaptureNextFrame(std::string output_path);
	bool LastCaptureSucceeded() const { return last_capture_succeeded_; }

	void* GetD3D12Device() const override { return upstream_.Get_Device(); }

	Rml::CompiledGeometryHandle CompileGeometry(
		Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
	void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
		Rml::TextureHandle texture) override;
	void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
	void UpdateGeometryVertices(Rml::CompiledGeometryHandle geometry,
		Rml::Span<const Rml::Vertex> vertices) override;

	void EnableScissorRegion(bool enable) override;
	void SetScissorRegion(Rml::Rectanglei region) override;
	void EnableClipMask(bool enable) override;
	void RenderToClipMask(Rml::ClipMaskOperation mask_operation,
		Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation) override;

	Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions,
		const Rml::String& source) override;
	Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source_data,
		Rml::Vector2i source_dimensions) override;
	void ReleaseTexture(Rml::TextureHandle texture_handle) override;
	void UpdateTextureData(Rml::TextureHandle texture_handle,
		Rml::Span<const Rml::byte> source_data, Rml::Vector2i source_dimensions) override;

	void SetTransform(const Rml::Matrix4f* transform) override;
	Rml::LayerHandle PushLayer() override;
	void CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination,
		Rml::BlendMode blend_mode, Rml::Span<const Rml::CompiledFilterHandle> filters) override;
	void PopLayer() override;
	Rml::TextureHandle SaveLayerAsTexture() override;
	Rml::CompiledFilterHandle SaveLayerAsMaskImage() override;
	Rml::CompiledFilterHandle CompileFilter(const Rml::String& name,
		const Rml::Dictionary& parameters) override;
	void ReleaseFilter(Rml::CompiledFilterHandle filter) override;
	Rml::CompiledShaderHandle CompileShader(const Rml::String& name,
		const Rml::Dictionary& parameters) override;
	void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry,
		Rml::Vector2f translation, Rml::TextureHandle texture) override;
	void ReleaseShader(Rml::CompiledShaderHandle shader) override;

	uintptr_t GenerateYUVTexture(const uint8_t* y_data, uint32_t y_stride,
		const uint8_t* u_data, const uint8_t* v_data, uint32_t uv_stride,
		uint32_t width, uint32_t height) override;
	void UpdateYUVTexture(uintptr_t handle, const uint8_t* y_data, uint32_t y_stride,
		const uint8_t* u_data, const uint8_t* v_data, uint32_t uv_stride,
		uint32_t width, uint32_t height) override;
	void ReleaseYUVTexture(uintptr_t handle) override;
	void RenderYUVGeometry(Rml::CompiledGeometryHandle geometry,
		Rml::Vector2f translation, uintptr_t yuv_handle) override;

	uintptr_t GenerateNV12Texture(const uint8_t* y_data, uint32_t y_stride,
		const uint8_t* uv_data, uint32_t uv_stride,
		uint32_t width, uint32_t height) override;
	void UpdateNV12Texture(uintptr_t handle, const uint8_t* y_data, uint32_t y_stride,
		const uint8_t* uv_data, uint32_t uv_stride,
		uint32_t width, uint32_t height) override;
	void ReleaseNV12Texture(uintptr_t handle) override;
	void RenderNV12Geometry(Rml::CompiledGeometryHandle geometry,
		Rml::Vector2f translation, uintptr_t nv12_handle) override;

	bool CaptureScreen(int& width, int& height, int& num_components,
		Rml::UniquePtr<Rml::byte[]>& data);
	uintptr_t GenerateNativeNV12Texture(
		void* d3d12_resource, void* d3d12_chroma_resource,
		std::shared_ptr<void> owner,
		void* ready_fence, uint64_t ready_value, uint32_t resource_state, bool rgba,
		uint32_t width, uint32_t height) override;
	bool UpdateNativeNV12Texture(
		uintptr_t handle, void* d3d12_resource, void* d3d12_chroma_resource,
		std::shared_ptr<void> owner,
		void* ready_fence, uint64_t ready_value, uint32_t resource_state, bool rgba,
		uint32_t width, uint32_t height) override;
	RenderInterface_DX12& upstream() { return upstream_; }
	const RenderInterface_DX12& upstream() const { return upstream_; }

private:
	struct GeometryProxy;
	struct TextureProxy;
	struct RetiredTexture {
		Rml::TextureHandle upstream = 0;
		std::shared_ptr<void> owner;
	};

	static GeometryProxy* Geometry(Rml::CompiledGeometryHandle handle);
	static TextureProxy* Texture(Rml::TextureHandle handle);
	Rml::TextureHandle WrapTexture(Rml::TextureHandle upstream_handle);
	Rml::TextureHandle UnwrapTexture(Rml::TextureHandle handle) const;
	void ReplaceTexture(TextureProxy& proxy, Rml::Span<const Rml::byte> source_data,
		Rml::Vector2i source_dimensions);
	void RetireTexture(Rml::TextureHandle upstream_handle, std::shared_ptr<void> owner,
		uint32_t last_use_frame);
	void CollectRetiredTextures(uint32_t completed_frame);

	static void ConvertI420ToRgba(std::vector<Rml::byte>& rgba,
		const uint8_t* y_data, uint32_t y_stride,
		const uint8_t* u_data, const uint8_t* v_data, uint32_t uv_stride,
		uint32_t width, uint32_t height);
	static void ConvertNV12ToRgba(std::vector<Rml::byte>& rgba,
		const uint8_t* y_data, uint32_t y_stride,
		const uint8_t* uv_data, uint32_t uv_stride,
		uint32_t width, uint32_t height);

	RenderInterface_DX12 upstream_;
	Dx12VideoConverter video_converter_;
	void* window_handle_ = nullptr;
	int viewport_width_ = 0;
	int viewport_height_ = 0;
	bool frame_active_ = false;
	bool last_capture_succeeded_ = false;
	std::array<std::vector<RetiredTexture>, RMLUI_RENDER_BACKEND_FIELD_SWAPCHAIN_BACKBUFFER_COUNT>
		retired_textures_;
};
