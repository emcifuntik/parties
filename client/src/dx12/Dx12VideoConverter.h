#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include <RmlUi_Include_DirectX_12.h>
#include <d3d12.h>
#include <wrl/client.h>

class Dx12FrameSlot {
public:
	static Dx12FrameSlot after_fence_wait(uint32_t back_buffer_index) {
		return Dx12FrameSlot(back_buffer_index);
	}
	uint32_t index() const noexcept { return index_; }

private:
	explicit Dx12FrameSlot(uint32_t index) : index_(index) {}
	uint32_t index_;
};

class Dx12VideoConverter {
public:
	static constexpr uint32_t frame_count = RMLUI_RENDER_BACKEND_FIELD_SWAPCHAIN_BACKBUFFER_COUNT;

	enum class Layout : uint32_t {
		I420 = 0,
		NV12 = 1,
		DirectNV12 = 2,
	};

	struct Texture {
		~Texture();

		Microsoft::WRL::ComPtr<ID3D12Resource> output;
		uint32_t width = 0;
		uint32_t height = 0;
		Layout layout = Layout::NV12;

		ID3D12Resource* Luma() const noexcept { return y.Get(); }
		ID3D12Resource* Chroma() const noexcept { return uv.Get(); }

	private:
		friend class Dx12VideoConverter;
		struct UploadPlane {
			Microsoft::WRL::ComPtr<ID3D12Resource> resource;
			void* mapped = nullptr;
			uint32_t row_pitch = 0;
		};

		Microsoft::WRL::ComPtr<ID3D12Resource> y;
		Microsoft::WRL::ComPtr<ID3D12Resource> uv;
		Microsoft::WRL::ComPtr<ID3D12Resource> v;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors;
		std::array<UploadPlane, frame_count> y_upload;
		std::array<UploadPlane, frame_count> uv_upload;
		std::array<UploadPlane, frame_count> v_upload;
		D3D12_RESOURCE_STATES y_state = D3D12_RESOURCE_STATE_COPY_DEST;
		D3D12_RESOURCE_STATES uv_state = D3D12_RESOURCE_STATE_COPY_DEST;
		D3D12_RESOURCE_STATES v_state = D3D12_RESOURCE_STATE_COPY_DEST;
		D3D12_RESOURCE_STATES output_state = D3D12_RESOURCE_STATE_COMMON;
	};

	bool Initialize(ID3D12Device* device);
	explicit operator bool() const { return pipeline_ && root_signature_; }

	std::shared_ptr<Texture> Create(Layout layout, uint32_t width, uint32_t height);
	bool UpdateI420(Texture& texture, ID3D12GraphicsCommandList* commands, Dx12FrameSlot frame,
		const uint8_t* y, uint32_t y_stride,
		const uint8_t* u, const uint8_t* v, uint32_t uv_stride);
	bool UpdateNV12(Texture& texture, ID3D12GraphicsCommandList* commands, Dx12FrameSlot frame,
		const uint8_t* y, uint32_t y_stride,
		const uint8_t* uv, uint32_t uv_stride);

private:
	bool CreateOutput(Texture& texture);
	bool CreateCpuPlanes(Texture& texture);
	bool CreateDescriptors(Texture& texture);
	bool CreateUploadPlane(Texture::UploadPlane& upload, uint32_t width_bytes, uint32_t height);
	void WritePlane(Texture::UploadPlane& upload, const uint8_t* source,
		uint32_t source_stride, uint32_t width_bytes, uint32_t height);
	void CopyPlane(ID3D12GraphicsCommandList* commands, ID3D12Resource* destination,
		D3D12_RESOURCE_STATES& state, const Texture::UploadPlane& upload,
		uint32_t width, uint32_t height);
	void Dispatch(Texture& texture, ID3D12GraphicsCommandList* commands, uint32_t frame_index);
	void CreatePlaneSrv(Texture& texture, uint32_t frame_index, uint32_t descriptor,
		ID3D12Resource* resource, DXGI_FORMAT format, uint32_t plane_slice);

	Microsoft::WRL::ComPtr<ID3D12Device> device_;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
	uint32_t descriptor_size_ = 0;
};
