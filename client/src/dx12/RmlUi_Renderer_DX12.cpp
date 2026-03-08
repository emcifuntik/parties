// Phase 2-6: DX12 renderer — geometry, textures, scissor, transforms, stencil clip masks,
// layers, compositing, filters, custom shaders (gradients), postprocess effects.
// Shaders compiled at runtime via D3DCompile.

// clang-format off
#include "RmlUi_Include_Windows.h"
#include "RmlUi_Renderer_DX12.h"
// clang-format on

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Log.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dwmapi.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")

#include <parties/profiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const char* HrToString(HRESULT hr) {
	switch (hr) {
	case S_OK: return "S_OK";
	case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
	case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG";
	case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET";
	case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
	case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
	case E_INVALIDARG: return "E_INVALIDARG";
	default: return "UNKNOWN";
	}
}

#define DX_CHECK(expr, msg)                                                              \
	do {                                                                                  \
		HRESULT hr_ = (expr);                                                             \
		if (FAILED(hr_)) {                                                                \
			std::fprintf(stderr, "[DX12] %s failed: %s (0x%08lX) at %s:%d\n",           \
				(msg), HrToString(hr_), static_cast<unsigned long>(hr_),                 \
				__FILE__, __LINE__);                                                      \
			return false;                                                                 \
		}                                                                                 \
	} while (0)

#define DX_CHECK_VOID(expr, msg)                                                         \
	do {                                                                                  \
		HRESULT hr_ = (expr);                                                             \
		if (FAILED(hr_)) {                                                                \
			std::fprintf(stderr, "[DX12] %s failed: %s (0x%08lX) at %s:%d\n",           \
				(msg), HrToString(hr_), static_cast<unsigned long>(hr_),                 \
				__FILE__, __LINE__);                                                      \
			return;                                                                       \
		}                                                                                 \
	} while (0)

// ---------------------------------------------------------------------------
// Shader sources (compiled at runtime with D3DCompile)
// ---------------------------------------------------------------------------

static const char g_vs_source[] = R"HLSL(
cbuffer TransformCB : register(b0) {
	column_major float4x4 transform;
	float2 translate;
	float2 _pad;
};

struct VS_Input {
	float2 position : POSITION;
	float4 color    : COLOR0;
	float2 texcoord : TEXCOORD0;
};

struct VS_Output {
	float4 position : SV_POSITION;
	float4 color    : COLOR0;
	float2 texcoord : TEXCOORD0;
};

VS_Output main(VS_Input input) {
	VS_Output output;
	float2 pos = input.position + translate;
	output.position = mul(transform, float4(pos, 0.0, 1.0));
	output.color = input.color;
	output.texcoord = input.texcoord;
	return output;
}
)HLSL";

static const char g_ps_color_source[] = R"HLSL(
struct PS_Input {
	float4 position : SV_POSITION;
	float4 color    : COLOR0;
	float2 texcoord : TEXCOORD0;
};

float4 main(PS_Input input) : SV_TARGET {
	return input.color;
}
)HLSL";

static const char g_ps_texture_source[] = R"HLSL(
Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct PS_Input {
	float4 position : SV_POSITION;
	float4 color    : COLOR0;
	float2 texcoord : TEXCOORD0;
};

float4 main(PS_Input input) : SV_TARGET {
	return tex.Sample(samp, input.texcoord) * input.color;
}
)HLSL";

// Passthrough vertex shader for layer compositing.
// Positions are already in NDC (-1 to +1), no transform applied.
// Uses the same vertex layout as the main VS for simplicity (reuses Rml::Vertex struct).
static const char g_vs_passthrough_source[] = R"HLSL(
struct VS_Input {
	float2 position : POSITION;
	float4 color    : COLOR0;
	float2 texcoord : TEXCOORD0;
};

struct VS_Output {
	float4 position : SV_POSITION;
	float2 texcoord : TEXCOORD0;
};

VS_Output main(VS_Input input) {
	VS_Output output;
	output.position = float4(input.position, 0.0, 1.0);
	output.texcoord = input.texcoord;
	return output;
}
)HLSL";

// YUV pixel shader — samples 3 R8 textures (Y, U, V), converts to RGB in the shader.
// Uses the same coefficients as the CPU path for consistent color reproduction.
static const char g_ps_yuv_source[] = R"HLSL(
Texture2D tex_y : register(t0);
Texture2D tex_u : register(t1);
Texture2D tex_v : register(t2);
SamplerState samp : register(s0);

struct PS_Input {
	float4 position : SV_POSITION;
	float4 color    : COLOR0;
	float2 texcoord : TEXCOORD0;
};

float4 main(PS_Input input) : SV_TARGET {
	float Y = tex_y.Sample(samp, input.texcoord).r * 255.0;
	float U = tex_u.Sample(samp, input.texcoord).r * 255.0;
	float V = tex_v.Sample(samp, input.texcoord).r * 255.0;

	float C = Y - 16.0;
	float D = U - 128.0;
	float E = V - 128.0;

	float R = (298.0 * C + 459.0 * E + 128.0) / 65280.0;
	float G = (298.0 * C -  55.0 * D - 136.0 * E + 128.0) / 65280.0;
	float B = (298.0 * C + 541.0 * D + 128.0) / 65280.0;

	return float4(saturate(R), saturate(G), saturate(B), 1.0);
}
)HLSL";

// NV12 pixel shader — samples R8 Y texture (t0) and R8G8 UV texture (t1).
// UV plane is interleaved (U,V) pairs at half resolution, hardware bilinear filters.
static const char g_ps_nv12_source[] = R"HLSL(
Texture2D tex_y  : register(t0);
Texture2D tex_uv : register(t1);
SamplerState samp : register(s0);

struct PS_Input {
	float4 position : SV_POSITION;
	float4 color    : COLOR0;
	float2 texcoord : TEXCOORD0;
};

float4 main(PS_Input input) : SV_TARGET {
	float Y = tex_y.Sample(samp, input.texcoord).r * 255.0;
	float U = tex_uv.Sample(samp, input.texcoord).r * 255.0;
	float V = tex_uv.Sample(samp, input.texcoord).g * 255.0;

	float C = Y - 16.0;
	float D = U - 128.0;
	float E = V - 128.0;

	float R = (298.0 * C + 459.0 * E + 128.0) / 65280.0;
	float G = (298.0 * C -  55.0 * D - 136.0 * E + 128.0) / 65280.0;
	float B = (298.0 * C + 541.0 * D + 128.0) / 65280.0;

	return float4(saturate(R), saturate(G), saturate(B), 1.0);
}
)HLSL";

// Passthrough pixel shader — samples texture directly, no vertex color multiply.
static const char g_ps_passthrough_source[] = R"HLSL(
Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct PS_Input {
	float4 position : SV_POSITION;
	float2 texcoord : TEXCOORD0;
};

float4 main(PS_Input input) : SV_TARGET {
	return tex.Sample(samp, input.texcoord);
}
)HLSL";

// ---------------------------------------------------------------------------
// Phase 6 shader sources: color matrix, blur, drop shadow, blend mask, gradient
// ---------------------------------------------------------------------------

// Color matrix pixel shader — applies a 4x4 color matrix in premultiplied alpha space.
// The constant term (column 3) is multiplied by alpha to stay in premultiplied space.
static const char g_ps_color_matrix_source[] = R"HLSL(
cbuffer FilterCB : register(b1) {
	column_major float4x4 color_matrix;
};

Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct PS_Input {
	float4 position : SV_POSITION;
	float2 texcoord : TEXCOORD0;
};

float4 main(PS_Input input) : SV_TARGET {
	float4 texColor = tex.Sample(samp, input.texcoord);
	float3 transformed = mul(color_matrix, texColor).rgb;
	return float4(transformed, texColor.a);
}
)HLSL";

// Separable Gaussian blur pixel shader — 7-tap (center + 3 each side).
// texel_offset: (1/width, 0) for horizontal, (0, 1/height) for vertical.
static const char g_ps_blur_source[] = R"HLSL(
cbuffer FilterCB : register(b1) {
	float2 texel_offset;
	float2 _pad0;
	float4 weights;  // weights[0..3] packed as float4 (we use .x .y .z .w = w0 w1 w2 w3)
};

Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct PS_Input {
	float4 position : SV_POSITION;
	float2 texcoord : TEXCOORD0;
};

float4 main(PS_Input input) : SV_TARGET {
	float4 color = tex.Sample(samp, input.texcoord) * weights.x;
	[unroll] for (int i = 1; i < 4; i++) {
		float w = (i == 1) ? weights.y : (i == 2) ? weights.z : weights.w;
		float2 offset = texel_offset * float(i);
		color += tex.Sample(samp, input.texcoord + offset) * w;
		color += tex.Sample(samp, input.texcoord - offset) * w;
	}
	return color;
}
)HLSL";

// Drop shadow pixel shader — samples source with UV offset, multiplies alpha by shadow color.
static const char g_ps_drop_shadow_source[] = R"HLSL(
cbuffer FilterCB : register(b1) {
	float2 uv_offset;
	float2 _pad0;
	float4 shadow_color;
};

Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct PS_Input {
	float4 position : SV_POSITION;
	float2 texcoord : TEXCOORD0;
};

float4 main(PS_Input input) : SV_TARGET {
	float alpha = tex.Sample(samp, input.texcoord + uv_offset).a;
	return alpha * shadow_color;
}
)HLSL";

// Blend mask pixel shader — multiplies source by mask alpha.
static const char g_ps_blend_mask_source[] = R"HLSL(
Texture2D tex : register(t0);
Texture2D mask_tex : register(t1);
SamplerState samp : register(s0);

struct PS_Input {
	float4 position : SV_POSITION;
	float2 texcoord : TEXCOORD0;
};

float4 main(PS_Input input) : SV_TARGET {
	float4 texColor = tex.Sample(samp, input.texcoord);
	float maskAlpha = mask_tex.Sample(samp, input.texcoord).a;
	return texColor * maskAlpha;
}
)HLSL";

// Gradient pixel shader — linear, radial, conic (+ repeating variants).
// Uses root constants at b1 for all gradient parameters.
static const char g_ps_gradient_source[] = R"HLSL(
#define LINEAR 0
#define RADIAL 1
#define CONIC 2
#define REPEATING_LINEAR 3
#define REPEATING_RADIAL 4
#define REPEATING_CONIC 5
#define MAX_NUM_STOPS 16
#define PI 3.14159265

cbuffer GradientCB : register(b1) {
	int func;
	int num_stops;
	float2 p;       // start point / center
	float2 v;       // direction vector / curvature
	float2 _pad0;
	float4 stop_colors[MAX_NUM_STOPS];
	float stop_positions[MAX_NUM_STOPS];
};

struct PS_Input {
	float4 position : SV_POSITION;
	float4 color    : COLOR0;
	float2 texcoord : TEXCOORD0;
};

float4 mix_stop_colors(float t) {
	float4 color = stop_colors[0];
	for (int i = 1; i < num_stops; i++)
		color = lerp(color, stop_colors[i], smoothstep(stop_positions[i-1], stop_positions[i], t));
	return color;
}

float4 main(PS_Input input) : SV_TARGET {
	float t = 0.0;

	if (func == LINEAR || func == REPEATING_LINEAR) {
		float dist_square = dot(v, v);
		float2 V = input.texcoord - p;
		t = dot(v, V) / dist_square;
	} else if (func == RADIAL || func == REPEATING_RADIAL) {
		float2 V = input.texcoord - p;
		t = length(v * V);
	} else if (func == CONIC || func == REPEATING_CONIC) {
		float2x2 R = float2x2(v.x, v.y, -v.y, v.x);
		float2 V = mul(R, input.texcoord - p);
		t = 0.5 + atan2(-V.x, V.y) / (2.0 * PI);
	}

	if (func == REPEATING_LINEAR || func == REPEATING_RADIAL || func == REPEATING_CONIC) {
		float t0 = stop_positions[0];
		float t1 = stop_positions[num_stops - 1];
		t = t0 + fmod(t - t0, t1 - t0);
	}

	return input.color * mix_stop_colors(t);
}
)HLSL";

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr int MAX_NUM_STOPS = 16;
static constexpr int BLUR_SIZE = 7;
static constexpr int BLUR_NUM_WEIGHTS = (BLUR_SIZE + 1) / 2;  // 4

// ---------------------------------------------------------------------------
// Internal data structures
// ---------------------------------------------------------------------------

// Constant buffer layout matching HLSL TransformCB.
// 64 bytes (matrix) + 8 bytes (translate) + 8 bytes (pad) = 80 bytes.
struct alignas(256) TransformCB {
	float transform[16]; // column-major 4x4
	float translate[2];
	float _pad[2];
};
static_assert(sizeof(TransformCB) == 256, "CB must be 256-byte aligned for root CBV");

struct GeometryData {
	Microsoft::WRL::ComPtr<ID3D12Resource> vertex_buffer;
	Microsoft::WRL::ComPtr<ID3D12Resource> index_buffer;
	D3D12_VERTEX_BUFFER_VIEW vbv = {};
	D3D12_INDEX_BUFFER_VIEW ibv = {};
	int num_indices = 0;
};

struct TextureData {
	Microsoft::WRL::ComPtr<ID3D12Resource> texture;
	int32_t srv_index = -1;
	int width = 0;
	int height = 0;
	bool is_layer_texture = false; // true if this TextureData wraps a layer's color texture (SaveLayerAsTexture)
};

struct YUVUploadBuffer {
	Microsoft::WRL::ComPtr<ID3D12Resource> resource;
	void* mapped = nullptr;      // Persistently mapped pointer (never unmapped)
	UINT64 size = 0;             // Allocated buffer size
	UINT aligned_row_pitch = 0;  // D3D12-aligned row pitch
};

struct YUVTextureData {
	Microsoft::WRL::ComPtr<ID3D12Resource> y_texture, u_texture, v_texture;
	// Per-back-buffer upload buffers with persistent mapping
	static constexpr int NUM_UPLOAD_SETS = 2; // Must match NUM_BACK_BUFFERS
	YUVUploadBuffer y_upload[NUM_UPLOAD_SETS];
	YUVUploadBuffer u_upload[NUM_UPLOAD_SETS];
	YUVUploadBuffer v_upload[NUM_UPLOAD_SETS];
	int32_t y_srv = -1, u_srv = -1, v_srv = -1;
	int width = 0, height = 0;  // Full Y resolution; U/V are width/2 × height/2
};

struct NV12TextureData {
	Microsoft::WRL::ComPtr<ID3D12Resource> y_texture;   // R8_UNORM, width × height
	Microsoft::WRL::ComPtr<ID3D12Resource> uv_texture;  // R8G8_UNORM, width/2 × height/2
	static constexpr int NUM_UPLOAD_SETS = 2; // Must match NUM_BACK_BUFFERS
	YUVUploadBuffer y_upload[NUM_UPLOAD_SETS];
	YUVUploadBuffer uv_upload[NUM_UPLOAD_SETS];
	int32_t y_srv = -1, uv_srv = -1;
	int width = 0, height = 0;
};

struct RenderInterface_DX12::FrameUploadHeap {
	Microsoft::WRL::ComPtr<ID3D12Resource> resource;
	void* mapped = nullptr;
	UINT64 size = 0;
	UINT64 offset = 0;
};

struct LayerData {
	Microsoft::WRL::ComPtr<ID3D12Resource> color_texture; // R8G8B8A8_UNORM render target
	int32_t srv_index = -1;     // slot in SRV heap for reading as texture
	int width = 0;
	int height = 0;
};

// Postprocess render target for filter ping-ponging
struct PostprocessTarget {
	Microsoft::WRL::ComPtr<ID3D12Resource> texture;
	int32_t srv_index = -1;
	int rtv_slot = -1; // index in RTV heap (absolute)
	int width = 0;
	int height = 0;
};

// Compiled filter data stored by CompileFilter
enum class FilterType { Invalid = 0, Passthrough, Blur, DropShadow, ColorMatrix, MaskImage };
struct CompiledFilterData {
	FilterType type = FilterType::Invalid;

	// Passthrough (opacity)
	float blend_factor = 1.0f;

	// Blur
	float sigma = 0.0f;

	// Drop shadow
	Rml::Vector2f offset = {0.f, 0.f};
	Rml::ColourbPremultiplied color = {};

	// Color matrix
	Rml::Matrix4f color_matrix = Rml::Matrix4f::Identity();

	// MaskImage — stores a TextureData* pointing to the mask layer texture
	TextureData* mask_texture = nullptr;
};

// Gradient function types — must match shader defines
enum class ShaderGradientFunction { Linear, Radial, Conic, RepeatingLinear, RepeatingRadial, RepeatingConic };

// Compiled shader data stored by CompileShader
enum class CompiledShaderType { Invalid = 0, Gradient };
struct CompiledShaderData {
	CompiledShaderType type = CompiledShaderType::Invalid;

	// Gradient
	ShaderGradientFunction gradient_function = ShaderGradientFunction::Linear;
	Rml::Vector2f p = {0.f, 0.f};
	Rml::Vector2f v = {0.f, 0.f};
	Rml::Vector<float> stop_positions;
	Rml::Vector<Rml::Colourf> stop_colors;
};

// CB layout for gradient shader root constants.
// Must match GradientCB in HLSL. Max 64 DWORDs = 256 bytes for root constants.
// func(1) + num_stops(1) + p(2) + v(2) + _pad(2) + stop_colors(16*4=64) + stop_positions(16) = 88 DWORDs = 352 bytes
// That exceeds 64 DWORDs. Use a root CBV instead.
struct alignas(256) GradientCB {
	int func;
	int num_stops;
	float p[2];
	float v[2];
	float _pad0[2];
	float stop_colors[MAX_NUM_STOPS * 4]; // float4 per stop
	float stop_positions[MAX_NUM_STOPS];
};
static_assert(sizeof(GradientCB) >= 352, "GradientCB must fit all gradient data");

// CB layout for filter params (blur, color matrix, drop shadow).
// This goes into root CBV at b1.
struct alignas(256) FilterCB {
	float data[64]; // 256 bytes, enough for any filter CB
};
static_assert(sizeof(FilterCB) == 256, "FilterCB must be 256-byte aligned");

static Rml::Colourf ConvertToColorf(Rml::ColourbPremultiplied c0) {
	Rml::Colourf result;
	for (int i = 0; i < 4; i++)
		result[i] = (1.f / 255.f) * float(c0[i]);
	return result;
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

RenderInterface_DX12::RenderInterface_DX12(void* p_window_handle, const Backend::RmlRendererSettings& settings)
	: vsync_(settings.vsync), hwnd_(static_cast<HWND>(p_window_handle))
{
	RECT rc{};
	GetClientRect(hwnd_, &rc);
	int w = rc.right - rc.left;
	int h = rc.bottom - rc.top;
	if (w <= 0) w = 1;
	if (h <= 0) h = 1;

	if (!CreateDevice()) return;
	if (!CreateCommandQueue()) return;
	if (!CreateSwapChain(w, h)) return;
	if (!CreateRtvHeap()) return;
	if (!CreateDsvHeap()) return;
	if (!CreateSrvHeap()) return;
	if (!CreateCommandAllocatorsAndList()) return;
	if (!CreateFences()) return;

	CreateRenderTargetViews();
	if (!CreateDepthStencilBuffer()) return;

	if (!CreatePipelineState()) return;
	if (!CreateFullscreenQuad()) return;

	width_ = w;
	height_ = h;

	if (!CreateFrameUploadHeaps()) return;
	if (!CreatePostprocessTargets()) return;
	projection_ = Rml::Matrix4f::ProjectOrtho(0.0f, static_cast<float>(w),
		static_cast<float>(h), 0.0f, -10000.0f, 10000.0f);
	current_back_buffer_index_ = static_cast<IDXGISwapChain4*>(swap_chain_.Get())->GetCurrentBackBufferIndex();

	valid_ = true;
	std::printf("[DX12] Renderer initialized (%dx%d)\n", width_, height_);
}

RenderInterface_DX12::~RenderInterface_DX12() {
	if (valid_) {
		Flush();

		// Release postprocess targets
		ReleasePostprocessTargets();

		// Release all layers
		ReleaseAllLayers();

		// Release fullscreen quad
		if (fullscreen_quad_) {
			delete fullscreen_quad_;
			fullscreen_quad_ = nullptr;
		}

		// Release frame upload heaps
		if (frame_upload_heaps_) {
			for (int i = 0; i < NUM_BACK_BUFFERS; ++i) {
				auto& fuh = frame_upload_heaps_[i];
				if (fuh.mapped && fuh.resource) fuh.resource->Unmap(0, nullptr);
				fuh.resource.Reset();
			}
			delete[] frame_upload_heaps_;
			frame_upload_heaps_ = nullptr;
		}

		// Process all remaining deferred releases after GPU is idle
		for (auto& dr : deferred_releases_) {
			if (dr.srv_index >= 0) FreeSrvSlot(dr.srv_index);
			dr.resource.Reset();
		}
		deferred_releases_.clear();
	}

	if (frame_latency_waitable_) {
		CloseHandle(frame_latency_waitable_);
		frame_latency_waitable_ = nullptr;
	}
	if (fence_event_) {
		CloseHandle(fence_event_);
		fence_event_ = nullptr;
	}
}

RenderInterface_DX12::operator bool() const {
	return valid_;
}

// ---------------------------------------------------------------------------
// Device creation
// ---------------------------------------------------------------------------

bool RenderInterface_DX12::CreateDevice() {
	UINT dxgi_flags = 0;

#if defined(_DEBUG) || defined(RMLUI_DX_DEBUG)
	{
		ComPtr<ID3D12Debug> debug_controller;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller)))) {
			debug_controller->EnableDebugLayer();
			dxgi_flags |= DXGI_CREATE_FACTORY_DEBUG;
			std::printf("[DX12] Debug layer enabled\n");
		}
	}
#endif

	DX_CHECK(CreateDXGIFactory2(dxgi_flags, IID_PPV_ARGS(&factory_)),
		"CreateDXGIFactory2");

	// Enumerate adapters, prefer hardware
	ComPtr<IDXGIAdapter1> adapter;
	ComPtr<IDXGIFactory6> factory6;
	if (SUCCEEDED(factory_.As(&factory6))) {
		for (UINT i = 0;
			 SUCCEEDED(factory6->EnumAdapterByGpuPreference(
				 i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
			 ++i) {
			DXGI_ADAPTER_DESC1 desc{};
			adapter->GetDesc1(&desc);
			if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
				adapter.Reset();
				continue;
			}
			if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
					__uuidof(ID3D12Device), nullptr))) {
				std::printf("[DX12] Using adapter: %ls\n", desc.Description);
				break;
			}
			adapter.Reset();
		}
	}

	if (!adapter) {
		// Fallback: enumerate without preference
		for (UINT i = 0; SUCCEEDED(factory_->EnumAdapters1(i, &adapter)); ++i) {
			DXGI_ADAPTER_DESC1 desc{};
			adapter->GetDesc1(&desc);
			if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
				adapter.Reset();
				continue;
			}
			if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
					__uuidof(ID3D12Device), nullptr))) {
				std::printf("[DX12] Using adapter (fallback): %ls\n", desc.Description);
				break;
			}
			adapter.Reset();
		}
	}

	if (!adapter) {
		std::fprintf(stderr, "[DX12] No suitable adapter found\n");
		return false;
	}

	DX_CHECK(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)),
		"D3D12CreateDevice");

	// Name the device for debug tooling
	device_->SetName(L"RmlUi_DX12_Device");

	return true;
}

// ---------------------------------------------------------------------------
// Command queue
// ---------------------------------------------------------------------------

bool RenderInterface_DX12::CreateCommandQueue() {
	D3D12_COMMAND_QUEUE_DESC desc{};
	desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	desc.NodeMask = 0;

	DX_CHECK(device_->CreateCommandQueue(&desc, IID_PPV_ARGS(&command_queue_)),
		"CreateCommandQueue");
	command_queue_->SetName(L"RmlUi_DirectQueue");

	return true;
}

// ---------------------------------------------------------------------------
// Swap chain
// ---------------------------------------------------------------------------

bool RenderInterface_DX12::CreateSwapChain(int width, int height) {
	DXGI_SWAP_CHAIN_DESC1 sc_desc{};
	sc_desc.Width = static_cast<UINT>(width);
	sc_desc.Height = static_cast<UINT>(height);
	sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sc_desc.Stereo = FALSE;
	sc_desc.SampleDesc.Count = 1;
	sc_desc.SampleDesc.Quality = 0;
	sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sc_desc.BufferCount = NUM_BACK_BUFFERS;
	sc_desc.Scaling = DXGI_SCALING_STRETCH;
	sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sc_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	sc_desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

	// Use CreateSwapChainForComposition + DirectComposition for proper VRR/G-Sync
	// frame pacing. With HWND swap chains, G-Sync makes Present non-blocking,
	// causing uncapped FPS. DComp lets DWM manage presentation timing correctly.
	ComPtr<IDXGISwapChain1> swap_chain1;
	DX_CHECK(factory_->CreateSwapChainForComposition(
		command_queue_.Get(), &sc_desc, nullptr, &swap_chain1),
		"CreateSwapChainForComposition");

	// Disable Alt+Enter fullscreen toggle
	DX_CHECK(factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER),
		"MakeWindowAssociation");

	DX_CHECK(swap_chain1.As(&swap_chain_), "QueryInterface IDXGISwapChain4");

	swap_chain_->SetMaximumFrameLatency(NUM_BACK_BUFFERS);
	frame_latency_waitable_ = swap_chain_->GetFrameLatencyWaitableObject();

	// Set up DirectComposition visual tree: DComp device -> target (HWND) -> visual -> swap chain
	DX_CHECK(DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&dcomp_device_)),
		"DCompositionCreateDevice");

	DX_CHECK(dcomp_device_->CreateTargetForHwnd(hwnd_, TRUE, &dcomp_target_),
		"CreateTargetForHwnd");

	DX_CHECK(dcomp_device_->CreateVisual(&dcomp_visual_),
		"CreateVisual");

	DX_CHECK(dcomp_visual_->SetContent(swap_chain_.Get()),
		"SetContent(SwapChain)");

	DX_CHECK(dcomp_target_->SetRoot(dcomp_visual_.Get()),
		"SetRoot");

	DX_CHECK(dcomp_device_->Commit(),
		"DComp Commit");

	std::printf("[DX12] DirectComposition swap chain created\n");

	return true;
}

// ---------------------------------------------------------------------------
// RTV descriptor heap
// ---------------------------------------------------------------------------

bool RenderInterface_DX12::CreateRtvHeap() {
	D3D12_DESCRIPTOR_HEAP_DESC desc{};
	desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	desc.NumDescriptors = RTV_HEAP_SIZE; // back buffers + layer render targets
	desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

	DX_CHECK(device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&rtv_heap_)),
		"CreateDescriptorHeap(RTV)");
	rtv_heap_->SetName(L"RmlUi_RTV_Heap");

	rtv_descriptor_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	return true;
}

// ---------------------------------------------------------------------------
// DSV descriptor heap (for depth/stencil)
// ---------------------------------------------------------------------------

bool RenderInterface_DX12::CreateDsvHeap() {
	D3D12_DESCRIPTOR_HEAP_DESC desc{};
	desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	desc.NumDescriptors = 1;
	desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

	DX_CHECK(device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&dsv_heap_)),
		"CreateDescriptorHeap(DSV)");
	dsv_heap_->SetName(L"RmlUi_DSV_Heap");

	return true;
}

// ---------------------------------------------------------------------------
// Depth/stencil buffer
// ---------------------------------------------------------------------------

bool RenderInterface_DX12::CreateDepthStencilBuffer() {
	// Release old buffer if recreating on resize
	depth_stencil_buffer_.Reset();

	// Use stored viewport dimensions — GetClientRect may return stale values during transitions
	int w = width_ > 0 ? width_ : 1;
	int h = height_ > 0 ? height_ : 1;

	D3D12_HEAP_PROPERTIES heap_props{};
	heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC ds_desc{};
	ds_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	ds_desc.Width = static_cast<UINT64>(w);
	ds_desc.Height = static_cast<UINT>(h);
	ds_desc.DepthOrArraySize = 1;
	ds_desc.MipLevels = 1;
	ds_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	ds_desc.SampleDesc.Count = 1;
	ds_desc.SampleDesc.Quality = 0;
	ds_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	ds_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE clear_value{};
	clear_value.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	clear_value.DepthStencil.Depth = 1.0f;
	clear_value.DepthStencil.Stencil = 0;

	DX_CHECK(device_->CreateCommittedResource(
		&heap_props, D3D12_HEAP_FLAG_NONE, &ds_desc,
		D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear_value,
		IID_PPV_ARGS(&depth_stencil_buffer_)),
		"CreateCommittedResource(DepthStencil)");
	depth_stencil_buffer_->SetName(L"RmlUi_DepthStencil");

	// Create DSV
	D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
	dsv_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dsv_desc.Texture2D.MipSlice = 0;
	dsv_desc.Flags = D3D12_DSV_FLAG_NONE;

	device_->CreateDepthStencilView(depth_stencil_buffer_.Get(), &dsv_desc,
		dsv_heap_->GetCPUDescriptorHandleForHeapStart());

	return true;
}

// ---------------------------------------------------------------------------
// SRV descriptor heap (shader-visible, for texture binding)
// ---------------------------------------------------------------------------

bool RenderInterface_DX12::CreateSrvHeap() {
	D3D12_DESCRIPTOR_HEAP_DESC desc{};
	desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	desc.NumDescriptors = SRV_HEAP_SIZE;
	desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

	DX_CHECK(device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&srv_heap_)),
		"CreateDescriptorHeap(SRV)");
	srv_heap_->SetName(L"RmlUi_SRV_Heap");

	srv_descriptor_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	// Initialize free list (all slots available, in reverse order so pop_back gives low indices first)
	srv_free_list_.reserve(SRV_HEAP_SIZE);
	for (int32_t i = SRV_HEAP_SIZE - 1; i >= 0; --i) {
		srv_free_list_.push_back(i);
	}

	return true;
}

int32_t RenderInterface_DX12::AllocateSrvSlot() {
	if (srv_free_list_.empty()) {
		std::fprintf(stderr, "[DX12] SRV descriptor heap exhausted (%d slots)\n", SRV_HEAP_SIZE);
		return -1;
	}
	int32_t idx = srv_free_list_.back();
	srv_free_list_.pop_back();
	return idx;
}

void RenderInterface_DX12::FreeSrvSlot(int32_t index) {
	if (index >= 0 && index < SRV_HEAP_SIZE) {
		srv_free_list_.push_back(index);
	}
}

// ---------------------------------------------------------------------------
// Command allocators and command list
// ---------------------------------------------------------------------------

bool RenderInterface_DX12::CreateCommandAllocatorsAndList() {
	for (int i = 0; i < NUM_BACK_BUFFERS; ++i) {
		DX_CHECK(device_->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocators_[i])),
			"CreateCommandAllocator");

		wchar_t name[64];
		swprintf(name, 64, L"RmlUi_CmdAllocator_%d", i);
		command_allocators_[i]->SetName(name);
	}

	// Create command list in closed state — BeginFrame will reset it
	DX_CHECK(device_->CreateCommandList(
		0, D3D12_COMMAND_LIST_TYPE_DIRECT,
		command_allocators_[0].Get(), nullptr,
		IID_PPV_ARGS(&command_list_)),
		"CreateCommandList");
	command_list_->SetName(L"RmlUi_CmdList");

	// Close immediately — BeginFrame will reset+open it
	command_list_->Close();

	return true;
}

// ---------------------------------------------------------------------------
// Fences
// ---------------------------------------------------------------------------

bool RenderInterface_DX12::CreateFences() {
	DX_CHECK(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)),
		"CreateFence");
	fence_->SetName(L"RmlUi_Fence");

	fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!fence_event_) {
		std::fprintf(stderr, "[DX12] CreateEvent failed\n");
		return false;
	}

	for (auto& v : fence_values_) v = 0;
	next_fence_value_ = 1;

	return true;
}

// ---------------------------------------------------------------------------
// Render target views
// ---------------------------------------------------------------------------

void RenderInterface_DX12::CreateRenderTargetViews() {
	D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();

	for (int i = 0; i < NUM_BACK_BUFFERS; ++i) {
		HRESULT hr = swap_chain_->GetBuffer(i, IID_PPV_ARGS(&back_buffers_[i]));
		if (FAILED(hr)) {
			std::fprintf(stderr, "[DX12] GetBuffer(%d) failed: 0x%08lX\n",
				i, static_cast<unsigned long>(hr));
			return;
		}

		device_->CreateRenderTargetView(back_buffers_[i].Get(), nullptr, rtv_handle);

		wchar_t name[64];
		swprintf(name, 64, L"RmlUi_BackBuffer_%d", i);
		back_buffers_[i]->SetName(name);

		rtv_handle.ptr += rtv_descriptor_size_;
	}
}

// ---------------------------------------------------------------------------
// Pipeline state: root signature + PSOs
// ---------------------------------------------------------------------------

static Microsoft::WRL::ComPtr<ID3DBlob> CompileHLSL(const char* source, size_t length,
	const char* entry, const char* target, const char* name) {
	Microsoft::WRL::ComPtr<ID3DBlob> blob;
	Microsoft::WRL::ComPtr<ID3DBlob> errors;

	UINT flags = 0;
#if defined(_DEBUG) || defined(RMLUI_DX_DEBUG)
	flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	HRESULT hr = D3DCompile(source, length, name, nullptr, nullptr,
		entry, target, flags, 0, &blob, &errors);

	if (FAILED(hr)) {
		const char* err_msg = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown";
		std::fprintf(stderr, "[DX12] Shader compilation failed (%s): %s\n", name, err_msg);
		return nullptr;
	}

	return blob;
}

bool RenderInterface_DX12::CreatePipelineState() {
	// --- Compile shaders ---
	auto vs_blob = CompileHLSL(g_vs_source, sizeof(g_vs_source) - 1, "main", "vs_5_0", "RmlUi_VS");
	if (!vs_blob) return false;

	auto ps_color_blob = CompileHLSL(g_ps_color_source, sizeof(g_ps_color_source) - 1, "main", "ps_5_0", "RmlUi_PS_Color");
	if (!ps_color_blob) return false;

	auto ps_texture_blob = CompileHLSL(g_ps_texture_source, sizeof(g_ps_texture_source) - 1, "main", "ps_5_0", "RmlUi_PS_Texture");
	if (!ps_texture_blob) return false;

	// --- Root signature ---
	// Parameter 0: Root CBV at b0 (TransformCB) — vertex shader
	// Parameter 1: Descriptor table with 1 SRV at t0 — pixel shader
	// Parameter 2: Root CBV at b1 (FilterCB/GradientCB) — pixel shader
	// Parameter 3: Descriptor table with 1 SRV at t1 — pixel shader (mask texture)
	// Static sampler: linear filter, clamp mode at s0

	D3D12_DESCRIPTOR_RANGE1 srv_range{};
	srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srv_range.NumDescriptors = 1;
	srv_range.BaseShaderRegister = 0;  // t0
	srv_range.RegisterSpace = 0;
	srv_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
	srv_range.OffsetInDescriptorsFromTableStart = 0;

	D3D12_DESCRIPTOR_RANGE1 srv_range_mask{};
	srv_range_mask.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srv_range_mask.NumDescriptors = 1;
	srv_range_mask.BaseShaderRegister = 1;  // t1
	srv_range_mask.RegisterSpace = 0;
	srv_range_mask.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
	srv_range_mask.OffsetInDescriptorsFromTableStart = 0;

	D3D12_ROOT_PARAMETER1 root_params[4] = {};

	// Param 0: Root CBV at b0 (TransformCB)
	root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	root_params[0].Descriptor.ShaderRegister = 0; // b0
	root_params[0].Descriptor.RegisterSpace = 0;
	root_params[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;
	root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

	// Param 1: Descriptor table for SRV t0
	root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	root_params[1].DescriptorTable.NumDescriptorRanges = 1;
	root_params[1].DescriptorTable.pDescriptorRanges = &srv_range;
	root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	// Param 2: Root CBV at b1 (FilterCB / GradientCB)
	root_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	root_params[2].Descriptor.ShaderRegister = 1; // b1
	root_params[2].Descriptor.RegisterSpace = 0;
	root_params[2].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;
	root_params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	// Param 3: Descriptor table for SRV t1 (mask texture)
	root_params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	root_params[3].DescriptorTable.NumDescriptorRanges = 1;
	root_params[3].DescriptorTable.pDescriptorRanges = &srv_range_mask;
	root_params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	// Static sampler: bilinear, clamp
	D3D12_STATIC_SAMPLER_DESC sampler{};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MipLODBias = 0.0f;
	sampler.MaxAnisotropy = 1;
	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
	sampler.MinLOD = 0.0f;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderRegister = 0;  // s0
	sampler.RegisterSpace = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs_desc{};
	rs_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
	rs_desc.Desc_1_1.NumParameters = 4;
	rs_desc.Desc_1_1.pParameters = root_params;
	rs_desc.Desc_1_1.NumStaticSamplers = 1;
	rs_desc.Desc_1_1.pStaticSamplers = &sampler;
	rs_desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	ComPtr<ID3DBlob> rs_blob;
	ComPtr<ID3DBlob> rs_error;
	DX_CHECK(D3D12SerializeVersionedRootSignature(&rs_desc, &rs_blob, &rs_error),
		"D3D12SerializeVersionedRootSignature");

	DX_CHECK(device_->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
		IID_PPV_ARGS(&root_signature_)),
		"CreateRootSignature");
	root_signature_->SetName(L"RmlUi_RootSignature");

	// --- Input layout ---
	// Rml::Vertex: position (float2, 8B), colour (byte4, 4B), tex_coord (float2, 8B) = 20B total
	D3D12_INPUT_ELEMENT_DESC input_layout[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,     0, 8,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	// --- Shared PSO desc ---
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
	pso_desc.pRootSignature = root_signature_.Get();
	pso_desc.VS = { vs_blob->GetBufferPointer(), vs_blob->GetBufferSize() };
	pso_desc.InputLayout = { input_layout, _countof(input_layout) };
	pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

	// Premultiplied alpha blend
	pso_desc.BlendState.AlphaToCoverageEnable = FALSE;
	pso_desc.BlendState.IndependentBlendEnable = FALSE;
	auto& rt_blend = pso_desc.BlendState.RenderTarget[0];
	rt_blend.BlendEnable = TRUE;
	rt_blend.SrcBlend = D3D12_BLEND_ONE;
	rt_blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	rt_blend.BlendOp = D3D12_BLEND_OP_ADD;
	rt_blend.SrcBlendAlpha = D3D12_BLEND_ONE;
	rt_blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	rt_blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	rt_blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	// Rasterizer
	pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pso_desc.RasterizerState.FrontCounterClockwise = FALSE;
	pso_desc.RasterizerState.DepthBias = 0;
	pso_desc.RasterizerState.DepthBiasClamp = 0.0f;
	pso_desc.RasterizerState.SlopeScaledDepthBias = 0.0f;
	pso_desc.RasterizerState.DepthClipEnable = TRUE;
	pso_desc.RasterizerState.MultisampleEnable = FALSE;
	pso_desc.RasterizerState.AntialiasedLineEnable = FALSE;
	pso_desc.RasterizerState.ForcedSampleCount = 0;
	pso_desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

	// Depth: always disabled (we only use stencil).
	// All PSOs must have DSVFormat set since we always bind the DSV.
	pso_desc.DepthStencilState.DepthEnable = FALSE;
	pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	pso_desc.DepthStencilState.StencilEnable = FALSE;
	pso_desc.DepthStencilState.StencilReadMask = 0xFF;
	pso_desc.DepthStencilState.StencilWriteMask = 0xFF;

	// Default stencil ops (KEEP everything)
	D3D12_DEPTH_STENCILOP_DESC stencil_keep = {};
	stencil_keep.StencilFailOp = D3D12_STENCIL_OP_KEEP;
	stencil_keep.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	stencil_keep.StencilPassOp = D3D12_STENCIL_OP_KEEP;
	stencil_keep.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
	pso_desc.DepthStencilState.FrontFace = stencil_keep;
	pso_desc.DepthStencilState.BackFace = stencil_keep;

	// Output
	pso_desc.NumRenderTargets = 1;
	pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	pso_desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	pso_desc.SampleDesc.Count = 1;
	pso_desc.SampleDesc.Quality = 0;
	pso_desc.SampleMask = UINT_MAX;

	// ---------------------------------------------------------------
	// 1. Color PSO (stencil disabled) — normal rendering without clip mask
	// ---------------------------------------------------------------
	pso_desc.DepthStencilState.StencilEnable = FALSE;
	pso_desc.PS = { ps_color_blob->GetBufferPointer(), ps_color_blob->GetBufferSize() };
	DX_CHECK(device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso_color_)),
		"CreateGraphicsPipelineState(Color)");
	pso_color_->SetName(L"RmlUi_PSO_Color");

	// ---------------------------------------------------------------
	// 2. Texture PSO (stencil disabled) — normal rendering without clip mask
	// ---------------------------------------------------------------
	pso_desc.PS = { ps_texture_blob->GetBufferPointer(), ps_texture_blob->GetBufferSize() };
	DX_CHECK(device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso_texture_)),
		"CreateGraphicsPipelineState(Texture)");
	pso_texture_->SetName(L"RmlUi_PSO_Texture");

	// ---------------------------------------------------------------
	// 3. Color PSO with stencil EQUAL test — rendering content inside clip mask
	// ---------------------------------------------------------------
	pso_desc.DepthStencilState.StencilEnable = TRUE;
	pso_desc.DepthStencilState.FrontFace = stencil_keep;  // EQUAL test, all ops KEEP
	pso_desc.DepthStencilState.BackFace = stencil_keep;
	pso_desc.PS = { ps_color_blob->GetBufferPointer(), ps_color_blob->GetBufferSize() };
	rt_blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	DX_CHECK(device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso_color_stencil_)),
		"CreateGraphicsPipelineState(ColorStencil)");
	pso_color_stencil_->SetName(L"RmlUi_PSO_ColorStencil");

	// ---------------------------------------------------------------
	// 4. Texture PSO with stencil EQUAL test — rendering content inside clip mask
	// ---------------------------------------------------------------
	pso_desc.PS = { ps_texture_blob->GetBufferPointer(), ps_texture_blob->GetBufferSize() };
	DX_CHECK(device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso_texture_stencil_)),
		"CreateGraphicsPipelineState(TextureStencil)");
	pso_texture_stencil_->SetName(L"RmlUi_PSO_TextureStencil");

	// ---------------------------------------------------------------
	// 5. Stencil Set PSO — write stencil with REPLACE, no color output
	//    Used for Set and SetInverse operations.
	// ---------------------------------------------------------------
	D3D12_DEPTH_STENCILOP_DESC stencil_replace = {};
	stencil_replace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
	stencil_replace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	stencil_replace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
	stencil_replace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

	pso_desc.DepthStencilState.StencilEnable = TRUE;
	pso_desc.DepthStencilState.FrontFace = stencil_replace;
	pso_desc.DepthStencilState.BackFace = stencil_replace;
	rt_blend.RenderTargetWriteMask = 0;  // No color writes
	pso_desc.PS = { ps_color_blob->GetBufferPointer(), ps_color_blob->GetBufferSize() };
	DX_CHECK(device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso_stencil_set_)),
		"CreateGraphicsPipelineState(StencilSet)");
	pso_stencil_set_->SetName(L"RmlUi_PSO_StencilSet");

	// ---------------------------------------------------------------
	// 6. Stencil Intersect PSO — increment stencil, no color output
	//    Used for Intersect operation.
	// ---------------------------------------------------------------
	D3D12_DEPTH_STENCILOP_DESC stencil_incr = {};
	stencil_incr.StencilFailOp = D3D12_STENCIL_OP_KEEP;
	stencil_incr.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	stencil_incr.StencilPassOp = D3D12_STENCIL_OP_INCR_SAT;
	stencil_incr.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;

	pso_desc.DepthStencilState.FrontFace = stencil_incr;
	pso_desc.DepthStencilState.BackFace = stencil_incr;
	DX_CHECK(device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso_stencil_intersect_)),
		"CreateGraphicsPipelineState(StencilIntersect)");
	pso_stencil_intersect_->SetName(L"RmlUi_PSO_StencilIntersect");

	// ---------------------------------------------------------------
	// 7 & 8. Passthrough PSOs for layer compositing
	// ---------------------------------------------------------------
	auto vs_pt_blob = CompileHLSL(g_vs_passthrough_source, sizeof(g_vs_passthrough_source) - 1,
		"main", "vs_5_0", "RmlUi_VS_Passthrough");
	if (!vs_pt_blob) return false;

	auto ps_pt_blob = CompileHLSL(g_ps_passthrough_source, sizeof(g_ps_passthrough_source) - 1,
		"main", "ps_5_0", "RmlUi_PS_Passthrough");
	if (!ps_pt_blob) return false;

	// Reset PSO desc for passthrough: no stencil, no depth, same root signature
	pso_desc.VS = { vs_pt_blob->GetBufferPointer(), vs_pt_blob->GetBufferSize() };
	pso_desc.PS = { ps_pt_blob->GetBufferPointer(), ps_pt_blob->GetBufferSize() };
	pso_desc.DepthStencilState.StencilEnable = FALSE;
	pso_desc.DepthStencilState.DepthEnable = FALSE;
	pso_desc.DSVFormat = DXGI_FORMAT_UNKNOWN; // No DSV bound during compositing

	// 7. Passthrough Blend — premultiplied alpha blending
	rt_blend.BlendEnable = TRUE;
	rt_blend.SrcBlend = D3D12_BLEND_ONE;
	rt_blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	rt_blend.BlendOp = D3D12_BLEND_OP_ADD;
	rt_blend.SrcBlendAlpha = D3D12_BLEND_ONE;
	rt_blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	rt_blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	rt_blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	DX_CHECK(device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso_passthrough_blend_)),
		"CreateGraphicsPipelineState(PassthroughBlend)");
	pso_passthrough_blend_->SetName(L"RmlUi_PSO_PassthroughBlend");

	// 8. Passthrough Replace — no blending, overwrite destination
	rt_blend.BlendEnable = FALSE;
	rt_blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	DX_CHECK(device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso_passthrough_replace_)),
		"CreateGraphicsPipelineState(PassthroughReplace)");
	pso_passthrough_replace_->SetName(L"RmlUi_PSO_PassthroughReplace");

	// ---------------------------------------------------------------
	// Phase 6 PSOs: filters and shaders
	// All use passthrough VS, DSVFormat=UNKNOWN, no stencil
	// ---------------------------------------------------------------

	// 9. Color Matrix — no blend (overwrite)
	auto ps_color_matrix_blob = CompileHLSL(g_ps_color_matrix_source, sizeof(g_ps_color_matrix_source) - 1,
		"main", "ps_5_0", "RmlUi_PS_ColorMatrix");
	if (!ps_color_matrix_blob) return false;

	pso_desc.PS = { ps_color_matrix_blob->GetBufferPointer(), ps_color_matrix_blob->GetBufferSize() };
	rt_blend.BlendEnable = FALSE;
	rt_blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	DX_CHECK(device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso_color_matrix_)),
		"CreateGraphicsPipelineState(ColorMatrix)");
	pso_color_matrix_->SetName(L"RmlUi_PSO_ColorMatrix");

	// 10. Blur — no blend (overwrite)
	auto ps_blur_blob = CompileHLSL(g_ps_blur_source, sizeof(g_ps_blur_source) - 1,
		"main", "ps_5_0", "RmlUi_PS_Blur");
	if (!ps_blur_blob) return false;

	pso_desc.PS = { ps_blur_blob->GetBufferPointer(), ps_blur_blob->GetBufferSize() };
	DX_CHECK(device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso_blur_)),
		"CreateGraphicsPipelineState(Blur)");
	pso_blur_->SetName(L"RmlUi_PSO_Blur");

	// 11. Drop Shadow — no blend (overwrite into postprocess target)
	auto ps_drop_shadow_blob = CompileHLSL(g_ps_drop_shadow_source, sizeof(g_ps_drop_shadow_source) - 1,
		"main", "ps_5_0", "RmlUi_PS_DropShadow");
	if (!ps_drop_shadow_blob) return false;

	pso_desc.PS = { ps_drop_shadow_blob->GetBufferPointer(), ps_drop_shadow_blob->GetBufferSize() };
	DX_CHECK(device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso_drop_shadow_)),
		"CreateGraphicsPipelineState(DropShadow)");
	pso_drop_shadow_->SetName(L"RmlUi_PSO_DropShadow");

	// 12. Blend Mask — no blend (overwrite)
	auto ps_blend_mask_blob = CompileHLSL(g_ps_blend_mask_source, sizeof(g_ps_blend_mask_source) - 1,
		"main", "ps_5_0", "RmlUi_PS_BlendMask");
	if (!ps_blend_mask_blob) return false;

	pso_desc.PS = { ps_blend_mask_blob->GetBufferPointer(), ps_blend_mask_blob->GetBufferSize() };
	DX_CHECK(device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso_blend_mask_)),
		"CreateGraphicsPipelineState(BlendMask)");
	pso_blend_mask_->SetName(L"RmlUi_PSO_BlendMask");

	// 13. Gradient — uses main VS with transform, premultiplied alpha blend, with DSV
	auto ps_gradient_blob = CompileHLSL(g_ps_gradient_source, sizeof(g_ps_gradient_source) - 1,
		"main", "ps_5_0", "RmlUi_PS_Gradient");
	if (!ps_gradient_blob) return false;

	// Switch back to main VS and full PSO settings for gradient
	pso_desc.VS = { vs_blob->GetBufferPointer(), vs_blob->GetBufferSize() };
	pso_desc.PS = { ps_gradient_blob->GetBufferPointer(), ps_gradient_blob->GetBufferSize() };
	pso_desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	pso_desc.DepthStencilState.StencilEnable = FALSE;
	pso_desc.DepthStencilState.DepthEnable = FALSE;
	rt_blend.BlendEnable = TRUE;
	rt_blend.SrcBlend = D3D12_BLEND_ONE;
	rt_blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	rt_blend.BlendOp = D3D12_BLEND_OP_ADD;
	rt_blend.SrcBlendAlpha = D3D12_BLEND_ONE;
	rt_blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	rt_blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	rt_blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	DX_CHECK(device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso_gradient_)),
		"CreateGraphicsPipelineState(Gradient)");
	pso_gradient_->SetName(L"RmlUi_PSO_Gradient");

	// 14. Gradient with stencil test — for rendering inside clip masks
	pso_desc.DepthStencilState.StencilEnable = TRUE;
	pso_desc.DepthStencilState.FrontFace = stencil_keep;
	pso_desc.DepthStencilState.BackFace = stencil_keep;
	DX_CHECK(device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso_gradient_stencil_)),
		"CreateGraphicsPipelineState(GradientStencil)");
	pso_gradient_stencil_->SetName(L"RmlUi_PSO_GradientStencil");

	// ---------------------------------------------------------------
	// 15 & 16. YUV video PSOs — 3 R8 textures → RGB in pixel shader
	// Uses a separate root signature with 3 SRV descriptor tables.
	// ---------------------------------------------------------------
	{
		auto ps_yuv_blob = CompileHLSL(g_ps_yuv_source, sizeof(g_ps_yuv_source) - 1,
			"main", "ps_5_0", "RmlUi_PS_YUV");
		if (!ps_yuv_blob) return false;

		// YUV root signature: CBV@b0 + 3 separate descriptor tables (t0,t1,t2)
		D3D12_DESCRIPTOR_RANGE1 yuv_ranges[3] = {};
		for (int i = 0; i < 3; i++) {
			yuv_ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			yuv_ranges[i].NumDescriptors = 1;
			yuv_ranges[i].BaseShaderRegister = static_cast<UINT>(i); // t0, t1, t2
			yuv_ranges[i].RegisterSpace = 0;
			yuv_ranges[i].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
			yuv_ranges[i].OffsetInDescriptorsFromTableStart = 0;
		}

		D3D12_ROOT_PARAMETER1 yuv_params[4] = {};
		// Param 0: Root CBV at b0 (TransformCB)
		yuv_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		yuv_params[0].Descriptor.ShaderRegister = 0;
		yuv_params[0].Descriptor.RegisterSpace = 0;
		yuv_params[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;
		yuv_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
		// Params 1-3: Descriptor tables for t0(Y), t1(U), t2(V)
		for (int i = 0; i < 3; i++) {
			yuv_params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			yuv_params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
			yuv_params[1 + i].DescriptorTable.pDescriptorRanges = &yuv_ranges[i];
			yuv_params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		}

		D3D12_VERSIONED_ROOT_SIGNATURE_DESC yuv_rs_desc{};
		yuv_rs_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
		yuv_rs_desc.Desc_1_1.NumParameters = 4;
		yuv_rs_desc.Desc_1_1.pParameters = yuv_params;
		yuv_rs_desc.Desc_1_1.NumStaticSamplers = 1;
		yuv_rs_desc.Desc_1_1.pStaticSamplers = &sampler; // reuse same bilinear sampler
		yuv_rs_desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

		ComPtr<ID3DBlob> yuv_rs_blob, yuv_rs_error;
		DX_CHECK(D3D12SerializeVersionedRootSignature(&yuv_rs_desc, &yuv_rs_blob, &yuv_rs_error),
			"D3D12SerializeVersionedRootSignature(YUV)");
		DX_CHECK(device_->CreateRootSignature(0, yuv_rs_blob->GetBufferPointer(), yuv_rs_blob->GetBufferSize(),
			IID_PPV_ARGS(&yuv_root_signature_)),
			"CreateRootSignature(YUV)");
		yuv_root_signature_->SetName(L"RmlUi_YUV_RootSignature");

		// YUV PSO — same vertex layout, premultiplied alpha blend, with DSV
		D3D12_GRAPHICS_PIPELINE_STATE_DESC yuv_pso{};
		yuv_pso.pRootSignature = yuv_root_signature_.Get();
		yuv_pso.VS = { vs_blob->GetBufferPointer(), vs_blob->GetBufferSize() };
		yuv_pso.PS = { ps_yuv_blob->GetBufferPointer(), ps_yuv_blob->GetBufferSize() };
		yuv_pso.InputLayout = { input_layout, _countof(input_layout) };
		yuv_pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		yuv_pso.BlendState.RenderTarget[0].BlendEnable = FALSE; // Video is opaque
		yuv_pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		yuv_pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		yuv_pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		yuv_pso.RasterizerState.DepthClipEnable = TRUE;
		yuv_pso.DepthStencilState.DepthEnable = FALSE;
		yuv_pso.DepthStencilState.StencilEnable = FALSE;
		yuv_pso.DepthStencilState.StencilReadMask = 0xFF;
		yuv_pso.DepthStencilState.StencilWriteMask = 0xFF;
		yuv_pso.DepthStencilState.FrontFace = stencil_keep;
		yuv_pso.DepthStencilState.BackFace = stencil_keep;
		yuv_pso.NumRenderTargets = 1;
		yuv_pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		yuv_pso.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
		yuv_pso.SampleDesc.Count = 1;
		yuv_pso.SampleMask = UINT_MAX;

		DX_CHECK(device_->CreateGraphicsPipelineState(&yuv_pso, IID_PPV_ARGS(&pso_yuv_)),
			"CreateGraphicsPipelineState(YUV)");
		pso_yuv_->SetName(L"RmlUi_PSO_YUV");

		// 16. YUV with stencil EQUAL test
		yuv_pso.DepthStencilState.StencilEnable = TRUE;
		DX_CHECK(device_->CreateGraphicsPipelineState(&yuv_pso, IID_PPV_ARGS(&pso_yuv_stencil_)),
			"CreateGraphicsPipelineState(YUVStencil)");
		pso_yuv_stencil_->SetName(L"RmlUi_PSO_YUVStencil");
	}

	// ---------------------------------------------------------------
	// 17 & 18. NV12 video PSOs — R8 Y + R8G8 UV → RGB in pixel shader
	// Uses a root signature with 2 SRV descriptor tables (t0, t1).
	// ---------------------------------------------------------------
	{
		auto ps_nv12_blob = CompileHLSL(g_ps_nv12_source, sizeof(g_ps_nv12_source) - 1,
			"main", "ps_5_0", "RmlUi_PS_NV12");
		if (!ps_nv12_blob) return false;

		// NV12 root signature: CBV@b0 + 2 descriptor tables (t0, t1)
		D3D12_DESCRIPTOR_RANGE1 nv12_ranges[2] = {};
		for (int i = 0; i < 2; i++) {
			nv12_ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			nv12_ranges[i].NumDescriptors = 1;
			nv12_ranges[i].BaseShaderRegister = static_cast<UINT>(i);
			nv12_ranges[i].RegisterSpace = 0;
			nv12_ranges[i].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
			nv12_ranges[i].OffsetInDescriptorsFromTableStart = 0;
		}

		D3D12_ROOT_PARAMETER1 nv12_params[3] = {};
		// Param 0: Root CBV at b0 (TransformCB)
		nv12_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		nv12_params[0].Descriptor.ShaderRegister = 0;
		nv12_params[0].Descriptor.RegisterSpace = 0;
		nv12_params[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;
		nv12_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
		// Params 1-2: Descriptor tables for t0(Y), t1(UV)
		for (int i = 0; i < 2; i++) {
			nv12_params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			nv12_params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
			nv12_params[1 + i].DescriptorTable.pDescriptorRanges = &nv12_ranges[i];
			nv12_params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		}

		D3D12_VERSIONED_ROOT_SIGNATURE_DESC nv12_rs_desc{};
		nv12_rs_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
		nv12_rs_desc.Desc_1_1.NumParameters = 3;
		nv12_rs_desc.Desc_1_1.pParameters = nv12_params;
		nv12_rs_desc.Desc_1_1.NumStaticSamplers = 1;
		nv12_rs_desc.Desc_1_1.pStaticSamplers = &sampler;
		nv12_rs_desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

		ComPtr<ID3DBlob> nv12_sig_blob, nv12_sig_err;
		DX_CHECK(D3D12SerializeVersionedRootSignature(&nv12_rs_desc,
			&nv12_sig_blob, &nv12_sig_err),
			"D3D12SerializeVersionedRootSignature(NV12)");
		DX_CHECK(device_->CreateRootSignature(0,
			nv12_sig_blob->GetBufferPointer(), nv12_sig_blob->GetBufferSize(),
			IID_PPV_ARGS(&nv12_root_signature_)),
			"CreateRootSignature(NV12)");
		nv12_root_signature_->SetName(L"RmlUi_NV12_RootSignature");

		D3D12_GRAPHICS_PIPELINE_STATE_DESC nv12_pso{};
		nv12_pso.pRootSignature = nv12_root_signature_.Get();
		nv12_pso.VS = { vs_blob->GetBufferPointer(), vs_blob->GetBufferSize() };
		nv12_pso.PS = { ps_nv12_blob->GetBufferPointer(), ps_nv12_blob->GetBufferSize() };
		nv12_pso.InputLayout = { input_layout, _countof(input_layout) };
		nv12_pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		nv12_pso.BlendState.RenderTarget[0].BlendEnable = FALSE;
		nv12_pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		nv12_pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		nv12_pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		nv12_pso.RasterizerState.DepthClipEnable = TRUE;
		nv12_pso.DepthStencilState.DepthEnable = FALSE;
		nv12_pso.DepthStencilState.StencilEnable = FALSE;
		nv12_pso.DepthStencilState.StencilReadMask = 0xFF;
		nv12_pso.DepthStencilState.StencilWriteMask = 0xFF;
		nv12_pso.DepthStencilState.FrontFace = stencil_keep;
		nv12_pso.DepthStencilState.BackFace = stencil_keep;
		nv12_pso.NumRenderTargets = 1;
		nv12_pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		nv12_pso.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
		nv12_pso.SampleDesc.Count = 1;
		nv12_pso.SampleMask = UINT_MAX;

		DX_CHECK(device_->CreateGraphicsPipelineState(&nv12_pso, IID_PPV_ARGS(&pso_nv12_)),
			"CreateGraphicsPipelineState(NV12)");
		pso_nv12_->SetName(L"RmlUi_PSO_NV12");

		nv12_pso.DepthStencilState.StencilEnable = TRUE;
		DX_CHECK(device_->CreateGraphicsPipelineState(&nv12_pso, IID_PPV_ARGS(&pso_nv12_stencil_)),
			"CreateGraphicsPipelineState(NV12Stencil)");
		pso_nv12_stencil_->SetName(L"RmlUi_PSO_NV12Stencil");
	}

	return true;
}

// ---------------------------------------------------------------------------
// Release back buffer references (before ResizeBuffers)
// ---------------------------------------------------------------------------

void RenderInterface_DX12::ReleaseBackBufferResources() {
	for (auto& bb : back_buffers_) bb.Reset();
}

// ---------------------------------------------------------------------------
// Fullscreen quad (for layer compositing)
// ---------------------------------------------------------------------------

bool RenderInterface_DX12::CreateFullscreenQuad() {
	// Fullscreen quad in NDC. Uses Rml::Vertex for compatibility with input layout.
	// (-1, +1) = top-left in DX NDC, (+1, -1) = bottom-right
	Rml::Vertex vertices[4];

	// Top-left
	vertices[0].position = {-1.0f, +1.0f};
	vertices[0].colour = {255, 255, 255, 255};
	vertices[0].tex_coord = {0.0f, 0.0f};
	// Top-right
	vertices[1].position = {+1.0f, +1.0f};
	vertices[1].colour = {255, 255, 255, 255};
	vertices[1].tex_coord = {1.0f, 0.0f};
	// Bottom-left
	vertices[2].position = {-1.0f, -1.0f};
	vertices[2].colour = {255, 255, 255, 255};
	vertices[2].tex_coord = {0.0f, 1.0f};
	// Bottom-right
	vertices[3].position = {+1.0f, -1.0f};
	vertices[3].colour = {255, 255, 255, 255};
	vertices[3].tex_coord = {1.0f, 1.0f};

	int indices[6] = { 0, 1, 2, 2, 1, 3 };

	auto handle = CompileGeometry(
		Rml::Span<const Rml::Vertex>(vertices, 4),
		Rml::Span<const int>(indices, 6));
	if (!handle) return false;

	fullscreen_quad_ = reinterpret_cast<GeometryData*>(handle);
	return true;
}

// ---------------------------------------------------------------------------
// Upload buffer helper
// ---------------------------------------------------------------------------

RenderInterface_DX12::ComPtr<ID3D12Resource> RenderInterface_DX12::CreateUploadBuffer(UINT size, const void* data) {
	D3D12_HEAP_PROPERTIES upload_heap{};
	upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

	// Round up to 256-byte alignment for CBV
	UINT aligned_size = (size + 255) & ~255u;

	D3D12_RESOURCE_DESC buf_desc{};
	buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buf_desc.Width = aligned_size;
	buf_desc.Height = 1;
	buf_desc.DepthOrArraySize = 1;
	buf_desc.MipLevels = 1;
	buf_desc.Format = DXGI_FORMAT_UNKNOWN;
	buf_desc.SampleDesc.Count = 1;
	buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	ComPtr<ID3D12Resource> resource;
	HRESULT hr = device_->CreateCommittedResource(
		&upload_heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
		IID_PPV_ARGS(&resource));
	if (FAILED(hr)) {
		std::fprintf(stderr, "[DX12] Upload buffer creation failed: 0x%08lX\n",
			static_cast<unsigned long>(hr));
		return nullptr;
	}

	if (data) {
		void* mapped = nullptr;
		D3D12_RANGE read_range{0, 0};
		hr = resource->Map(0, &read_range, &mapped);
		if (FAILED(hr)) {
			std::fprintf(stderr, "[DX12] Upload buffer Map failed: 0x%08lX\n",
				static_cast<unsigned long>(hr));
			return nullptr;
		}
		std::memcpy(mapped, data, size);
		resource->Unmap(0, nullptr);
	}

	return resource;
}

// ---------------------------------------------------------------------------
// Per-frame linear upload heap (eliminates per-draw CreateCommittedResource)
// ---------------------------------------------------------------------------

bool RenderInterface_DX12::CreateFrameUploadHeaps() {
	frame_upload_heaps_ = new FrameUploadHeap[NUM_BACK_BUFFERS]();

	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_UPLOAD;

	D3D12_RESOURCE_DESC buf_desc{};
	buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buf_desc.Width = FRAME_UPLOAD_HEAP_SIZE;
	buf_desc.Height = 1;
	buf_desc.DepthOrArraySize = 1;
	buf_desc.MipLevels = 1;
	buf_desc.Format = DXGI_FORMAT_UNKNOWN;
	buf_desc.SampleDesc.Count = 1;
	buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	for (int i = 0; i < NUM_BACK_BUFFERS; ++i) {
		auto& fuh = frame_upload_heaps_[i];
		DX_CHECK(device_->CreateCommittedResource(
			&heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
			IID_PPV_ARGS(&fuh.resource)),
			"CreateCommittedResource(FrameUploadHeap)");

		D3D12_RANGE read_range{0, 0};
		DX_CHECK(fuh.resource->Map(0, &read_range, &fuh.mapped),
			"Map(FrameUploadHeap)");
		fuh.size = FRAME_UPLOAD_HEAP_SIZE;
		fuh.offset = 0;
	}
	return true;
}

RenderInterface_DX12::CbAllocation RenderInterface_DX12::AllocateCB(uint32_t size, const void* data) {
	uint32_t aligned_size = (size + 255) & ~255u;
	auto& fuh = frame_upload_heaps_[current_back_buffer_index_];

	if (fuh.offset + aligned_size > fuh.size) {
		std::fprintf(stderr, "[DX12] Frame upload heap exhausted (%llu / %llu)\n",
			fuh.offset + aligned_size, fuh.size);
		return {0, nullptr};
	}

	CbAllocation alloc{};
	alloc.gpu_address = fuh.resource->GetGPUVirtualAddress() + fuh.offset;
	alloc.cpu_ptr = static_cast<uint8_t*>(fuh.mapped) + fuh.offset;

	if (data)
		std::memcpy(alloc.cpu_ptr, data, size);

	fuh.offset += aligned_size;
	return alloc;
}

// ---------------------------------------------------------------------------
// Postprocess render targets (for filter ping-pong)
// ---------------------------------------------------------------------------

bool RenderInterface_DX12::CreatePostprocessTargets() {
	for (int i = 0; i < NUM_POSTPROCESS_TARGETS; ++i) {
		auto* pp = new PostprocessTarget();
		pp->width = width_;
		pp->height = height_;

		// RTV slot: after back buffers and layers
		pp->rtv_slot = NUM_BACK_BUFFERS + MAX_LAYER_COUNT + i;

		// Allocate SRV
		pp->srv_index = AllocateSrvSlot();
		if (pp->srv_index < 0) {
			delete pp;
			return false;
		}

		// Create texture
		D3D12_HEAP_PROPERTIES default_heap{};
		default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

		D3D12_RESOURCE_DESC tex_desc{};
		tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		tex_desc.Width = static_cast<UINT64>(width_);
		tex_desc.Height = static_cast<UINT>(height_);
		tex_desc.DepthOrArraySize = 1;
		tex_desc.MipLevels = 1;
		tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		tex_desc.SampleDesc.Count = 1;
		tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

		D3D12_CLEAR_VALUE clear_value{};
		clear_value.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		clear_value.Color[0] = 0.0f;
		clear_value.Color[1] = 0.0f;
		clear_value.Color[2] = 0.0f;
		clear_value.Color[3] = 0.0f;

		HRESULT hr = device_->CreateCommittedResource(
			&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear_value,
			IID_PPV_ARGS(&pp->texture));
		if (FAILED(hr)) {
			std::fprintf(stderr, "[DX12] Postprocess texture creation failed: 0x%08lX\n",
				static_cast<unsigned long>(hr));
			FreeSrvSlot(pp->srv_index);
			delete pp;
			return false;
		}

		wchar_t name[64];
		swprintf(name, 64, L"RmlUi_Postprocess_%d", i);
		pp->texture->SetName(name);

		// Create RTV
		D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
		rtv_handle.ptr += static_cast<SIZE_T>(pp->rtv_slot) * rtv_descriptor_size_;
		device_->CreateRenderTargetView(pp->texture.Get(), nullptr, rtv_handle);

		// Create SRV
		D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
		srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv_desc.Texture2D.MipLevels = 1;
		srv_desc.Texture2D.MostDetailedMip = 0;

		D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = srv_heap_->GetCPUDescriptorHandleForHeapStart();
		cpu_handle.ptr += static_cast<SIZE_T>(pp->srv_index) * srv_descriptor_size_;
		device_->CreateShaderResourceView(pp->texture.Get(), &srv_desc, cpu_handle);

		postprocess_targets_[i] = pp;
	}

	return true;
}

void RenderInterface_DX12::ReleasePostprocessTargets() {
	for (int i = 0; i < NUM_POSTPROCESS_TARGETS; ++i) {
		if (postprocess_targets_[i]) {
			if (postprocess_targets_[i]->srv_index >= 0)
				FreeSrvSlot(postprocess_targets_[i]->srv_index);
			delete postprocess_targets_[i];
			postprocess_targets_[i] = nullptr;
		}
	}
}

// ---------------------------------------------------------------------------
// Layer management
// ---------------------------------------------------------------------------

// Helper: compute RTV handle for a layer. Layer RTVs start after back buffer RTVs.
static D3D12_CPU_DESCRIPTOR_HANDLE GetLayerRTV(ID3D12DescriptorHeap* rtv_heap,
	uint32_t rtv_descriptor_size, int layer_index) {
	D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
	handle.ptr += static_cast<SIZE_T>(RenderInterface_DX12::NUM_BACK_BUFFERS + layer_index) * rtv_descriptor_size;
	return handle;
}

int RenderInterface_DX12::AllocateLayer() {
	// Find a free slot in the pool
	for (size_t i = 0; i < layer_in_use_.size(); ++i) {
		if (!layer_in_use_[i]) {
			auto& layer = layer_pool_[i];
			// Check if dimensions match current viewport
			if (layer.width == width_ && layer.height == height_) {
				layer_in_use_[i] = true;
				return static_cast<int>(i);
			}
			// Dimensions changed — release old resources and recreate
			if (layer.srv_index >= 0) FreeSrvSlot(layer.srv_index);
			layer.color_texture.Reset();
			layer.srv_index = -1;
			layer.width = 0;
			layer.height = 0;
		}
	}

	// Need to allocate a new layer or reuse a cleared slot
	int slot = -1;
	for (size_t i = 0; i < layer_pool_.size(); ++i) {
		if (!layer_in_use_[i] && !layer_pool_[i].color_texture) {
			slot = static_cast<int>(i);
			break;
		}
	}

	if (slot < 0) {
		if (static_cast<int>(layer_pool_.size()) >= MAX_LAYER_COUNT) {
			std::fprintf(stderr, "[DX12] Layer pool exhausted (%d layers)\n", MAX_LAYER_COUNT);
			return -1;
		}
		slot = static_cast<int>(layer_pool_.size());
		layer_pool_.emplace_back();
		layer_in_use_.push_back(false);
	}

	auto& layer = layer_pool_[slot];

	// Allocate SRV slot
	int32_t srv_slot = AllocateSrvSlot();
	if (srv_slot < 0) return -1;
	layer.srv_index = srv_slot;

	// Create color texture
	D3D12_HEAP_PROPERTIES default_heap{};
	default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC tex_desc{};
	tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	tex_desc.Width = static_cast<UINT64>(width_);
	tex_desc.Height = static_cast<UINT>(height_);
	tex_desc.DepthOrArraySize = 1;
	tex_desc.MipLevels = 1;
	tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	tex_desc.SampleDesc.Count = 1;
	tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	D3D12_CLEAR_VALUE clear_value{};
	clear_value.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	clear_value.Color[0] = 0.0f;
	clear_value.Color[1] = 0.0f;
	clear_value.Color[2] = 0.0f;
	clear_value.Color[3] = 0.0f;

	HRESULT hr = device_->CreateCommittedResource(
		&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
		D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
		IID_PPV_ARGS(&layer.color_texture));
	if (FAILED(hr)) {
		std::fprintf(stderr, "[DX12] Layer texture creation failed: 0x%08lX\n",
			static_cast<unsigned long>(hr));
		FreeSrvSlot(srv_slot);
		layer.srv_index = -1;
		return -1;
	}

	wchar_t name[64];
	swprintf(name, 64, L"RmlUi_Layer_%d", slot);
	layer.color_texture->SetName(name);

	layer.width = width_;
	layer.height = height_;

	// Create RTV for this layer
	device_->CreateRenderTargetView(layer.color_texture.Get(), nullptr,
		GetLayerRTV(rtv_heap_.Get(), rtv_descriptor_size_, slot));

	// Create SRV for reading as texture during compositing
	D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
	srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv_desc.Texture2D.MipLevels = 1;
	srv_desc.Texture2D.MostDetailedMip = 0;

	D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = srv_heap_->GetCPUDescriptorHandleForHeapStart();
	cpu_handle.ptr += static_cast<SIZE_T>(srv_slot) * srv_descriptor_size_;
	device_->CreateShaderResourceView(layer.color_texture.Get(), &srv_desc, cpu_handle);

	layer_in_use_[slot] = true;
	return slot;
}

void RenderInterface_DX12::FreeLayer(int layer_index) {
	if (layer_index >= 0 && layer_index < static_cast<int>(layer_in_use_.size())) {
		layer_in_use_[layer_index] = false;
	}
}

void RenderInterface_DX12::ReleaseAllLayers() {
	for (size_t i = 0; i < layer_pool_.size(); ++i) {
		auto& layer = layer_pool_[i];
		if (layer.srv_index >= 0) FreeSrvSlot(layer.srv_index);
		layer.color_texture.Reset();
		layer.srv_index = -1;
		layer.width = 0;
		layer.height = 0;
	}
	layer_pool_.clear();
	layer_in_use_.clear();
	layer_stack_.clear();
}

void RenderInterface_DX12::SetRenderTargetToLayer(int layer_index) {
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetLayerRTV(rtv_heap_.Get(), rtv_descriptor_size_, layer_index);
	D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
	command_list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
}

void RenderInterface_DX12::SetRenderTargetToBackBuffer() {
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += static_cast<SIZE_T>(current_back_buffer_index_) * rtv_descriptor_size_;
	D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
	command_list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
}

// ---------------------------------------------------------------------------
// GPU synchronization
// ---------------------------------------------------------------------------

void RenderInterface_DX12::WaitForFenceValue(uint64_t value) {
	if (fence_->GetCompletedValue() < value) {
		fence_->SetEventOnCompletion(value, fence_event_);
		WaitForSingleObjectEx(fence_event_, INFINITE, FALSE);
	}
}

void RenderInterface_DX12::Flush() {
	// Signal and wait for all pending GPU work
	for (int i = 0; i < NUM_BACK_BUFFERS; ++i) {
		uint64_t signal_value = next_fence_value_++;
		command_queue_->Signal(fence_.Get(), signal_value);
		WaitForFenceValue(signal_value);
		fence_values_[i] = signal_value;
	}
}

// ---------------------------------------------------------------------------
// Deferred deletion
// ---------------------------------------------------------------------------

void RenderInterface_DX12::DeferRelease(ComPtr<ID3D12Resource> resource, int32_t srv_index) {
	// Tag with the fence value that will be signaled when the current frame's
	// command list finishes executing (signaled in EndFrame).
	// next_fence_value_ is the value that WILL be used at the next Signal call.
	deferred_releases_.push_back({ std::move(resource), srv_index, next_fence_value_ });
}

void RenderInterface_DX12::ProcessDeferredReleases() {
	ZoneScopedN("DX12::ProcessDeferredReleases");
	// Release resources whose tagged fence value has been completed by the GPU.
	uint64_t completed = fence_->GetCompletedValue();
	auto it = std::remove_if(deferred_releases_.begin(), deferred_releases_.end(),
		[&](DeferredRelease& dr) {
			if (completed >= dr.fence_value) {
				if (dr.srv_index >= 0) FreeSrvSlot(dr.srv_index);
				dr.resource.Reset();
				return true;
			}
			return false;
		});
	deferred_releases_.erase(it, deferred_releases_.end());
}

// ---------------------------------------------------------------------------
// SetViewport — resize swap chain
// ---------------------------------------------------------------------------

void RenderInterface_DX12::SetViewport(int viewport_width, int viewport_height, bool force) {
	ZoneScopedN("DX12::SetViewport");
	if (viewport_width <= 0 || viewport_height <= 0) return;
	if (!force && viewport_width == width_ && viewport_height == height_) return;

	// Wait for all GPU work to finish before resizing
	Flush();

	// Drain all deferred releases now that GPU is idle
	ProcessDeferredReleases();

	// Release postprocess targets — recreated after resize
	ReleasePostprocessTargets();

	// Release all layers — they'll be recreated at new dimensions when needed
	ReleaseAllLayers();

	// Release back buffer references
	ReleaseBackBufferResources();

	HRESULT hr = swap_chain_->ResizeBuffers(
		NUM_BACK_BUFFERS,
		static_cast<UINT>(viewport_width),
		static_cast<UINT>(viewport_height),
		DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);

	if (FAILED(hr)) {
		std::fprintf(stderr, "[DX12] ResizeBuffers failed: %s (0x%08lX)\n",
			HrToString(hr), static_cast<unsigned long>(hr));
		valid_ = false;
		return;
	}

	// Update dimensions before recreating dependent resources
	width_ = viewport_width;
	height_ = viewport_height;
	current_back_buffer_index_ = swap_chain_->GetCurrentBackBufferIndex();

	// Recreate RTVs for the new buffers
	CreateRenderTargetViews();

	// Recreate depth/stencil buffer at new dimensions
	CreateDepthStencilBuffer();

	// Update projection matrix for new dimensions
	projection_ = Rml::Matrix4f::ProjectOrtho(0.0f, static_cast<float>(viewport_width),
		static_cast<float>(viewport_height), 0.0f, -10000.0f, 10000.0f);

	// Recreate postprocess targets at new dimensions
	CreatePostprocessTargets();

	// Notify DComp that the swap chain was resized
	if (dcomp_device_)
		dcomp_device_->Commit();
}


// ---------------------------------------------------------------------------
// BeginFrame
// ---------------------------------------------------------------------------

void RenderInterface_DX12::BeginFrame() {
	ZoneScopedN("DX12::BeginFrame");

	// Wait on frame latency waitable object (limits CPU queue depth)
	{
		ZoneScopedN("DX12::WaitFrameLatency");
		if (frame_latency_waitable_)
			WaitForSingleObjectEx(frame_latency_waitable_, 1000, TRUE);
	}

	// Wait until the GPU has finished with this frame's command allocator
	{
		ZoneScopedN("DX12::WaitFence");
		WaitForFenceValue(fence_values_[current_back_buffer_index_]);
	}
	// Reset per-frame upload heap for this back buffer
	frame_upload_heaps_[current_back_buffer_index_].offset = 0;

	// Free deferred releases from the previous use of this back buffer slot
	ProcessDeferredReleases();

	// Reset command allocator and command list for this frame
	HRESULT hr = command_allocators_[current_back_buffer_index_]->Reset();
	if (FAILED(hr)) {
		std::fprintf(stderr, "[DX12] CommandAllocator::Reset failed: 0x%08lX\n",
			static_cast<unsigned long>(hr));
		return;
	}

	hr = command_list_->Reset(command_allocators_[current_back_buffer_index_].Get(), nullptr);
	if (FAILED(hr)) {
		std::fprintf(stderr, "[DX12] CommandList::Reset failed: 0x%08lX\n",
			static_cast<unsigned long>(hr));
		return;
	}

	// Transition back buffer: PRESENT -> RENDER_TARGET
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = back_buffers_[current_back_buffer_index_].Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	command_list_->ResourceBarrier(1, &barrier);

	// Set viewport
	D3D12_VIEWPORT viewport{};
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;
	viewport.Width = static_cast<float>(width_);
	viewport.Height = static_cast<float>(height_);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	command_list_->RSSetViewports(1, &viewport);

	// Default scissor to full viewport
	scissor_rect_.left = 0;
	scissor_rect_.top = 0;
	scissor_rect_.right = static_cast<LONG>(width_);
	scissor_rect_.bottom = static_cast<LONG>(height_);
	command_list_->RSSetScissorRects(1, &scissor_rect_);

	// Set render target with depth/stencil
	D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
	rtv_handle.ptr += static_cast<SIZE_T>(current_back_buffer_index_) * rtv_descriptor_size_;
	D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
	command_list_->OMSetRenderTargets(1, &rtv_handle, FALSE, &dsv_handle);

	// Bind root signature and SRV heap for the whole frame
	command_list_->SetGraphicsRootSignature(root_signature_.Get());
	ID3D12DescriptorHeap* heaps[] = { srv_heap_.Get() };
	command_list_->SetDescriptorHeaps(1, heaps);

	// Set topology (triangle list) for the frame
	command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------

void RenderInterface_DX12::Clear() {
	ZoneScopedN("DX12::Clear");
	// Dark background: #0d0e17
	const float clear_color[4] = { 0.051f, 0.055f, 0.090f, 1.0f };

	D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
	rtv_handle.ptr += static_cast<SIZE_T>(current_back_buffer_index_) * rtv_descriptor_size_;
	command_list_->ClearRenderTargetView(rtv_handle, clear_color, 0, nullptr);

	// Clear stencil to 0
	D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
	command_list_->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

	// Reset clip mask state for the new frame
	clip_mask_enabled_ = false;
	stencil_ref_ = 0;
}

// ---------------------------------------------------------------------------
// EndFrame — barrier, close, execute, present, signal fence
// ---------------------------------------------------------------------------

void RenderInterface_DX12::EndFrame() {
	ZoneScopedN("DX12::EndFrame");
	// Transition back buffer: RENDER_TARGET -> PRESENT
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = back_buffers_[current_back_buffer_index_].Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	command_list_->ResourceBarrier(1, &barrier);

	// Close and execute
	HRESULT hr = command_list_->Close();
	if (FAILED(hr)) {
		std::fprintf(stderr, "[DX12] CommandList::Close failed: 0x%08lX\n",
			static_cast<unsigned long>(hr));
		return;
	}

	ID3D12CommandList* lists[] = { command_list_.Get() };
	command_queue_->ExecuteCommandLists(1, lists);

	// Signal fence for this frame
	uint64_t signal_value = next_fence_value_++;
	hr = command_queue_->Signal(fence_.Get(), signal_value);
	if (FAILED(hr)) {
		std::fprintf(stderr, "[DX12] Signal failed: 0x%08lX\n",
			static_cast<unsigned long>(hr));
	}
	fence_values_[current_back_buffer_index_] = signal_value;

	HRESULT present_hr;
	{
		ZoneScopedN("DX12::Present");
		present_hr = swap_chain_->Present(vsync_ ? 1 : 0, 0);
	}
	hr = present_hr;
	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
		HRESULT reason = device_->GetDeviceRemovedReason();
		std::fprintf(stderr, "[DX12] Device removed during Present: %s (0x%08lX)\n",
			HrToString(reason), static_cast<unsigned long>(reason));
		valid_ = false;
		return;
	}
	if (FAILED(hr)) {
		std::fprintf(stderr, "[DX12] Present FAILED: %s (0x%08lX)\n",
			HrToString(hr), static_cast<unsigned long>(hr));
	}

	// Force vblank sync — DwmFlush blocks until the compositor presents.
	// Needed because G-Sync/FreeSync makes Present non-blocking even with SyncInterval=1.
	if (vsync_) {
		ZoneScopedN("DX12::DwmFlush");
		DwmFlush();
	}

	// Update back buffer index for next frame
	current_back_buffer_index_ = swap_chain_->GetCurrentBackBufferIndex();
	FrameMark;
}

// ===========================================================================
// Geometry
// ===========================================================================

Rml::CompiledGeometryHandle RenderInterface_DX12::CompileGeometry(
	Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) {
	ZoneScopedN("DX12::CompileGeometry");


	if (vertices.empty() || indices.empty()) return Rml::CompiledGeometryHandle(0);

	auto* geom = new GeometryData();

	const UINT vb_size = static_cast<UINT>(vertices.size() * sizeof(Rml::Vertex));
	const UINT ib_size = static_cast<UINT>(indices.size() * sizeof(int));

	D3D12_HEAP_PROPERTIES upload_heap{};
	upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

	D3D12_RESOURCE_DESC buf_desc{};
	buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buf_desc.Width = vb_size;
	buf_desc.Height = 1;
	buf_desc.DepthOrArraySize = 1;
	buf_desc.MipLevels = 1;
	buf_desc.Format = DXGI_FORMAT_UNKNOWN;
	buf_desc.SampleDesc.Count = 1;
	buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	HRESULT hr = device_->CreateCommittedResource(
		&upload_heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
		IID_PPV_ARGS(&geom->vertex_buffer));
	if (FAILED(hr)) {
		std::fprintf(stderr, "[DX12] VB creation failed: 0x%08lX\n", static_cast<unsigned long>(hr));
		delete geom;
		return Rml::CompiledGeometryHandle(0);
	}
	geom->vertex_buffer->SetName(L"RmlUi_VB");

	void* mapped = nullptr;
	D3D12_RANGE read_range{0, 0};
	hr = geom->vertex_buffer->Map(0, &read_range, &mapped);
	if (FAILED(hr)) {
		std::fprintf(stderr, "[DX12] VB Map failed: 0x%08lX\n", static_cast<unsigned long>(hr));
		delete geom;
		return Rml::CompiledGeometryHandle(0);
	}
	std::memcpy(mapped, vertices.data(), vb_size);
	geom->vertex_buffer->Unmap(0, nullptr);

	buf_desc.Width = ib_size;
	hr = device_->CreateCommittedResource(
		&upload_heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
		IID_PPV_ARGS(&geom->index_buffer));
	if (FAILED(hr)) {
		std::fprintf(stderr, "[DX12] IB creation failed: 0x%08lX\n", static_cast<unsigned long>(hr));
		delete geom;
		return Rml::CompiledGeometryHandle(0);
	}
	geom->index_buffer->SetName(L"RmlUi_IB");

	hr = geom->index_buffer->Map(0, &read_range, &mapped);
	if (FAILED(hr)) {
		std::fprintf(stderr, "[DX12] IB Map failed: 0x%08lX\n", static_cast<unsigned long>(hr));
		delete geom;
		return Rml::CompiledGeometryHandle(0);
	}
	std::memcpy(mapped, indices.data(), ib_size);
	geom->index_buffer->Unmap(0, nullptr);

	geom->vbv.BufferLocation = geom->vertex_buffer->GetGPUVirtualAddress();
	geom->vbv.SizeInBytes = vb_size;
	geom->vbv.StrideInBytes = sizeof(Rml::Vertex);

	geom->ibv.BufferLocation = geom->index_buffer->GetGPUVirtualAddress();
	geom->ibv.SizeInBytes = ib_size;
	geom->ibv.Format = DXGI_FORMAT_R32_UINT;

	geom->num_indices = static_cast<int>(indices.size());

	return reinterpret_cast<Rml::CompiledGeometryHandle>(geom);
}

void RenderInterface_DX12::UpdateGeometryVertices(
	Rml::CompiledGeometryHandle geometry, Rml::Span<const Rml::Vertex> vertices) {
	if (!geometry || vertices.empty()) return;
	auto* geom = reinterpret_cast<GeometryData*>(geometry);
	const UINT vb_size = static_cast<UINT>(vertices.size() * sizeof(Rml::Vertex));
	void* mapped = nullptr;
	D3D12_RANGE read_range{0, 0};
	HRESULT hr = geom->vertex_buffer->Map(0, &read_range, &mapped);
	if (SUCCEEDED(hr)) {
		std::memcpy(mapped, vertices.data(), vb_size);
		geom->vertex_buffer->Unmap(0, nullptr);
	}
}

void RenderInterface_DX12::RenderGeometry(
	Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
	Rml::TextureHandle texture) {
	ZoneScopedN("DX12::RenderGeometry");


	if (!geometry) return;

	auto* geom = reinterpret_cast<GeometryData*>(geometry);
	auto* tex = reinterpret_cast<TextureData*>(texture);

	// --- Build transform constant buffer ---
	// Compute final_transform = projection * model_transform
	Rml::Matrix4f mvp;
	if (transform_active_) {
		mvp = projection_ * transform_;
	} else {
		mvp = projection_;
	}

	// Create a temporary upload buffer for the constant data.
	// This buffer is deferred-released after the GPU finishes this frame.
	TransformCB cb_data{};
	std::memcpy(cb_data.transform, mvp.data(), 16 * sizeof(float));
	cb_data.translate[0] = translation.x;
	cb_data.translate[1] = translation.y;
	cb_data._pad[0] = 0.0f;
	cb_data._pad[1] = 0.0f;

	auto cb = AllocateCB(sizeof(TransformCB), &cb_data);
	if (!cb.cpu_ptr) return;

	// --- Set pipeline state ---
	bool has_texture = (tex != nullptr && tex->srv_index >= 0);
	if (clip_mask_enabled_) {
		command_list_->SetPipelineState(has_texture ? pso_texture_stencil_.Get() : pso_color_stencil_.Get());
		command_list_->OMSetStencilRef(stencil_ref_);
	} else {
		command_list_->SetPipelineState(has_texture ? pso_texture_.Get() : pso_color_.Get());
	}

	// --- Set root CBV (parameter 0) ---
	command_list_->SetGraphicsRootConstantBufferView(0, cb.gpu_address);

	// --- Bind texture SRV (parameter 1) ---
	if (has_texture) {
		D3D12_GPU_DESCRIPTOR_HANDLE srv_handle = srv_heap_->GetGPUDescriptorHandleForHeapStart();
		srv_handle.ptr += static_cast<UINT64>(tex->srv_index) * srv_descriptor_size_;
		command_list_->SetGraphicsRootDescriptorTable(1, srv_handle);
	}

	// --- Set scissor ---
	if (scissor_enabled_) {
		command_list_->RSSetScissorRects(1, &scissor_rect_);
	} else {
		D3D12_RECT full_rect = { 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };
		command_list_->RSSetScissorRects(1, &full_rect);
	}

	// --- Draw ---
	command_list_->IASetVertexBuffers(0, 1, &geom->vbv);
	command_list_->IASetIndexBuffer(&geom->ibv);
	command_list_->DrawIndexedInstanced(static_cast<UINT>(geom->num_indices), 1, 0, 0, 0);
}

void RenderInterface_DX12::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) {
	if (!geometry) return;
	auto* geom = reinterpret_cast<GeometryData*>(geometry);
	// Defer deletion — GPU may still be using these buffers
	DeferRelease(std::move(geom->vertex_buffer));
	DeferRelease(std::move(geom->index_buffer));
	delete geom;
}

// ===========================================================================
// Textures
// ===========================================================================

void RenderInterface_DX12::UploadTextureData(ID3D12Resource* dest_texture,
	const Rml::byte* data, int width, int height) {
	ZoneScopedN("DX12::UploadTextureData");

	const UINT64 row_pitch = static_cast<UINT64>(width) * 4;
	// D3D12 requires 256-byte aligned row pitch for texture upload
	const UINT64 aligned_row_pitch = (row_pitch + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
		& ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
	const UINT64 upload_size = aligned_row_pitch * static_cast<UINT64>(height);

	// Create upload buffer
	D3D12_HEAP_PROPERTIES upload_heap{};
	upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

	D3D12_RESOURCE_DESC buf_desc{};
	buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buf_desc.Width = upload_size;
	buf_desc.Height = 1;
	buf_desc.DepthOrArraySize = 1;
	buf_desc.MipLevels = 1;
	buf_desc.Format = DXGI_FORMAT_UNKNOWN;
	buf_desc.SampleDesc.Count = 1;
	buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	ComPtr<ID3D12Resource> upload_buffer;
	HRESULT hr = device_->CreateCommittedResource(
		&upload_heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
		IID_PPV_ARGS(&upload_buffer));
	if (FAILED(hr)) {
		std::fprintf(stderr, "[DX12] Texture upload buffer creation failed: 0x%08lX\n",
			static_cast<unsigned long>(hr));
		return;
	}
	upload_buffer->SetName(L"RmlUi_TexUpload");

	// Map and copy data row by row (handling pitch alignment)
	void* mapped = nullptr;
	D3D12_RANGE read_range{0, 0};
	hr = upload_buffer->Map(0, &read_range, &mapped);
	if (FAILED(hr)) {
		std::fprintf(stderr, "[DX12] Texture upload Map failed: 0x%08lX\n",
			static_cast<unsigned long>(hr));
		return;
	}

	auto* dst = static_cast<uint8_t*>(mapped);
	for (int y = 0; y < height; ++y) {
		std::memcpy(dst + y * aligned_row_pitch, data + y * row_pitch, static_cast<size_t>(row_pitch));
	}
	upload_buffer->Unmap(0, nullptr);

	// Record copy command on the main command list.
	// The texture starts in COPY_DEST state (set during creation).
	D3D12_TEXTURE_COPY_LOCATION src_loc{};
	src_loc.pResource = upload_buffer.Get();
	src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src_loc.PlacedFootprint.Offset = 0;
	src_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	src_loc.PlacedFootprint.Footprint.Width = static_cast<UINT>(width);
	src_loc.PlacedFootprint.Footprint.Height = static_cast<UINT>(height);
	src_loc.PlacedFootprint.Footprint.Depth = 1;
	src_loc.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(aligned_row_pitch);

	D3D12_TEXTURE_COPY_LOCATION dst_loc{};
	dst_loc.pResource = dest_texture;
	dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst_loc.SubresourceIndex = 0;

	command_list_->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);


	// Transition: COPY_DEST -> PIXEL_SHADER_RESOURCE
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = dest_texture;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	command_list_->ResourceBarrier(1, &barrier);

	// Upload buffer must survive until GPU finishes the copy
	DeferRelease(std::move(upload_buffer));
}

Rml::TextureHandle RenderInterface_DX12::GenerateTexture(
	Rml::Span<const Rml::byte> source_data, Rml::Vector2i source_dimensions) {
	ZoneScopedN("DX12::GenerateTexture");

	if (source_data.empty() || source_dimensions.x <= 0 || source_dimensions.y <= 0)
		return Rml::TextureHandle(0);

	int32_t srv_slot = AllocateSrvSlot();
	if (srv_slot < 0) return Rml::TextureHandle(0);

	auto* tex = new TextureData();
	tex->width = source_dimensions.x;
	tex->height = source_dimensions.y;
	tex->srv_index = srv_slot;

	// Create texture resource in DEFAULT heap
	D3D12_HEAP_PROPERTIES default_heap{};
	default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC tex_desc{};
	tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	tex_desc.Width = static_cast<UINT64>(source_dimensions.x);
	tex_desc.Height = static_cast<UINT>(source_dimensions.y);
	tex_desc.DepthOrArraySize = 1;
	tex_desc.MipLevels = 1;
	tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	tex_desc.SampleDesc.Count = 1;
	tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	tex_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

	HRESULT hr = device_->CreateCommittedResource(
		&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
		IID_PPV_ARGS(&tex->texture));
	if (FAILED(hr)) {
		std::fprintf(stderr, "[DX12] Texture creation failed: 0x%08lX\n",
			static_cast<unsigned long>(hr));
		FreeSrvSlot(srv_slot);
		delete tex;
		return Rml::TextureHandle(0);
	}
	tex->texture->SetName(L"RmlUi_Texture");

	// Create SRV
	D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
	srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv_desc.Texture2D.MipLevels = 1;
	srv_desc.Texture2D.MostDetailedMip = 0;

	D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = srv_heap_->GetCPUDescriptorHandleForHeapStart();
	cpu_handle.ptr += static_cast<SIZE_T>(srv_slot) * srv_descriptor_size_;
	device_->CreateShaderResourceView(tex->texture.Get(), &srv_desc, cpu_handle);

	// Upload pixel data
	UploadTextureData(tex->texture.Get(), source_data.data(), source_dimensions.x, source_dimensions.y);

	return reinterpret_cast<Rml::TextureHandle>(tex);
}

Rml::TextureHandle RenderInterface_DX12::LoadTexture(
	Rml::Vector2i& /*texture_dimensions*/, const Rml::String& /*source*/) {
	// Return 0 to let RmlUi fall back to GenerateTexture for font glyphs etc.
	return Rml::TextureHandle(0);
}

void RenderInterface_DX12::ReleaseTexture(Rml::TextureHandle texture_handle) {
	if (!texture_handle) return;
	auto* tex = reinterpret_cast<TextureData*>(texture_handle);
	if (tex->is_layer_texture) {
		// Layer texture — the layer pool owns the resource and SRV. Don't free them here.
		// Just delete the wrapper. The layer pool will clean up when the layer is freed or
		// during ReleaseAllLayers.
		delete tex;
	} else {
		// Regular texture — defer deletion of the resource and SRV slot
		DeferRelease(std::move(tex->texture), tex->srv_index);
		delete tex;
	}
}

void RenderInterface_DX12::UpdateTextureData(
	Rml::TextureHandle texture_handle, Rml::Span<const Rml::byte> source_data,
	Rml::Vector2i source_dimensions) {
	ZoneScopedN("DX12::UpdateTextureData");

	if (!texture_handle) return;
	auto* tex = reinterpret_cast<TextureData*>(texture_handle);

	if (source_dimensions.x != tex->width || source_dimensions.y != tex->height) {
		std::fprintf(stderr, "[DX12] UpdateTextureData: dimension mismatch (%dx%d vs %dx%d)\n",
			source_dimensions.x, source_dimensions.y, tex->width, tex->height);
		return;
	}

	// Transition from PIXEL_SHADER_RESOURCE back to COPY_DEST for re-upload
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = tex->texture.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	command_list_->ResourceBarrier(1, &barrier);

	// UploadTextureData will transition back to PIXEL_SHADER_RESOURCE
	UploadTextureData(tex->texture.Get(), source_data.data(), tex->width, tex->height);
}

// ===========================================================================
// YUV Textures (I420 → 3 × R8 GPU textures)
// ===========================================================================

void RenderInterface_DX12::UploadR8TextureData(ID3D12Resource* dest_texture,
	const uint8_t* data, uint32_t src_stride, int width, int height,
	void* upload_buf_ptr) {
	ZoneScopedN("DX12::UploadR8TextureData");
	auto* upload_buf = static_cast<YUVUploadBuffer*>(upload_buf_ptr);

	const UINT aligned_row_pitch = (static_cast<UINT>(width) + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
		& ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
	const UINT64 upload_size = static_cast<UINT64>(aligned_row_pitch) * height;

	// Allocate and persistently map upload buffer on first use or size change
	if (!upload_buf->resource || upload_buf->size < upload_size) {
		// Unmap old buffer if any
		if (upload_buf->mapped && upload_buf->resource) {
			upload_buf->resource->Unmap(0, nullptr);
			upload_buf->mapped = nullptr;
		}
		if (upload_buf->resource)
			DeferRelease(std::move(upload_buf->resource));

		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_UPLOAD;

		D3D12_RESOURCE_DESC buf_desc{};
		buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		buf_desc.Width = upload_size;
		buf_desc.Height = 1;
		buf_desc.DepthOrArraySize = 1;
		buf_desc.MipLevels = 1;
		buf_desc.Format = DXGI_FORMAT_UNKNOWN;
		buf_desc.SampleDesc.Count = 1;
		buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		HRESULT hr = device_->CreateCommittedResource(
			&heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
			IID_PPV_ARGS(&upload_buf->resource));
		if (FAILED(hr)) return;

		// Persistently map — never unmapped until buffer is destroyed
		D3D12_RANGE read_range{0, 0};
		hr = upload_buf->resource->Map(0, &read_range, &upload_buf->mapped);
		if (FAILED(hr)) { upload_buf->resource.Reset(); return; }

		upload_buf->size = upload_size;
		upload_buf->aligned_row_pitch = aligned_row_pitch;
	}

	// Fast path: just memcpy into the already-mapped buffer
	auto* dst = static_cast<uint8_t*>(upload_buf->mapped);
	if (aligned_row_pitch == src_stride) {
		// Strides match — single bulk copy
		std::memcpy(dst, data, static_cast<size_t>(aligned_row_pitch) * height);
	} else {
		for (int y = 0; y < height; ++y)
			std::memcpy(dst + y * aligned_row_pitch, data + y * src_stride,
				static_cast<size_t>(width));
	}

	D3D12_TEXTURE_COPY_LOCATION src_loc{};
	src_loc.pResource = upload_buf->resource.Get();
	src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src_loc.PlacedFootprint.Offset = 0;
	src_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UNORM;
	src_loc.PlacedFootprint.Footprint.Width = static_cast<UINT>(width);
	src_loc.PlacedFootprint.Footprint.Height = static_cast<UINT>(height);
	src_loc.PlacedFootprint.Footprint.Depth = 1;
	src_loc.PlacedFootprint.Footprint.RowPitch = aligned_row_pitch;

	D3D12_TEXTURE_COPY_LOCATION dst_loc{};
	dst_loc.pResource = dest_texture;
	dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst_loc.SubresourceIndex = 0;

	command_list_->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);


	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = dest_texture;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	command_list_->ResourceBarrier(1, &barrier);
}

static ID3D12Resource* CreateR8Texture(ID3D12Device* device, int width, int height,
	ID3D12DescriptorHeap* srv_heap, uint32_t srv_descriptor_size, int32_t srv_slot) {

	D3D12_HEAP_PROPERTIES default_heap{};
	default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = static_cast<UINT64>(width);
	desc.Height = static_cast<UINT>(height);
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_R8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

	ID3D12Resource* tex = nullptr;
	HRESULT hr = device->CreateCommittedResource(
		&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
		IID_PPV_ARGS(&tex));
	if (FAILED(hr)) return nullptr;

	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = DXGI_FORMAT_R8_UNORM;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;

	D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = srv_heap->GetCPUDescriptorHandleForHeapStart();
	cpu_handle.ptr += static_cast<SIZE_T>(srv_slot) * srv_descriptor_size;
	device->CreateShaderResourceView(tex, &srv, cpu_handle);

	return tex;
}

uintptr_t RenderInterface_DX12::GenerateYUVTexture(
	const uint8_t* y_data, uint32_t y_stride,
	const uint8_t* u_data, const uint8_t* v_data, uint32_t uv_stride,
	uint32_t width, uint32_t height) {
	ZoneScopedN("DX12::GenerateYUVTexture");

	int32_t y_srv = AllocateSrvSlot();
	int32_t u_srv = AllocateSrvSlot();
	int32_t v_srv = AllocateSrvSlot();
	if (y_srv < 0 || u_srv < 0 || v_srv < 0) {
		if (y_srv >= 0) FreeSrvSlot(y_srv);
		if (u_srv >= 0) FreeSrvSlot(u_srv);
		if (v_srv >= 0) FreeSrvSlot(v_srv);
		return 0;
	}

	auto* yuv = new YUVTextureData();
	yuv->width = static_cast<int>(width);
	yuv->height = static_cast<int>(height);
	yuv->y_srv = y_srv;
	yuv->u_srv = u_srv;
	yuv->v_srv = v_srv;

	int half_w = static_cast<int>(width / 2);
	int half_h = static_cast<int>(height / 2);

	yuv->y_texture.Attach(CreateR8Texture(device_.Get(), static_cast<int>(width), static_cast<int>(height),
		srv_heap_.Get(), srv_descriptor_size_, y_srv));
	yuv->u_texture.Attach(CreateR8Texture(device_.Get(), half_w, half_h,
		srv_heap_.Get(), srv_descriptor_size_, u_srv));
	yuv->v_texture.Attach(CreateR8Texture(device_.Get(), half_w, half_h,
		srv_heap_.Get(), srv_descriptor_size_, v_srv));

	if (!yuv->y_texture || !yuv->u_texture || !yuv->v_texture) {
		delete yuv;
		return 0;
	}

	yuv->y_texture->SetName(L"YUV_Y");
	yuv->u_texture->SetName(L"YUV_U");
	yuv->v_texture->SetName(L"YUV_V");

	int ub = current_back_buffer_index_;
	UploadR8TextureData(yuv->y_texture.Get(), y_data, y_stride,
		static_cast<int>(width), static_cast<int>(height), &yuv->y_upload[ub]);
	UploadR8TextureData(yuv->u_texture.Get(), u_data, uv_stride, half_w, half_h, &yuv->u_upload[ub]);
	UploadR8TextureData(yuv->v_texture.Get(), v_data, uv_stride, half_w, half_h, &yuv->v_upload[ub]);

	return reinterpret_cast<uintptr_t>(yuv);
}

void RenderInterface_DX12::UpdateYUVTexture(uintptr_t handle,
	const uint8_t* y_data, uint32_t y_stride,
	const uint8_t* u_data, const uint8_t* v_data, uint32_t uv_stride,
	uint32_t width, uint32_t height) {
	ZoneScopedN("DX12::UpdateYUVTexture");

	if (!handle) return;
	auto* yuv = reinterpret_cast<YUVTextureData*>(handle);

	int half_w = static_cast<int>(width / 2);
	int half_h = static_cast<int>(height / 2);

	// Transition all 3 textures: PIXEL_SHADER_RESOURCE → COPY_DEST
	D3D12_RESOURCE_BARRIER barriers[3] = {};
	ID3D12Resource* textures[3] = { yuv->y_texture.Get(), yuv->u_texture.Get(), yuv->v_texture.Get() };
	for (int i = 0; i < 3; i++) {
		barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[i].Transition.pResource = textures[i];
		barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	}
	command_list_->ResourceBarrier(3, barriers);

	// Upload all 3 planes using persistent upload buffers (no per-frame allocation)
	int ub = current_back_buffer_index_;
	UploadR8TextureData(yuv->y_texture.Get(), y_data, y_stride,
		static_cast<int>(width), static_cast<int>(height), &yuv->y_upload[ub]);
	UploadR8TextureData(yuv->u_texture.Get(), u_data, uv_stride, half_w, half_h, &yuv->u_upload[ub]);
	UploadR8TextureData(yuv->v_texture.Get(), v_data, uv_stride, half_w, half_h, &yuv->v_upload[ub]);
}

void RenderInterface_DX12::ReleaseYUVTexture(uintptr_t handle) {
	ZoneScopedN("DX12::ReleaseYUVTexture");
	if (!handle) return;
	auto* yuv = reinterpret_cast<YUVTextureData*>(handle);
	DeferRelease(std::move(yuv->y_texture), yuv->y_srv);
	DeferRelease(std::move(yuv->u_texture), yuv->u_srv);
	DeferRelease(std::move(yuv->v_texture), yuv->v_srv);
	for (int i = 0; i < YUVTextureData::NUM_UPLOAD_SETS; i++) {
		if (yuv->y_upload[i].mapped) { yuv->y_upload[i].resource->Unmap(0, nullptr); yuv->y_upload[i].mapped = nullptr; }
		if (yuv->u_upload[i].mapped) { yuv->u_upload[i].resource->Unmap(0, nullptr); yuv->u_upload[i].mapped = nullptr; }
		if (yuv->v_upload[i].mapped) { yuv->v_upload[i].resource->Unmap(0, nullptr); yuv->v_upload[i].mapped = nullptr; }
		DeferRelease(std::move(yuv->y_upload[i].resource));
		DeferRelease(std::move(yuv->u_upload[i].resource));
		DeferRelease(std::move(yuv->v_upload[i].resource));
	}
	delete yuv;
}

void RenderInterface_DX12::RenderYUVGeometry(
	Rml::CompiledGeometryHandle geometry,
	Rml::Vector2f translation, uintptr_t yuv_handle) {
	ZoneScopedN("DX12::RenderYUVGeometry");

	if (!geometry || !yuv_handle) return;
	auto* geom = reinterpret_cast<GeometryData*>(geometry);
	auto* yuv = reinterpret_cast<YUVTextureData*>(yuv_handle);

	// Build transform CB (same as RenderGeometry)
	Rml::Matrix4f mvp = transform_active_ ? projection_ * transform_ : projection_;
	TransformCB cb_data{};
	std::memcpy(cb_data.transform, mvp.data(), 16 * sizeof(float));
	cb_data.translate[0] = translation.x;
	cb_data.translate[1] = translation.y;
	auto cb = AllocateCB(sizeof(TransformCB), &cb_data);
	if (!cb.cpu_ptr) return;

	// Switch to YUV root signature and PSO
	command_list_->SetGraphicsRootSignature(yuv_root_signature_.Get());
	if (clip_mask_enabled_) {
		command_list_->SetPipelineState(pso_yuv_stencil_.Get());
		command_list_->OMSetStencilRef(stencil_ref_);
	} else {
		command_list_->SetPipelineState(pso_yuv_.Get());
	}

	// Re-bind the SRV heap (required after root signature change)
	ID3D12DescriptorHeap* heaps[] = { srv_heap_.Get() };
	command_list_->SetDescriptorHeaps(1, heaps);

	// Param 0: transform CBV
	command_list_->SetGraphicsRootConstantBufferView(0, cb.gpu_address);

	// Params 1-3: Y, U, V texture SRVs
	D3D12_GPU_DESCRIPTOR_HANDLE base = srv_heap_->GetGPUDescriptorHandleForHeapStart();
	int32_t srv_indices[3] = { yuv->y_srv, yuv->u_srv, yuv->v_srv };
	for (int i = 0; i < 3; i++) {
		D3D12_GPU_DESCRIPTOR_HANDLE h = base;
		h.ptr += static_cast<UINT64>(srv_indices[i]) * srv_descriptor_size_;
		command_list_->SetGraphicsRootDescriptorTable(1 + i, h);
	}

	// Scissor
	if (scissor_enabled_) {
		command_list_->RSSetScissorRects(1, &scissor_rect_);
	} else {
		D3D12_RECT full_rect = { 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };
		command_list_->RSSetScissorRects(1, &full_rect);
	}

	// Draw
	command_list_->IASetVertexBuffers(0, 1, &geom->vbv);
	command_list_->IASetIndexBuffer(&geom->ibv);
	command_list_->DrawIndexedInstanced(static_cast<UINT>(geom->num_indices), 1, 0, 0, 0);

	// Restore main root signature for subsequent RmlUi draws
	command_list_->SetGraphicsRootSignature(root_signature_.Get());
	command_list_->SetDescriptorHeaps(1, heaps);
}

// ===========================================================================
// NV12 Texture (R8 Y + R8G8 UV — native hardware decoder format)
// ===========================================================================

static ID3D12Resource* CreateR8G8Texture(ID3D12Device* device, int width, int height,
	ID3D12DescriptorHeap* srv_heap, uint32_t srv_descriptor_size, int32_t srv_slot) {

	D3D12_HEAP_PROPERTIES default_heap{};
	default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = static_cast<UINT64>(width);
	desc.Height = static_cast<UINT>(height);
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_R8G8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

	ID3D12Resource* tex = nullptr;
	HRESULT hr = device->CreateCommittedResource(
		&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
		IID_PPV_ARGS(&tex));
	if (FAILED(hr)) return nullptr;

	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = DXGI_FORMAT_R8G8_UNORM;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;

	D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = srv_heap->GetCPUDescriptorHandleForHeapStart();
	cpu_handle.ptr += static_cast<SIZE_T>(srv_slot) * srv_descriptor_size;
	device->CreateShaderResourceView(tex, &srv, cpu_handle);

	return tex;
}

void RenderInterface_DX12::UploadR8G8TextureData(ID3D12Resource* dest_texture,
	const uint8_t* data, uint32_t src_stride, int width, int height,
	void* upload_buf_ptr) {
	ZoneScopedN("DX12::UploadR8G8TextureData");
	auto* upload_buf = static_cast<YUVUploadBuffer*>(upload_buf_ptr);

	const UINT width_bytes = static_cast<UINT>(width) * 2;  // R8G8 = 2 bytes per pixel
	const UINT aligned_row_pitch = (width_bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
		& ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
	const UINT64 upload_size = static_cast<UINT64>(aligned_row_pitch) * height;

	if (!upload_buf->resource || upload_buf->size < upload_size) {
		if (upload_buf->mapped && upload_buf->resource) {
			upload_buf->resource->Unmap(0, nullptr);
			upload_buf->mapped = nullptr;
		}
		if (upload_buf->resource)
			DeferRelease(std::move(upload_buf->resource));

		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_UPLOAD;

		D3D12_RESOURCE_DESC buf_desc{};
		buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		buf_desc.Width = upload_size;
		buf_desc.Height = 1;
		buf_desc.DepthOrArraySize = 1;
		buf_desc.MipLevels = 1;
		buf_desc.Format = DXGI_FORMAT_UNKNOWN;
		buf_desc.SampleDesc.Count = 1;
		buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		HRESULT hr = device_->CreateCommittedResource(
			&heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
			IID_PPV_ARGS(&upload_buf->resource));
		if (FAILED(hr)) return;

		D3D12_RANGE read_range{0, 0};
		hr = upload_buf->resource->Map(0, &read_range, &upload_buf->mapped);
		if (FAILED(hr)) { upload_buf->resource.Reset(); return; }

		upload_buf->size = upload_size;
		upload_buf->aligned_row_pitch = aligned_row_pitch;
	}

	auto* dst = static_cast<uint8_t*>(upload_buf->mapped);
	if (aligned_row_pitch == src_stride) {
		std::memcpy(dst, data, static_cast<size_t>(aligned_row_pitch) * height);
	} else {
		for (int y = 0; y < height; ++y)
			std::memcpy(dst + y * aligned_row_pitch, data + y * src_stride, width_bytes);
	}

	D3D12_TEXTURE_COPY_LOCATION src_loc{};
	src_loc.pResource = upload_buf->resource.Get();
	src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src_loc.PlacedFootprint.Offset = 0;
	src_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8_UNORM;
	src_loc.PlacedFootprint.Footprint.Width = static_cast<UINT>(width);
	src_loc.PlacedFootprint.Footprint.Height = static_cast<UINT>(height);
	src_loc.PlacedFootprint.Footprint.Depth = 1;
	src_loc.PlacedFootprint.Footprint.RowPitch = aligned_row_pitch;

	D3D12_TEXTURE_COPY_LOCATION dst_loc{};
	dst_loc.pResource = dest_texture;
	dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst_loc.SubresourceIndex = 0;

	command_list_->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);


	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = dest_texture;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	command_list_->ResourceBarrier(1, &barrier);
}

uintptr_t RenderInterface_DX12::GenerateNV12Texture(
	const uint8_t* y_data, uint32_t y_stride,
	const uint8_t* uv_data, uint32_t uv_stride,
	uint32_t width, uint32_t height) {
	ZoneScopedN("DX12::GenerateNV12Texture");

	int32_t y_srv = AllocateSrvSlot();
	int32_t uv_srv = AllocateSrvSlot();
	if (y_srv < 0 || uv_srv < 0) {
		if (y_srv >= 0) FreeSrvSlot(y_srv);
		if (uv_srv >= 0) FreeSrvSlot(uv_srv);
		return 0;
	}

	auto* nv12 = new NV12TextureData();
	nv12->width = static_cast<int>(width);
	nv12->height = static_cast<int>(height);
	nv12->y_srv = y_srv;
	nv12->uv_srv = uv_srv;

	int half_w = static_cast<int>(width / 2);
	int half_h = static_cast<int>(height / 2);

	nv12->y_texture.Attach(CreateR8Texture(device_.Get(), static_cast<int>(width), static_cast<int>(height),
		srv_heap_.Get(), srv_descriptor_size_, y_srv));
	nv12->uv_texture.Attach(CreateR8G8Texture(device_.Get(), half_w, half_h,
		srv_heap_.Get(), srv_descriptor_size_, uv_srv));

	if (!nv12->y_texture || !nv12->uv_texture) {
		delete nv12;
		return 0;
	}

	nv12->y_texture->SetName(L"NV12_Y");
	nv12->uv_texture->SetName(L"NV12_UV");

	int ub = current_back_buffer_index_;
	UploadR8TextureData(nv12->y_texture.Get(), y_data, y_stride,
		static_cast<int>(width), static_cast<int>(height), &nv12->y_upload[ub]);
	UploadR8G8TextureData(nv12->uv_texture.Get(), uv_data, uv_stride, half_w, half_h, &nv12->uv_upload[ub]);

	return reinterpret_cast<uintptr_t>(nv12);
}

void RenderInterface_DX12::UpdateNV12Texture(uintptr_t handle,
	const uint8_t* y_data, uint32_t y_stride,
	const uint8_t* uv_data, uint32_t uv_stride,
	uint32_t width, uint32_t height) {
	ZoneScopedN("DX12::UpdateNV12Texture");

	if (!handle) return;
	auto* nv12 = reinterpret_cast<NV12TextureData*>(handle);

	int half_w = static_cast<int>(width / 2);
	int half_h = static_cast<int>(height / 2);

	// Transition textures: PIXEL_SHADER_RESOURCE → COPY_DEST
	D3D12_RESOURCE_BARRIER barriers[2] = {};
	ID3D12Resource* textures[2] = { nv12->y_texture.Get(), nv12->uv_texture.Get() };
	for (int i = 0; i < 2; i++) {
		barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[i].Transition.pResource = textures[i];
		barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	}
	command_list_->ResourceBarrier(2, barriers);

	int ub = current_back_buffer_index_;
	UploadR8TextureData(nv12->y_texture.Get(), y_data, y_stride,
		static_cast<int>(width), static_cast<int>(height), &nv12->y_upload[ub]);
	UploadR8G8TextureData(nv12->uv_texture.Get(), uv_data, uv_stride, half_w, half_h, &nv12->uv_upload[ub]);
}

void RenderInterface_DX12::ReleaseNV12Texture(uintptr_t handle) {
	ZoneScopedN("DX12::ReleaseNV12Texture");
	if (!handle) return;
	auto* nv12 = reinterpret_cast<NV12TextureData*>(handle);
	DeferRelease(std::move(nv12->y_texture), nv12->y_srv);
	DeferRelease(std::move(nv12->uv_texture), nv12->uv_srv);
	for (int i = 0; i < NV12TextureData::NUM_UPLOAD_SETS; i++) {
		if (nv12->y_upload[i].mapped) { nv12->y_upload[i].resource->Unmap(0, nullptr); nv12->y_upload[i].mapped = nullptr; }
		if (nv12->uv_upload[i].mapped) { nv12->uv_upload[i].resource->Unmap(0, nullptr); nv12->uv_upload[i].mapped = nullptr; }
		DeferRelease(std::move(nv12->y_upload[i].resource));
		DeferRelease(std::move(nv12->uv_upload[i].resource));
	}
	delete nv12;
}

void RenderInterface_DX12::RenderNV12Geometry(
	Rml::CompiledGeometryHandle geometry,
	Rml::Vector2f translation, uintptr_t nv12_handle) {
	ZoneScopedN("DX12::RenderNV12Geometry");

	if (!geometry || !nv12_handle) return;
	auto* geom = reinterpret_cast<GeometryData*>(geometry);
	auto* nv12 = reinterpret_cast<NV12TextureData*>(nv12_handle);

	Rml::Matrix4f mvp = transform_active_ ? projection_ * transform_ : projection_;
	TransformCB cb_data{};
	std::memcpy(cb_data.transform, mvp.data(), 16 * sizeof(float));
	cb_data.translate[0] = translation.x;
	cb_data.translate[1] = translation.y;
	auto cb = AllocateCB(sizeof(TransformCB), &cb_data);
	if (!cb.cpu_ptr) return;

	command_list_->SetGraphicsRootSignature(nv12_root_signature_.Get());
	if (clip_mask_enabled_) {
		command_list_->SetPipelineState(pso_nv12_stencil_.Get());
		command_list_->OMSetStencilRef(stencil_ref_);
	} else {
		command_list_->SetPipelineState(pso_nv12_.Get());
	}

	ID3D12DescriptorHeap* heaps[] = { srv_heap_.Get() };
	command_list_->SetDescriptorHeaps(1, heaps);

	command_list_->SetGraphicsRootConstantBufferView(0, cb.gpu_address);

	D3D12_GPU_DESCRIPTOR_HANDLE base = srv_heap_->GetGPUDescriptorHandleForHeapStart();
	int32_t srv_indices[2] = { nv12->y_srv, nv12->uv_srv };
	for (int i = 0; i < 2; i++) {
		D3D12_GPU_DESCRIPTOR_HANDLE h = base;
		h.ptr += static_cast<UINT64>(srv_indices[i]) * srv_descriptor_size_;
		command_list_->SetGraphicsRootDescriptorTable(1 + i, h);
	}

	if (scissor_enabled_) {
		command_list_->RSSetScissorRects(1, &scissor_rect_);
	} else {
		D3D12_RECT full_rect = { 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };
		command_list_->RSSetScissorRects(1, &full_rect);
	}

	command_list_->IASetVertexBuffers(0, 1, &geom->vbv);
	command_list_->IASetIndexBuffer(&geom->ibv);
	command_list_->DrawIndexedInstanced(static_cast<UINT>(geom->num_indices), 1, 0, 0, 0);

	command_list_->SetGraphicsRootSignature(root_signature_.Get());
	command_list_->SetDescriptorHeaps(1, heaps);
}

// ===========================================================================
// Scissor
// ===========================================================================

void RenderInterface_DX12::EnableScissorRegion(bool enable) {
	scissor_enabled_ = enable;
}

void RenderInterface_DX12::SetScissorRegion(Rml::Rectanglei region) {
	scissor_rect_.left = static_cast<LONG>(region.Left());
	scissor_rect_.top = static_cast<LONG>(region.Top());
	scissor_rect_.right = static_cast<LONG>(region.Right());
	scissor_rect_.bottom = static_cast<LONG>(region.Bottom());
}

// ===========================================================================
// Transform
// ===========================================================================

void RenderInterface_DX12::SetTransform(const Rml::Matrix4f* transform) {
	if (transform) {
		transform_ = *transform;
		transform_active_ = true;
	} else {
		transform_active_ = false;
	}
}

// ===========================================================================
// Clip Mask (stencil-based)
// ===========================================================================

void RenderInterface_DX12::EnableClipMask(bool enable) {
	clip_mask_enabled_ = enable;
	if (!enable) {
		stencil_ref_ = 0;
	}
}

void RenderInterface_DX12::RenderToClipMask(
	Rml::ClipMaskOperation mask_operation, Rml::CompiledGeometryHandle geometry,
	Rml::Vector2f translation) {
	ZoneScopedN("DX12::RenderToClipMask");

	if (!geometry) return;

	D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = dsv_heap_->GetCPUDescriptorHandleForHeapStart();

	switch (mask_operation) {
	case Rml::ClipMaskOperation::Set:
		// Clear stencil to 0, then write 1 where mask geometry renders.
		command_list_->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
		stencil_ref_ = 1;
		command_list_->SetPipelineState(pso_stencil_set_.Get());
		command_list_->OMSetStencilRef(1);  // REPLACE writes this value
		break;

	case Rml::ClipMaskOperation::SetInverse:
		// Clear stencil to 1, then write 0 where mask geometry renders.
		// After this, stencil == 1 everywhere EXCEPT the mask geometry.
		command_list_->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_STENCIL, 1.0f, 1, 0, nullptr);
		stencil_ref_ = 1;
		command_list_->SetPipelineState(pso_stencil_set_.Get());
		command_list_->OMSetStencilRef(0);  // REPLACE writes 0 where geometry is
		break;

	case Rml::ClipMaskOperation::Intersect:
		// Increment stencil where geometry renders (only where current stencil == stencil_ref_).
		// Then bump stencil_ref_ so subsequent EQUAL tests pass only in the intersection.
		command_list_->SetPipelineState(pso_stencil_intersect_.Get());
		command_list_->OMSetStencilRef(stencil_ref_);  // EQUAL test against current ref
		break;
	}

	// Render the mask geometry (same flow as RenderGeometry but with the stencil PSO already set).
	auto* geom = reinterpret_cast<GeometryData*>(geometry);

	// Build transform constant buffer
	Rml::Matrix4f mvp;
	if (transform_active_) {
		mvp = projection_ * transform_;
	} else {
		mvp = projection_;
	}

	TransformCB cb_data{};
	std::memcpy(cb_data.transform, mvp.data(), 16 * sizeof(float));
	cb_data.translate[0] = translation.x;
	cb_data.translate[1] = translation.y;
	cb_data._pad[0] = 0.0f;
	cb_data._pad[1] = 0.0f;

	auto cb = AllocateCB(sizeof(TransformCB), &cb_data);
	if (!cb.cpu_ptr) return;

	// Bind root CBV
	command_list_->SetGraphicsRootConstantBufferView(0, cb.gpu_address);

	// Set scissor
	if (scissor_enabled_) {
		command_list_->RSSetScissorRects(1, &scissor_rect_);
	} else {
		D3D12_RECT full_rect = { 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };
		command_list_->RSSetScissorRects(1, &full_rect);
	}

	// Draw
	command_list_->IASetVertexBuffers(0, 1, &geom->vbv);
	command_list_->IASetIndexBuffer(&geom->ibv);
	command_list_->DrawIndexedInstanced(static_cast<UINT>(geom->num_indices), 1, 0, 0, 0);

	// After rendering mask geometry, update stencil ref for Intersect
	if (mask_operation == Rml::ClipMaskOperation::Intersect) {
		stencil_ref_++;
	}
}

// ===========================================================================
// Layers (Phase 5)
// ===========================================================================

Rml::LayerHandle RenderInterface_DX12::PushLayer() {
	ZoneScopedN("DX12::PushLayer");
	int layer_idx = AllocateLayer();
	if (layer_idx < 0) return Rml::LayerHandle(0);

	// Layer textures are always in RENDER_TARGET state when allocated:
	// - New layers are created in RT state.
	// - Reused layers were transitioned back to RT in CompositeLayers after compositing.

	// Push onto the layer stack. Handle value = layer_idx + 1 (0 is reserved for base layer).
	layer_stack_.push_back(layer_idx);

	// Clear to transparent black
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetLayerRTV(rtv_heap_.Get(), rtv_descriptor_size_, layer_idx);
	const float transparent_black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	command_list_->ClearRenderTargetView(rtv, transparent_black, 0, nullptr);

	// Clear stencil for fresh clip state on the new layer
	D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
	command_list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

	// Bind this layer as render target
	command_list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

	// Reset clip mask state for this layer
	clip_mask_enabled_ = false;
	stencil_ref_ = 0;

	// LayerHandle: layer_idx + 1 (0 is reserved for the initial base layer / back buffer)
	return static_cast<Rml::LayerHandle>(layer_idx + 1);
}

void RenderInterface_DX12::PopLayer() {
	ZoneScopedN("DX12::PopLayer");
	if (layer_stack_.empty()) return;

	int popped_idx = layer_stack_.back();
	layer_stack_.pop_back();

	// Transition popped layer's color texture: RENDER_TARGET -> PIXEL_SHADER_RESOURCE
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = layer_pool_[popped_idx].color_texture.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	command_list_->ResourceBarrier(1, &barrier);

	// Restore previous render target
	if (layer_stack_.empty()) {
		// Back to back buffer
		SetRenderTargetToBackBuffer();
	} else {
		SetRenderTargetToLayer(layer_stack_.back());
	}

	// Reset clip mask — parent's clip state will be restored by RmlUi calling EnableClipMask/RenderToClipMask
	clip_mask_enabled_ = false;
	stencil_ref_ = 0;
}

void RenderInterface_DX12::CompositeLayers(
	Rml::LayerHandle source, Rml::LayerHandle destination,
	Rml::BlendMode blend_mode, Rml::Span<const Rml::CompiledFilterHandle> filters) {
	ZoneScopedN("DX12::CompositeLayers");

	// source and destination are LayerHandle values: 0 = back buffer, >0 = layer_idx + 1
	int src_idx = static_cast<int>(source) - 1;   // -1 means back buffer
	int dst_idx = static_cast<int>(destination) - 1;

	// Validate source layer
	if (src_idx < 0 || src_idx >= static_cast<int>(layer_pool_.size())) {
		std::fprintf(stderr, "[DX12] CompositeLayers: invalid source handle %zu\n",
			static_cast<size_t>(source));
		return;
	}

	auto& src_layer = layer_pool_[src_idx];

	// Source should already be in PIXEL_SHADER_RESOURCE state (set by PopLayer).
	// Apply filters if any. After RenderFilters, the filtered result is in postprocess_targets_[0]
	// in PSR state, and we composite from that instead of the source layer.
	bool has_filters = !filters.empty();
	if (has_filters) {
		RenderFilters(filters, src_idx);
	}

	// Determine the SRV to sample: filtered result (pp0) or source layer
	int32_t composite_srv_index;
	if (has_filters) {
		composite_srv_index = postprocess_targets_[0]->srv_index;
	} else {
		composite_srv_index = src_layer.srv_index;
	}

	// Bind destination as render target (without DSV — passthrough PSOs have DSVFormat=UNKNOWN)
	D3D12_CPU_DESCRIPTOR_HANDLE dst_rtv;
	if (dst_idx < 0) {
		// Destination is back buffer
		dst_rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
		dst_rtv.ptr += static_cast<SIZE_T>(current_back_buffer_index_) * rtv_descriptor_size_;
	} else {
		// Destination is a layer — it must be in RENDER_TARGET state
		dst_rtv = GetLayerRTV(rtv_heap_.Get(), rtv_descriptor_size_, dst_idx);
	}

	// Bind destination RTV without DSV (passthrough PSOs have DSVFormat=UNKNOWN)
	command_list_->OMSetRenderTargets(1, &dst_rtv, FALSE, nullptr);

	// Set fullscreen viewport and scissor
	D3D12_VIEWPORT viewport{};
	viewport.Width = static_cast<float>(width_);
	viewport.Height = static_cast<float>(height_);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	command_list_->RSSetViewports(1, &viewport);

	D3D12_RECT full_rect = { 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };
	command_list_->RSSetScissorRects(1, &full_rect);

	// Select PSO based on blend mode
	if (blend_mode == Rml::BlendMode::Replace) {
		command_list_->SetPipelineState(pso_passthrough_replace_.Get());
	} else {
		command_list_->SetPipelineState(pso_passthrough_blend_.Get());
	}

	// Bind source texture SRV (filtered or original)
	D3D12_GPU_DESCRIPTOR_HANDLE srv_handle = srv_heap_->GetGPUDescriptorHandleForHeapStart();
	srv_handle.ptr += static_cast<UINT64>(composite_srv_index) * srv_descriptor_size_;
	command_list_->SetGraphicsRootDescriptorTable(1, srv_handle);

	// Draw fullscreen quad
	command_list_->IASetVertexBuffers(0, 1, &fullscreen_quad_->vbv);
	command_list_->IASetIndexBuffer(&fullscreen_quad_->ibv);
	command_list_->DrawIndexedInstanced(6, 1, 0, 0, 0);

	// Restore DSV binding on destination for subsequent rendering (stencil clip masks)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
		command_list_->OMSetRenderTargets(1, &dst_rtv, FALSE, &dsv);
	}

	// Transition filtered postprocess target back to PSR (it already is — no-op needed).

	// Transition source layer back to RENDER_TARGET for potential reuse
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = src_layer.color_texture.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	command_list_->ResourceBarrier(1, &barrier);

	// Free source layer back to pool (it's been composited)
	FreeLayer(src_idx);
}

Rml::TextureHandle RenderInterface_DX12::SaveLayerAsTexture() {
	ZoneScopedN("DX12::SaveLayerAsTexture");
	if (layer_stack_.empty()) return Rml::TextureHandle(0);

	int layer_idx = layer_stack_.back();
	auto& layer = layer_pool_[layer_idx];

	// Create a TextureData that references the layer's color texture and SRV.
	// The layer stays allocated (in_use) until ReleaseTexture is called.
	auto* tex = new TextureData();
	tex->texture = layer.color_texture; // AddRef via ComPtr copy
	tex->srv_index = layer.srv_index;
	tex->width = layer.width;
	tex->height = layer.height;
	tex->is_layer_texture = true;

	// The layer texture is currently in RENDER_TARGET state (it's the active target).
	// Transition to PIXEL_SHADER_RESOURCE for sampling.
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = layer.color_texture.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	command_list_->ResourceBarrier(1, &barrier);

	return reinterpret_cast<Rml::TextureHandle>(tex);
}

Rml::CompiledFilterHandle RenderInterface_DX12::SaveLayerAsMaskImage() {
	ZoneScopedN("DX12::SaveLayerAsMaskImage");
	// Save the current layer as a mask image. For now (Phase 5), just save the SRV reference.
	// Actual mask application will be implemented in Phase 6 (filters).
	if (layer_stack_.empty()) return Rml::CompiledFilterHandle(0);

	int layer_idx = layer_stack_.back();
	auto& layer = layer_pool_[layer_idx];

	// Create a TextureData to hold the mask image reference
	auto* tex = new TextureData();
	tex->texture = layer.color_texture;
	tex->srv_index = layer.srv_index;
	tex->width = layer.width;
	tex->height = layer.height;
	tex->is_layer_texture = true;

	// Transition to PIXEL_SHADER_RESOURCE
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = layer.color_texture.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	command_list_->ResourceBarrier(1, &barrier);

	return reinterpret_cast<Rml::CompiledFilterHandle>(tex);
}

// ===========================================================================
// Filters (Phase 6)
// ===========================================================================

Rml::CompiledFilterHandle RenderInterface_DX12::CompileFilter(
	const Rml::String& name, const Rml::Dictionary& parameters) {

	auto* filter = new CompiledFilterData();

	if (name == "opacity") {
		filter->type = FilterType::Passthrough;
		filter->blend_factor = Rml::Get(parameters, "value", 1.0f);
	}
	else if (name == "blur") {
		filter->type = FilterType::Blur;
		filter->sigma = Rml::Get(parameters, "sigma", 1.0f);
	}
	else if (name == "drop-shadow") {
		filter->type = FilterType::DropShadow;
		filter->sigma = Rml::Get(parameters, "sigma", 0.f);
		filter->color = Rml::Get(parameters, "color", Rml::Colourb()).ToPremultiplied();
		filter->offset = Rml::Get(parameters, "offset", Rml::Vector2f(0.f));
	}
	else if (name == "brightness") {
		filter->type = FilterType::ColorMatrix;
		const float value = Rml::Get(parameters, "value", 1.0f);
		filter->color_matrix = Rml::Matrix4f::Diag(value, value, value, 1.f);
	}
	else if (name == "contrast") {
		filter->type = FilterType::ColorMatrix;
		const float value = Rml::Get(parameters, "value", 1.0f);
		const float grayness = 0.5f - 0.5f * value;
		filter->color_matrix = Rml::Matrix4f::Diag(value, value, value, 1.f);
		filter->color_matrix.SetColumn(3, Rml::Vector4f(grayness, grayness, grayness, 1.f));
	}
	else if (name == "invert") {
		filter->type = FilterType::ColorMatrix;
		const float value = Rml::Math::Clamp(Rml::Get(parameters, "value", 1.0f), 0.f, 1.f);
		const float inverted = 1.f - 2.f * value;
		filter->color_matrix = Rml::Matrix4f::Diag(inverted, inverted, inverted, 1.f);
		filter->color_matrix.SetColumn(3, Rml::Vector4f(value, value, value, 1.f));
	}
	else if (name == "grayscale") {
		filter->type = FilterType::ColorMatrix;
		const float value = Rml::Get(parameters, "value", 1.0f);
		const float rev_value = 1.f - value;
		const Rml::Vector3f gray = value * Rml::Vector3f(0.2126f, 0.7152f, 0.0722f);
		// clang-format off
		filter->color_matrix = Rml::Matrix4f::FromRows(
			{gray.x + rev_value, gray.y,             gray.z,             0.f},
			{gray.x,             gray.y + rev_value, gray.z,             0.f},
			{gray.x,             gray.y,             gray.z + rev_value, 0.f},
			{0.f,                0.f,                0.f,                1.f}
		);
		// clang-format on
	}
	else if (name == "sepia") {
		filter->type = FilterType::ColorMatrix;
		const float value = Rml::Get(parameters, "value", 1.0f);
		const float rev_value = 1.f - value;
		const Rml::Vector3f r_mix = value * Rml::Vector3f(0.393f, 0.769f, 0.189f);
		const Rml::Vector3f g_mix = value * Rml::Vector3f(0.349f, 0.686f, 0.168f);
		const Rml::Vector3f b_mix = value * Rml::Vector3f(0.272f, 0.534f, 0.131f);
		// clang-format off
		filter->color_matrix = Rml::Matrix4f::FromRows(
			{r_mix.x + rev_value, r_mix.y,             r_mix.z,             0.f},
			{g_mix.x,             g_mix.y + rev_value, g_mix.z,             0.f},
			{b_mix.x,             b_mix.y,             b_mix.z + rev_value, 0.f},
			{0.f,                 0.f,                 0.f,                 1.f}
		);
		// clang-format on
	}
	else if (name == "hue-rotate") {
		filter->type = FilterType::ColorMatrix;
		const float value = Rml::Get(parameters, "value", 1.0f);
		const float s = Rml::Math::Sin(value);
		const float c = Rml::Math::Cos(value);
		// clang-format off
		filter->color_matrix = Rml::Matrix4f::FromRows(
			{0.213f + 0.787f * c - 0.213f * s,  0.715f - 0.715f * c - 0.715f * s,  0.072f - 0.072f * c + 0.928f * s,  0.f},
			{0.213f - 0.213f * c + 0.143f * s,  0.715f + 0.285f * c + 0.140f * s,  0.072f - 0.072f * c - 0.283f * s,  0.f},
			{0.213f - 0.213f * c - 0.787f * s,  0.715f - 0.715f * c + 0.715f * s,  0.072f + 0.928f * c + 0.072f * s,  0.f},
			{0.f,                               0.f,                               0.f,                               1.f}
		);
		// clang-format on
	}
	else if (name == "saturate") {
		filter->type = FilterType::ColorMatrix;
		const float value = Rml::Get(parameters, "value", 1.0f);
		// clang-format off
		filter->color_matrix = Rml::Matrix4f::FromRows(
			{0.213f + 0.787f * value,  0.715f - 0.715f * value,  0.072f - 0.072f * value,  0.f},
			{0.213f - 0.213f * value,  0.715f + 0.285f * value,  0.072f - 0.072f * value,  0.f},
			{0.213f - 0.213f * value,  0.715f - 0.715f * value,  0.072f + 0.928f * value,  0.f},
			{0.f,                      0.f,                      0.f,                      1.f}
		);
		// clang-format on
	}
	else {
		Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported filter type '%s'.", name.c_str());
		delete filter;
		return Rml::CompiledFilterHandle(0);
	}

	return reinterpret_cast<Rml::CompiledFilterHandle>(filter);
}

void RenderInterface_DX12::ReleaseFilter(Rml::CompiledFilterHandle filter) {
	if (!filter) return;
	// Check if this is a CompiledFilterData (from CompileFilter) or a TextureData (from SaveLayerAsMaskImage).
	// SaveLayerAsMaskImage stores TextureData* with is_layer_texture=true.
	// CompiledFilterData with type==MaskImage stores a TextureData* inside it.
	// We distinguish by checking if the pointer looks like a CompiledFilterData with a valid type.
	// Since SaveLayerAsMaskImage returns a TextureData* directly, and CompiledFilterData has type as first field,
	// we use the mask_texture field: if type is MaskImage, the mask_texture's TextureData is cleaned up.
	//
	// Actually, SaveLayerAsMaskImage returns a TextureData* cast to CompiledFilterHandle.
	// We can tell them apart: TextureData's first field is a ComPtr (pointer-sized), while
	// CompiledFilterData's first field is FilterType enum (int-sized). We use a simple approach:
	// Check if the address points to something with a valid FilterType in [1..5].
	auto* as_filter = reinterpret_cast<CompiledFilterData*>(filter);
	if (as_filter->type >= FilterType::Passthrough && as_filter->type <= FilterType::MaskImage) {
		// This is a CompiledFilterData from CompileFilter
		if (as_filter->type == FilterType::MaskImage && as_filter->mask_texture) {
			// The mask texture wrapper — layer owns the actual resource
			delete as_filter->mask_texture;
		}
		delete as_filter;
	} else {
		// This is a TextureData* from SaveLayerAsMaskImage
		auto* tex = reinterpret_cast<TextureData*>(filter);
		delete tex;  // Layer owns the resource, just delete the wrapper
	}
}

// ===========================================================================
// Shaders (Phase 6) — gradients
// ===========================================================================

Rml::CompiledShaderHandle RenderInterface_DX12::CompileShader(
	const Rml::String& name, const Rml::Dictionary& parameters) {

	auto ApplyColorStopList = [](CompiledShaderData& shader, const Rml::Dictionary& shader_parameters) {
		auto it = shader_parameters.find("color_stop_list");
		if (it == shader_parameters.end()) return;
		const Rml::ColorStopList& color_stop_list = it->second.GetReference<Rml::ColorStopList>();
		const int num_stops = Rml::Math::Min(static_cast<int>(color_stop_list.size()), MAX_NUM_STOPS);

		shader.stop_positions.resize(num_stops);
		shader.stop_colors.resize(num_stops);
		for (int i = 0; i < num_stops; i++) {
			const Rml::ColorStop& stop = color_stop_list[i];
			shader.stop_positions[i] = stop.position.number;
			shader.stop_colors[i] = ConvertToColorf(stop.color);
		}
	};

	auto* shader = new CompiledShaderData();

	if (name == "linear-gradient") {
		shader->type = CompiledShaderType::Gradient;
		const bool repeating = Rml::Get(parameters, "repeating", false);
		shader->gradient_function = repeating ? ShaderGradientFunction::RepeatingLinear : ShaderGradientFunction::Linear;
		shader->p = Rml::Get(parameters, "p0", Rml::Vector2f(0.f));
		shader->v = Rml::Get(parameters, "p1", Rml::Vector2f(0.f)) - shader->p;
		ApplyColorStopList(*shader, parameters);
	}
	else if (name == "radial-gradient") {
		shader->type = CompiledShaderType::Gradient;
		const bool repeating = Rml::Get(parameters, "repeating", false);
		shader->gradient_function = repeating ? ShaderGradientFunction::RepeatingRadial : ShaderGradientFunction::Radial;
		shader->p = Rml::Get(parameters, "center", Rml::Vector2f(0.f));
		shader->v = Rml::Vector2f(1.f) / Rml::Get(parameters, "radius", Rml::Vector2f(1.f));
		ApplyColorStopList(*shader, parameters);
	}
	else if (name == "conic-gradient") {
		shader->type = CompiledShaderType::Gradient;
		const bool repeating = Rml::Get(parameters, "repeating", false);
		shader->gradient_function = repeating ? ShaderGradientFunction::RepeatingConic : ShaderGradientFunction::Conic;
		shader->p = Rml::Get(parameters, "center", Rml::Vector2f(0.f));
		const float angle = Rml::Get(parameters, "angle", 0.f);
		shader->v = {Rml::Math::Cos(angle), Rml::Math::Sin(angle)};
		ApplyColorStopList(*shader, parameters);
	}
	else {
		Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported shader type '%s'.", name.c_str());
		delete shader;
		return Rml::CompiledShaderHandle(0);
	}

	return reinterpret_cast<Rml::CompiledShaderHandle>(shader);
}

void RenderInterface_DX12::RenderShader(
	Rml::CompiledShaderHandle shader_handle, Rml::CompiledGeometryHandle geometry_handle,
	Rml::Vector2f translation, Rml::TextureHandle /*texture*/) {
	ZoneScopedN("DX12::RenderShader");

	if (!shader_handle || !geometry_handle) return;

	const auto* shader = reinterpret_cast<const CompiledShaderData*>(shader_handle);
	auto* geom = reinterpret_cast<GeometryData*>(geometry_handle);

	if (shader->type != CompiledShaderType::Gradient) return;

	// Build transform CB (same as RenderGeometry)
	Rml::Matrix4f mvp = transform_active_ ? (projection_ * transform_) : projection_;

	TransformCB cb_data{};
	std::memcpy(cb_data.transform, mvp.data(), 16 * sizeof(float));
	cb_data.translate[0] = translation.x;
	cb_data.translate[1] = translation.y;

	auto cb = AllocateCB(sizeof(TransformCB), &cb_data);
	if (!cb.cpu_ptr) return;

	// Build gradient CB
	const int num_stops = static_cast<int>(shader->stop_positions.size());

	GradientCB grad_cb{};
	grad_cb.func = static_cast<int>(shader->gradient_function);
	grad_cb.num_stops = num_stops;
	grad_cb.p[0] = shader->p.x;
	grad_cb.p[1] = shader->p.y;
	grad_cb.v[0] = shader->v.x;
	grad_cb.v[1] = shader->v.y;

	for (int i = 0; i < num_stops && i < MAX_NUM_STOPS; ++i) {
		grad_cb.stop_colors[i * 4 + 0] = shader->stop_colors[i].red;
		grad_cb.stop_colors[i * 4 + 1] = shader->stop_colors[i].green;
		grad_cb.stop_colors[i * 4 + 2] = shader->stop_colors[i].blue;
		grad_cb.stop_colors[i * 4 + 3] = shader->stop_colors[i].alpha;
		grad_cb.stop_positions[i] = shader->stop_positions[i];
	}

	auto grad = AllocateCB(sizeof(GradientCB), &grad_cb);
	if (!grad.cpu_ptr) return;

	// Set PSO
	if (clip_mask_enabled_) {
		command_list_->SetPipelineState(pso_gradient_stencil_.Get());
		command_list_->OMSetStencilRef(stencil_ref_);
	} else {
		command_list_->SetPipelineState(pso_gradient_.Get());
	}

	// Bind CBVs
	command_list_->SetGraphicsRootConstantBufferView(0, cb.gpu_address);
	command_list_->SetGraphicsRootConstantBufferView(2, grad.gpu_address);

	// Set scissor
	if (scissor_enabled_) {
		command_list_->RSSetScissorRects(1, &scissor_rect_);
	} else {
		D3D12_RECT full_rect = { 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };
		command_list_->RSSetScissorRects(1, &full_rect);
	}

	// Draw
	command_list_->IASetVertexBuffers(0, 1, &geom->vbv);
	command_list_->IASetIndexBuffer(&geom->ibv);
	command_list_->DrawIndexedInstanced(static_cast<UINT>(geom->num_indices), 1, 0, 0, 0);
}

void RenderInterface_DX12::ReleaseShader(Rml::CompiledShaderHandle shader_handle) {
	if (!shader_handle) return;
	delete reinterpret_cast<CompiledShaderData*>(shader_handle);
}

// ===========================================================================
// Filter rendering helpers (Phase 6)
// ===========================================================================

static void ComputeBlurWeights(float sigma, float* weights, int num_weights) {
	float normalization = 0.0f;
	for (int i = 0; i < num_weights; i++) {
		if (std::fabs(sigma) < 0.1f)
			weights[i] = (i == 0) ? 1.0f : 0.0f;
		else
			weights[i] = std::exp(-float(i * i) / (2.0f * sigma * sigma)) /
				(std::sqrt(2.f * 3.14159265f) * sigma);
		normalization += (i == 0 ? 1.f : 2.0f) * weights[i];
	}
	for (int i = 0; i < num_weights; i++)
		weights[i] /= normalization;
}

void RenderInterface_DX12::RenderBlur(float sigma, int src_pp, int dst_pp) {
	ZoneScopedN("DX12::RenderBlur");
	// Two-pass separable Gaussian blur: vertical then horizontal.
	// src_pp -> dst_pp (vertical), dst_pp -> src_pp (horizontal).
	// After this call, the blurred result is in src_pp.

	auto* src = postprocess_targets_[src_pp];
	auto* dst = postprocess_targets_[dst_pp];

	// Compute blur weights
	float weights[BLUR_NUM_WEIGHTS];
	ComputeBlurWeights(sigma, weights, BLUR_NUM_WEIGHTS);

	// Fullscreen viewport/scissor
	D3D12_VIEWPORT viewport{};
	viewport.Width = static_cast<float>(width_);
	viewport.Height = static_cast<float>(height_);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	command_list_->RSSetViewports(1, &viewport);

	D3D12_RECT full_rect = { 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };
	command_list_->RSSetScissorRects(1, &full_rect);

	command_list_->SetPipelineState(pso_blur_.Get());

	// --- Pass 1: Vertical blur (src -> dst) ---
	{
		// Transition src to PSR, dst to RT
		D3D12_RESOURCE_BARRIER barriers[2] = {};
		barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[0].Transition.pResource = src->texture.Get();
		barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[1].Transition.pResource = dst->texture.Get();
		barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		command_list_->ResourceBarrier(2, barriers);

		// Build filter CB: vertical texel offset
		FilterCB cb{};
		auto* f = cb.data;
		f[0] = 0.0f;                                  // texel_offset.x
		f[1] = 1.0f / static_cast<float>(height_);    // texel_offset.y
		f[2] = 0.0f;                                  // _pad
		f[3] = 0.0f;                                  // _pad
		f[4] = weights[0]; f[5] = weights[1]; f[6] = weights[2]; f[7] = weights[3];

		auto cb_alloc = AllocateCB(sizeof(FilterCB), &cb);
		if (!cb_alloc.cpu_ptr) return;

		D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
		rtv.ptr += static_cast<SIZE_T>(dst->rtv_slot) * rtv_descriptor_size_;
		command_list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

		command_list_->SetGraphicsRootConstantBufferView(2, cb_alloc.gpu_address);

		D3D12_GPU_DESCRIPTOR_HANDLE srv = srv_heap_->GetGPUDescriptorHandleForHeapStart();
		srv.ptr += static_cast<UINT64>(src->srv_index) * srv_descriptor_size_;
		command_list_->SetGraphicsRootDescriptorTable(1, srv);

		command_list_->IASetVertexBuffers(0, 1, &fullscreen_quad_->vbv);
		command_list_->IASetIndexBuffer(&fullscreen_quad_->ibv);
		command_list_->DrawIndexedInstanced(6, 1, 0, 0, 0);
	}

	// --- Pass 2: Horizontal blur (dst -> src) ---
	{
		D3D12_RESOURCE_BARRIER barriers[2] = {};
		barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[0].Transition.pResource = dst->texture.Get();
		barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[1].Transition.pResource = src->texture.Get();
		barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		command_list_->ResourceBarrier(2, barriers);

		FilterCB cb{};
		auto* f = cb.data;
		f[0] = 1.0f / static_cast<float>(width_);     // texel_offset.x
		f[1] = 0.0f;                                   // texel_offset.y
		f[2] = 0.0f;
		f[3] = 0.0f;
		f[4] = weights[0]; f[5] = weights[1]; f[6] = weights[2]; f[7] = weights[3];

		auto cb_alloc = AllocateCB(sizeof(FilterCB), &cb);
		if (!cb_alloc.cpu_ptr) return;

		D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
		rtv.ptr += static_cast<SIZE_T>(src->rtv_slot) * rtv_descriptor_size_;
		command_list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

		command_list_->SetGraphicsRootConstantBufferView(2, cb_alloc.gpu_address);

		D3D12_GPU_DESCRIPTOR_HANDLE srv = srv_heap_->GetGPUDescriptorHandleForHeapStart();
		srv.ptr += static_cast<UINT64>(dst->srv_index) * srv_descriptor_size_;
		command_list_->SetGraphicsRootDescriptorTable(1, srv);

		command_list_->IASetVertexBuffers(0, 1, &fullscreen_quad_->vbv);
		command_list_->IASetIndexBuffer(&fullscreen_quad_->ibv);
		command_list_->DrawIndexedInstanced(6, 1, 0, 0, 0);
	}

	// After blur: src is in RT state (contains blurred result), dst is in PSR state.
	// Transition dst back to PSR (it already is) — no-op. Leave src in RT.
}

void RenderInterface_DX12::RenderFilters(
	Rml::Span<const Rml::CompiledFilterHandle> filters, int source_layer_idx) {
	ZoneScopedN("DX12::RenderFilters");

	if (filters.empty()) return;

	// The source layer should already be in PSR state (set by PopLayer).
	// Copy source layer into postprocess target 0 using passthrough.

	auto& src_layer = layer_pool_[source_layer_idx];
	auto* pp0 = postprocess_targets_[0];
	auto* pp1 = postprocess_targets_[1];

	// Transition pp0 to RT for writing
	{
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = pp0->texture.Get();
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		command_list_->ResourceBarrier(1, &barrier);
	}

	// Blit source layer -> pp0 (passthrough, no blend)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
		rtv.ptr += static_cast<SIZE_T>(pp0->rtv_slot) * rtv_descriptor_size_;
		command_list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

		D3D12_VIEWPORT viewport{};
		viewport.Width = static_cast<float>(width_);
		viewport.Height = static_cast<float>(height_);
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		command_list_->RSSetViewports(1, &viewport);

		D3D12_RECT full_rect = { 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };
		command_list_->RSSetScissorRects(1, &full_rect);

		command_list_->SetPipelineState(pso_passthrough_replace_.Get());

		D3D12_GPU_DESCRIPTOR_HANDLE srv = srv_heap_->GetGPUDescriptorHandleForHeapStart();
		srv.ptr += static_cast<UINT64>(src_layer.srv_index) * srv_descriptor_size_;
		command_list_->SetGraphicsRootDescriptorTable(1, srv);

		command_list_->IASetVertexBuffers(0, 1, &fullscreen_quad_->vbv);
		command_list_->IASetIndexBuffer(&fullscreen_quad_->ibv);
		command_list_->DrawIndexedInstanced(6, 1, 0, 0, 0);
	}

	// pp0 is now in RT state with the source data.
	// Apply each filter, ping-ponging between pp0 and pp1.
	// After each filter, the "primary" (result) is in pp0.
	// The convention: pp0 is the "primary" (current result in RT), pp1 is "secondary" (in PSR).

	for (const Rml::CompiledFilterHandle filter_handle : filters) {
		if (!filter_handle) continue;

		// Try to interpret as CompiledFilterData first
		auto* filter = reinterpret_cast<const CompiledFilterData*>(filter_handle);

		// Check if this is actually a TextureData from SaveLayerAsMaskImage
		// TextureData has ComPtr as first member (8 bytes pointer), FilterType is an int.
		// Valid FilterType values are 1-5. If the first 4 bytes don't match, it's a TextureData.
		if (filter->type < FilterType::Passthrough || filter->type > FilterType::MaskImage) {
			// This is a TextureData* from SaveLayerAsMaskImage — treat as mask image filter
			auto* mask_tex = reinterpret_cast<TextureData*>(filter_handle);

			// Transition pp0 to PSR, pp1 to RT
			D3D12_RESOURCE_BARRIER barriers[2] = {};
			barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[0].Transition.pResource = pp0->texture.Get();
			barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[1].Transition.pResource = pp1->texture.Get();
			barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			command_list_->ResourceBarrier(2, barriers);

			command_list_->SetPipelineState(pso_blend_mask_.Get());

			D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
			rtv.ptr += static_cast<SIZE_T>(pp1->rtv_slot) * rtv_descriptor_size_;
			command_list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

			// Bind source (pp0) to t0
			D3D12_GPU_DESCRIPTOR_HANDLE srv0 = srv_heap_->GetGPUDescriptorHandleForHeapStart();
			srv0.ptr += static_cast<UINT64>(pp0->srv_index) * srv_descriptor_size_;
			command_list_->SetGraphicsRootDescriptorTable(1, srv0);

			// Bind mask to t1
			D3D12_GPU_DESCRIPTOR_HANDLE srv1 = srv_heap_->GetGPUDescriptorHandleForHeapStart();
			srv1.ptr += static_cast<UINT64>(mask_tex->srv_index) * srv_descriptor_size_;
			command_list_->SetGraphicsRootDescriptorTable(3, srv1);

			command_list_->IASetVertexBuffers(0, 1, &fullscreen_quad_->vbv);
			command_list_->IASetIndexBuffer(&fullscreen_quad_->ibv);
			command_list_->DrawIndexedInstanced(6, 1, 0, 0, 0);

			// Swap: pp1 is now the result (in RT), pp0 is in PSR
			std::swap(postprocess_targets_[0], postprocess_targets_[1]);
			pp0 = postprocess_targets_[0];
			pp1 = postprocess_targets_[1];
			continue;
		}

		switch (filter->type) {
		case FilterType::Passthrough: {
			// Opacity: multiply all channels by blend_factor
			// Implement as color matrix with diagonal = blend_factor
			FilterCB cb{};
			float bf = filter->blend_factor;
			// Color matrix: diagonal = (bf, bf, bf, bf), rest = 0
			// column_major layout: col0 = {bf,0,0,0}, col1 = {0,bf,0,0}, ...
			cb.data[0] = bf; cb.data[1] = 0; cb.data[2] = 0; cb.data[3] = 0;
			cb.data[4] = 0; cb.data[5] = bf; cb.data[6] = 0; cb.data[7] = 0;
			cb.data[8] = 0; cb.data[9] = 0; cb.data[10] = bf; cb.data[11] = 0;
			cb.data[12] = 0; cb.data[13] = 0; cb.data[14] = 0; cb.data[15] = bf;

			auto cb_alloc = AllocateCB(sizeof(FilterCB), &cb);
			if (!cb_alloc.cpu_ptr) continue;

			// Transition pp0 to PSR, pp1 to RT
			D3D12_RESOURCE_BARRIER barriers[2] = {};
			barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[0].Transition.pResource = pp0->texture.Get();
			barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[1].Transition.pResource = pp1->texture.Get();
			barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			command_list_->ResourceBarrier(2, barriers);

			command_list_->SetPipelineState(pso_color_matrix_.Get());

			D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
			rtv.ptr += static_cast<SIZE_T>(pp1->rtv_slot) * rtv_descriptor_size_;
			command_list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

			command_list_->SetGraphicsRootConstantBufferView(2, cb_alloc.gpu_address);

			D3D12_GPU_DESCRIPTOR_HANDLE srv = srv_heap_->GetGPUDescriptorHandleForHeapStart();
			srv.ptr += static_cast<UINT64>(pp0->srv_index) * srv_descriptor_size_;
			command_list_->SetGraphicsRootDescriptorTable(1, srv);

			command_list_->IASetVertexBuffers(0, 1, &fullscreen_quad_->vbv);
			command_list_->IASetIndexBuffer(&fullscreen_quad_->ibv);
			command_list_->DrawIndexedInstanced(6, 1, 0, 0, 0);

			std::swap(postprocess_targets_[0], postprocess_targets_[1]);
			pp0 = postprocess_targets_[0];
			pp1 = postprocess_targets_[1];
			break;
		}

		case FilterType::Blur: {
			// pp0 is in RT with current data. RenderBlur expects src in RT.
			RenderBlur(filter->sigma, 0, 1);
			// After RenderBlur: pp0 is in RT with blurred result, pp1 is in PSR.
			pp0 = postprocess_targets_[0];
			pp1 = postprocess_targets_[1];
			break;
		}

		case FilterType::DropShadow: {
			// 1. Render shadow: sample pp0 with UV offset, tint with shadow color -> pp1
			// 2. Optionally blur pp1
			// 3. Composite pp0 (original) on top of pp1 (shadow)
			// Result is in pp0 after swapping.

			// Step 1: Create shadow in pp1
			{
				D3D12_RESOURCE_BARRIER barriers[2] = {};
				barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barriers[0].Transition.pResource = pp0->texture.Get();
				barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
				barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
				barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barriers[1].Transition.pResource = pp1->texture.Get();
				barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
				barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
				command_list_->ResourceBarrier(2, barriers);

				FilterCB cb{};
				// UV offset: screen-space offset divided by dimensions, inverted for DX UV convention
				cb.data[0] = -filter->offset.x / static_cast<float>(width_);   // uv_offset.x
				cb.data[1] = filter->offset.y / static_cast<float>(height_);    // uv_offset.y
				cb.data[2] = 0.0f;
				cb.data[3] = 0.0f;
				Rml::Colourf shadow_colorf = ConvertToColorf(filter->color);
				cb.data[4] = shadow_colorf.red;
				cb.data[5] = shadow_colorf.green;
				cb.data[6] = shadow_colorf.blue;
				cb.data[7] = shadow_colorf.alpha;

				auto cb_alloc = AllocateCB(sizeof(FilterCB), &cb);
				if (!cb_alloc.cpu_ptr) continue;

				command_list_->SetPipelineState(pso_drop_shadow_.Get());

				D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
				rtv.ptr += static_cast<SIZE_T>(pp1->rtv_slot) * rtv_descriptor_size_;
				command_list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

				command_list_->SetGraphicsRootConstantBufferView(2, cb_alloc.gpu_address);

				D3D12_GPU_DESCRIPTOR_HANDLE srv = srv_heap_->GetGPUDescriptorHandleForHeapStart();
				srv.ptr += static_cast<UINT64>(pp0->srv_index) * srv_descriptor_size_;
				command_list_->SetGraphicsRootDescriptorTable(1, srv);

				command_list_->IASetVertexBuffers(0, 1, &fullscreen_quad_->vbv);
				command_list_->IASetIndexBuffer(&fullscreen_quad_->ibv);
				command_list_->DrawIndexedInstanced(6, 1, 0, 0, 0);
			}

			// pp0 is in PSR (original), pp1 is in RT (shadow)

			// Step 2: Blur the shadow if sigma >= 0.5
			if (filter->sigma >= 0.5f) {
				// RenderBlur expects src in RT state. pp1 is in RT.
				// Swap so blur operates on pp1 as index 0.
				std::swap(postprocess_targets_[0], postprocess_targets_[1]);
				pp0 = postprocess_targets_[0];
				pp1 = postprocess_targets_[1];
				RenderBlur(filter->sigma, 0, 1);
				// After blur: pp0 (blurred shadow) in RT, pp1 (original) in PSR.
			}

			// Step 3: Composite original on top of shadow.
			// The original is in pp1 (PSR state), shadow is in pp0 (RT state).
			// We want: shadow + original composited = result.
			// Draw original (pp1) onto shadow (pp0) with premultiplied blend.
			{
				// pp1 should be in PSR, pp0 in RT — which is the case after blur or after step 1 swap.
				// If no blur happened: pp0=PSR(original), pp1=RT(shadow).
				// We need to render original onto shadow. Let's ensure pp1 is PSR with original.
				// Actually after step 1 (no blur): pp0=PSR(original), pp1=RT(shadow). Good.
				// After step 2 (blur): pp0=RT(blurred shadow), pp1=PSR(original). Good.

				// If no blur: shadow is in pp1 (RT), original in pp0 (PSR)
				// Need to render pp0 onto pp1.
				// If blur: blurred shadow in pp0 (RT), original in pp1 (PSR)
				// Need to render pp1 onto pp0.
				// In both cases: draw the PSR target onto the RT target with premultiplied blend.

				int psr_idx = (filter->sigma >= 0.5f) ? 1 : 0;
				int rt_idx = (filter->sigma >= 0.5f) ? 0 : 1;

				auto* psr_pp = postprocess_targets_[psr_idx];
				auto* rt_pp = postprocess_targets_[rt_idx];

				command_list_->SetPipelineState(pso_passthrough_blend_.Get());

				D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
				rtv.ptr += static_cast<SIZE_T>(rt_pp->rtv_slot) * rtv_descriptor_size_;
				command_list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

				D3D12_GPU_DESCRIPTOR_HANDLE srv = srv_heap_->GetGPUDescriptorHandleForHeapStart();
				srv.ptr += static_cast<UINT64>(psr_pp->srv_index) * srv_descriptor_size_;
				command_list_->SetGraphicsRootDescriptorTable(1, srv);

				command_list_->IASetVertexBuffers(0, 1, &fullscreen_quad_->vbv);
				command_list_->IASetIndexBuffer(&fullscreen_quad_->ibv);
				command_list_->DrawIndexedInstanced(6, 1, 0, 0, 0);

				// Result is in rt_pp (RT state). Make sure it's postprocess_targets_[0].
				if (rt_idx != 0) {
					std::swap(postprocess_targets_[0], postprocess_targets_[1]);
				}
				pp0 = postprocess_targets_[0];
				pp1 = postprocess_targets_[1];
			}
			break;
		}

		case FilterType::ColorMatrix: {
			// Build filter CB with color matrix.
			// RmlUi uses ColumnMajorMatrix4f by default. The HLSL CB expects column_major float4x4.
			// data() returns 16 floats in column-major order — matches HLSL column_major.
			FilterCB cb{};
			std::memcpy(cb.data, filter->color_matrix.data(), 16 * sizeof(float));

			auto cb_alloc = AllocateCB(sizeof(FilterCB), &cb);
			if (!cb_alloc.cpu_ptr) continue;

			D3D12_RESOURCE_BARRIER barriers[2] = {};
			barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[0].Transition.pResource = pp0->texture.Get();
			barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[1].Transition.pResource = pp1->texture.Get();
			barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			command_list_->ResourceBarrier(2, barriers);

			command_list_->SetPipelineState(pso_color_matrix_.Get());

			D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
			rtv.ptr += static_cast<SIZE_T>(pp1->rtv_slot) * rtv_descriptor_size_;
			command_list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

			command_list_->SetGraphicsRootConstantBufferView(2, cb_alloc.gpu_address);

			D3D12_GPU_DESCRIPTOR_HANDLE srv = srv_heap_->GetGPUDescriptorHandleForHeapStart();
			srv.ptr += static_cast<UINT64>(pp0->srv_index) * srv_descriptor_size_;
			command_list_->SetGraphicsRootDescriptorTable(1, srv);

			command_list_->IASetVertexBuffers(0, 1, &fullscreen_quad_->vbv);
			command_list_->IASetIndexBuffer(&fullscreen_quad_->ibv);
			command_list_->DrawIndexedInstanced(6, 1, 0, 0, 0);

			std::swap(postprocess_targets_[0], postprocess_targets_[1]);
			pp0 = postprocess_targets_[0];
			pp1 = postprocess_targets_[1];
			break;
		}

		case FilterType::MaskImage: {
			// Should not happen — MaskImage filters come from SaveLayerAsMaskImage
			// which stores TextureData* directly, handled above.
			break;
		}

		case FilterType::Invalid:
			break;
		}
	}

	// After all filters, result is in pp0 (RT state).
	// Transition pp0 to PSR so CompositeLayers can sample it.
	{
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = pp0->texture.Get();
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		command_list_->ResourceBarrier(1, &barrier);
	}
}
