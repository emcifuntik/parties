#pragma once

#include <RmlUi/Core/RenderInterface.h>

#ifndef RMLUI_PLATFORM_WIN32
	#error "DirectX 12 renderer only supported on Windows"
#endif

// Forward declarations — avoid pulling heavy DX12 headers into every TU
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12CommandAllocator;
struct ID3D12GraphicsCommandList;
struct ID3D12DescriptorHeap;
struct ID3D12Resource;
struct ID3D12Fence;
struct ID3D12RootSignature;
struct ID3D12PipelineState;
struct IDXGISwapChain4;
struct IDXGIFactory4;
struct IDXGIAdapter1;

// Required for HANDLE, HWND
// clang-format off
#include "RmlUi_Include_Windows.h"
// clang-format on

#include <wrl/client.h>  // ComPtr
#include <cstdint>
#include <array>
#include <vector>

namespace Backend {
struct RmlRendererSettings {
	bool vsync;
	unsigned char msaa_sample_count;
};
} // namespace Backend

// Internal data structures — forward declared, defined in .cpp
struct GeometryData;
struct TextureData;
struct LayerData;
struct CompiledFilterData;
struct CompiledShaderData;
struct PostprocessTarget;

class RenderInterface_DX12 : public Rml::RenderInterface {
public:
	static constexpr int NUM_BACK_BUFFERS = 2;
	static constexpr int SRV_HEAP_SIZE = 1024;
	static constexpr int MAX_LAYER_COUNT = 16;
	static constexpr int NUM_POSTPROCESS_TARGETS = 2;
	// RTV heap: back buffers + layers + postprocess targets
	static constexpr int RTV_HEAP_SIZE = NUM_BACK_BUFFERS + MAX_LAYER_COUNT + NUM_POSTPROCESS_TARGETS;

	RenderInterface_DX12(void* p_window_handle, const Backend::RmlRendererSettings& settings);
	~RenderInterface_DX12();

	// Returns true if the renderer was successfully constructed.
	explicit operator bool() const;

	// The viewport should be updated whenever the window size changes.
	void SetViewport(int viewport_width, int viewport_height, bool force = false);

	// Sets up DX12 states for taking rendering commands from RmlUi.
	void BeginFrame();

	// Optional, can be used to clear the active framebuffer.
	void Clear();

	// Presents to screen and synchronizes.
	void EndFrame();

	// -- Inherited from Rml::RenderInterface --

	Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
	void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
	void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

	Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
	Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source_data, Rml::Vector2i source_dimensions) override;
	void ReleaseTexture(Rml::TextureHandle texture_handle) override;

	// Updates pixel data of an existing texture in-place (no resource/SRV reallocation).
	// Dimensions must match the original texture. For streaming video.
	void UpdateTextureData(Rml::TextureHandle texture_handle, Rml::Span<const Rml::byte> source_data, Rml::Vector2i source_dimensions);

	// YUV texture support — uploads I420 planes as 3 separate R8 GPU textures,
	// converted to RGB by a pixel shader during rendering (zero CPU conversion).
	// The handle is an opaque pointer (cast to YUVTextureData*).
	uintptr_t GenerateYUVTexture(
		const uint8_t* y_data, uint32_t y_stride,
		const uint8_t* u_data, const uint8_t* v_data, uint32_t uv_stride,
		uint32_t width, uint32_t height);
	void UpdateYUVTexture(uintptr_t handle,
		const uint8_t* y_data, uint32_t y_stride,
		const uint8_t* u_data, const uint8_t* v_data, uint32_t uv_stride,
		uint32_t width, uint32_t height);
	void ReleaseYUVTexture(uintptr_t handle);
	void RenderYUVGeometry(Rml::CompiledGeometryHandle geometry,
		Rml::Vector2f translation, uintptr_t yuv_handle);

	void EnableScissorRegion(bool enable) override;
	void SetScissorRegion(Rml::Rectanglei region) override;

	void SetTransform(const Rml::Matrix4f* transform) override;

	void EnableClipMask(bool enable) override;
	void RenderToClipMask(Rml::ClipMaskOperation mask_operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation) override;

	Rml::LayerHandle PushLayer() override;
	void PopLayer() override;
	void CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blend_mode,
		Rml::Span<const Rml::CompiledFilterHandle> filters) override;

	Rml::TextureHandle SaveLayerAsTexture() override;
	Rml::CompiledFilterHandle SaveLayerAsMaskImage() override;

	Rml::CompiledFilterHandle CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters) override;
	void ReleaseFilter(Rml::CompiledFilterHandle filter) override;

	Rml::CompiledShaderHandle CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) override;
	void RenderShader(Rml::CompiledShaderHandle shader_handle, Rml::CompiledGeometryHandle geometry_handle, Rml::Vector2f translation,
		Rml::TextureHandle texture) override;
	void ReleaseShader(Rml::CompiledShaderHandle effect_handle) override;

private:
	template <typename T>
	using ComPtr = Microsoft::WRL::ComPtr<T>;

	// --- Initialization ---
	bool CreateDevice();
	bool CreateCommandQueue();
	bool CreateSwapChain(int width, int height);
	bool CreateRtvHeap();
	bool CreateDsvHeap();
	bool CreateSrvHeap();
	bool CreateCommandAllocatorsAndList();
	bool CreateFences();
	void CreateRenderTargetViews();
	bool CreateDepthStencilBuffer();
	bool CreatePipelineState();
	bool CreateFullscreenQuad();
	bool CreatePostprocessTargets();

	// --- Layer management ---
	int AllocateLayer();
	void FreeLayer(int layer_index);
	void ReleaseAllLayers();
	void SetRenderTargetToLayer(int layer_index);
	void SetRenderTargetToBackBuffer();

	// --- Postprocess ---
	void ReleasePostprocessTargets();
	void RenderFilters(Rml::Span<const Rml::CompiledFilterHandle> filters, int source_layer_idx);
	void RenderBlur(float sigma, int src_pp, int dst_pp);

	// --- Synchronization ---
	void Flush();
	void WaitForFenceValue(uint64_t value);
	void ReleaseBackBufferResources();

	// --- Deferred deletion ---
	struct DeferredRelease {
		ComPtr<ID3D12Resource> resource;
		int32_t srv_index = -1;  // -1 if no SRV slot to free
	};
	void DeferRelease(ComPtr<ID3D12Resource> resource, int32_t srv_index = -1);
	void ProcessDeferredReleases();

	// --- SRV descriptor allocation ---
	int32_t AllocateSrvSlot();
	void FreeSrvSlot(int32_t index);

	// --- Texture upload helpers ---
	void UploadTextureData(ID3D12Resource* dest_texture, const Rml::byte* data, int width, int height);
	void UploadR8TextureData(ID3D12Resource* dest_texture, const uint8_t* data,
		uint32_t src_stride, int width, int height,
		void* upload_buf = nullptr);

	// --- CB helpers ---
	ComPtr<ID3D12Resource> CreateUploadBuffer(UINT size, const void* data);

	// Per-frame linear upload heap for constant buffers (avoids per-draw CreateCommittedResource)
	struct FrameUploadHeap;
	struct CbAllocation {
		uint64_t gpu_address;
		void* cpu_ptr;
	};
	static constexpr uint64_t FRAME_UPLOAD_HEAP_SIZE = 256 * 1024; // 256 KB per frame
	FrameUploadHeap* frame_upload_heaps_ = nullptr; // array of NUM_BACK_BUFFERS
	bool CreateFrameUploadHeaps();
	CbAllocation AllocateCB(uint32_t size, const void* data = nullptr);

	// --- State ---
	bool valid_ = false;
	bool vsync_ = true;
	int width_ = 0;
	int height_ = 0;
	HWND hwnd_ = nullptr;
	HANDLE frame_latency_waitable_ = nullptr;

	// Core objects
	ComPtr<IDXGIFactory4> factory_;
	ComPtr<ID3D12Device> device_;
	ComPtr<ID3D12CommandQueue> command_queue_;
	ComPtr<IDXGISwapChain4> swap_chain_;

	// RTV descriptor heap
	ComPtr<ID3D12DescriptorHeap> rtv_heap_;
	uint32_t rtv_descriptor_size_ = 0;

	// DSV descriptor heap and depth/stencil buffer
	ComPtr<ID3D12DescriptorHeap> dsv_heap_;
	ComPtr<ID3D12Resource> depth_stencil_buffer_;

	// SRV descriptor heap (shader-visible, for texture binding)
	ComPtr<ID3D12DescriptorHeap> srv_heap_;
	uint32_t srv_descriptor_size_ = 0;
	std::vector<int32_t> srv_free_list_;  // free slot indices

	// Pipeline
	ComPtr<ID3D12RootSignature> root_signature_;
	ComPtr<ID3D12PipelineState> pso_color_;              // no texture, stencil disabled
	ComPtr<ID3D12PipelineState> pso_texture_;            // with texture, stencil disabled
	ComPtr<ID3D12PipelineState> pso_color_stencil_;      // no texture, stencil EQUAL test
	ComPtr<ID3D12PipelineState> pso_texture_stencil_;    // with texture, stencil EQUAL test
	ComPtr<ID3D12PipelineState> pso_stencil_set_;        // write stencil (REPLACE), no color
	ComPtr<ID3D12PipelineState> pso_stencil_intersect_;  // write stencil (INCR_SAT), no color

	// YUV video rendering (3 × R8 textures → RGB conversion in pixel shader)
	ComPtr<ID3D12RootSignature> yuv_root_signature_;
	ComPtr<ID3D12PipelineState> pso_yuv_;
	ComPtr<ID3D12PipelineState> pso_yuv_stencil_;

	// Per-frame resources
	std::array<ComPtr<ID3D12Resource>, NUM_BACK_BUFFERS> back_buffers_;
	std::array<ComPtr<ID3D12CommandAllocator>, NUM_BACK_BUFFERS> command_allocators_;
	ComPtr<ID3D12GraphicsCommandList> command_list_;

	// Per-frame deferred deletion
	std::array<std::vector<DeferredRelease>, NUM_BACK_BUFFERS> deferred_releases_;

	// Synchronization
	ComPtr<ID3D12Fence> fence_;
	HANDLE fence_event_ = nullptr;
	std::array<uint64_t, NUM_BACK_BUFFERS> fence_values_ = {};
	uint64_t next_fence_value_ = 1;

	UINT current_back_buffer_index_ = 0;

	// Passthrough PSOs for layer compositing (DSVFormat=UNKNOWN, no stencil)
	ComPtr<ID3D12PipelineState> pso_passthrough_blend_;    // premultiplied alpha blend
	ComPtr<ID3D12PipelineState> pso_passthrough_replace_;  // no blending, overwrite

	// Phase 6 PSOs for filters and shaders (DSVFormat=UNKNOWN, no stencil)
	ComPtr<ID3D12PipelineState> pso_color_matrix_;         // color matrix filter
	ComPtr<ID3D12PipelineState> pso_blur_;                 // separable Gaussian blur
	ComPtr<ID3D12PipelineState> pso_drop_shadow_;          // drop shadow tint
	ComPtr<ID3D12PipelineState> pso_blend_mask_;           // blend mask (source * mask alpha)
	ComPtr<ID3D12PipelineState> pso_gradient_;             // gradient shader (premultiplied blend, with DSV)
	ComPtr<ID3D12PipelineState> pso_gradient_stencil_;     // gradient shader (with stencil test)

	// Fullscreen quad for layer compositing (pre-compiled at init, uses GeometryData)
	GeometryData* fullscreen_quad_ = nullptr;

	// Postprocess render targets for filter ping-ponging
	std::array<PostprocessTarget*, NUM_POSTPROCESS_TARGETS> postprocess_targets_ = {};

	// Layer pool and stack
	std::vector<LayerData> layer_pool_;          // all allocated layers
	std::vector<bool> layer_in_use_;             // which pool slots are in use
	std::vector<int> layer_stack_;               // stack of active layer indices (-1 = back buffer)

	// Render state
	Rml::Matrix4f projection_ = Rml::Matrix4f::Identity();
	Rml::Matrix4f transform_ = Rml::Matrix4f::Identity();
	bool transform_active_ = false;
	bool scissor_enabled_ = false;
	RECT scissor_rect_ = {};

	// Clip mask state (stencil-based)
	bool clip_mask_enabled_ = false;
	uint32_t stencil_ref_ = 0;
};
