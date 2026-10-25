#import "RmlUi_Renderer_Metal.h"

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#if TARGET_OS_IOS
#import <UIKit/UIKit.h>
#else
#import <AppKit/AppKit.h>
#endif
#import <simd/simd.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Math.h>

#include <lunasvg/lunasvg.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

static NSString* const kRmlUiShaderSource = @R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Uniforms {
    float4x4 transform;
    float2   translation;
    float2   _padding;
};

struct VertexIn {
    float2 position [[attribute(0)]];
    float4 color    [[attribute(1)]];
    float2 texcoord [[attribute(2)]];
};

struct VertexOut {
    float4 clip_position [[position]];
    float4 color;
    float2 texcoord;
};

vertex VertexOut rmlui_vertex(VertexIn in [[stage_in]],
                               constant Uniforms& u [[buffer(1)]])
{
    VertexOut out;
    float2 world_pos     = in.position + u.translation;
    out.clip_position    = u.transform * float4(world_pos, 0.0, 1.0);
    out.color            = in.color;
    out.texcoord         = in.texcoord;
    return out;
}

fragment float4 rmlui_fragment_color(VertexOut in [[stage_in]])
{
    return in.color;
}

fragment float4 rmlui_fragment_texture(VertexOut in         [[stage_in]],
                                       texture2d<float> tex [[texture(0)]],
                                       sampler samp         [[sampler(0)]])
{
    return in.color * tex.sample(samp, in.texcoord);
}

float3 bt709_limited_to_rgb(float y, float2 cbcr)
{
    const float  Y = (y - 16.0/255.0) * (255.0/219.0);
    const float2 C = (cbcr - float2(128.0/255.0)) * (255.0/224.0);
    return float3(Y + 1.5748 * C.y,
                  Y - 0.1873 * C.x - 0.4681 * C.y,
                  Y + 1.8556 * C.x);
}

fragment float4 rmlui_fragment_nv12(VertexOut in          [[stage_in]],
                                    texture2d<float> y_tex  [[texture(0)]],
                                    texture2d<float> uv_tex [[texture(1)]],
                                    sampler samp            [[sampler(0)]])
{
    float  y    = y_tex.sample(samp, in.texcoord).r;
    float2 cbcr = uv_tex.sample(samp, in.texcoord).rg;
    return in.color * float4(clamp(bt709_limited_to_rgb(y, cbcr), 0.0, 1.0), 1.0);
}

#define GRADIENT_MAX_STOPS        16
#define GRADIENT_LINEAR           0
#define GRADIENT_RADIAL           1
#define GRADIENT_CONIC            2
#define GRADIENT_REPEATING_LINEAR 3
#define GRADIENT_REPEATING_RADIAL 4
#define GRADIENT_REPEATING_CONIC  5

struct GradientUniforms {
    float4 stop_colors[GRADIENT_MAX_STOPS];
    float  stop_positions[GRADIENT_MAX_STOPS];
    float2 p;
    float2 v;
    int    func;
    int    num_stops;
    float2 _pad;
};

fragment float4 rmlui_fragment_gradient(VertexOut in [[stage_in]],
                                        constant GradientUniforms& g [[buffer(2)]])
{
    if (g.num_stops <= 0)
        return float4(0.0);

    float t = 0.0;

    if (g.func == GRADIENT_LINEAR || g.func == GRADIENT_REPEATING_LINEAR) {
        float2 v = g.v;
        float dist_sq = max(dot(v, v), 1.0e-12);
        float2 V = in.texcoord - g.p;
        t = dot(v, V) / dist_sq;
    } else if (g.func == GRADIENT_RADIAL || g.func == GRADIENT_REPEATING_RADIAL) {
        float2 V = in.texcoord - g.p;
        t = length(g.v * V);
    } else if (g.func == GRADIENT_CONIC || g.func == GRADIENT_REPEATING_CONIC) {
        float2x2 R = float2x2(g.v.x, -g.v.y, g.v.y, g.v.x);
        float2 V = R * (in.texcoord - g.p);
        t = 0.5 + atan2(-V.x, V.y) / (2.0 * M_PI_F);
    }

    if (g.func == GRADIENT_REPEATING_LINEAR || g.func == GRADIENT_REPEATING_RADIAL || g.func == GRADIENT_REPEATING_CONIC) {
        float t0 = g.stop_positions[0];
        float t1 = g.stop_positions[g.num_stops - 1];
        float period = t1 - t0;
        if (period > 1.0e-6)
            t = t0 + period * fract((t - t0) / period);
    }

    float4 color = g.stop_colors[0];
    for (int i = 1; i < min(g.num_stops, GRADIENT_MAX_STOPS); i++) {
        float p0 = g.stop_positions[i - 1];
        float p1 = g.stop_positions[i];
        float weight = (p1 > p0 + 1.0e-6) ? clamp((t - p0) / (p1 - p0), 0.0, 1.0) : step(p1, t);
        color = mix(color, g.stop_colors[i], weight);
    }

    return in.color * color;
}

)MSL";

struct Uniforms {
    simd_float4x4 transform;
    simd_float2   translation;
    simd_float2   _padding;
};

struct MetalResourceUse {
    std::mutex mutex;
    std::condition_variable completed;
    uint32_t in_flight = 0;
};

static void WaitUntilResourceIdle(const std::shared_ptr<MetalResourceUse>& use)
{
    std::unique_lock<std::mutex> lock(use->mutex);
    use->completed.wait(lock, [&] { return use->in_flight == 0; });
}

static void MarkResourceUsed(const std::shared_ptr<MetalResourceUse>& use,
                             uint64_t& last_frame,
                             uint64_t frame,
                             id<MTLCommandBuffer> command_buffer)
{
    if (!command_buffer || last_frame == frame)
        return;

    last_frame = frame;
    {
        std::lock_guard<std::mutex> lock(use->mutex);
        ++use->in_flight;
    }

    const std::shared_ptr<MetalResourceUse> completion_use = use;
    [command_buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
        {
            std::lock_guard<std::mutex> lock(completion_use->mutex);
            RMLUI_ASSERT(completion_use->in_flight > 0);
            --completion_use->in_flight;
        }
        completion_use->completed.notify_all();
    }];
}

struct MetalGeometry {
    static constexpr int kSlots = 3;

    id<MTLBuffer> vertex_buffers[kSlots] = {nil, nil, nil};
    std::shared_ptr<MetalResourceUse> vertex_use[kSlots];
    uint64_t last_usage_frame[kSlots] = {};
    bool safe_to_overwrite[kSlots] = {};
    id<MTLBuffer> index_buffer = nil;
    NSUInteger index_count = 0;
    int active_slot = 0;
    bool dynamic = false;

    MetalGeometry()
    {
        for (auto& use : vertex_use)
            use = std::make_shared<MetalResourceUse>();
    }

    ~MetalGeometry()
    {
        for (id<MTLBuffer> buffer : vertex_buffers)
            [buffer release];
        [index_buffer release];
    }
};

struct MetalTexture {
    static constexpr int kSlots = 3;

    id<MTLTexture> textures[kSlots] = {nil, nil, nil};
    std::shared_ptr<MetalResourceUse> texture_use[kSlots];
    uint64_t last_usage_frame[kSlots] = {};
    bool safe_to_overwrite[kSlots] = {};
    int active_slot = 0;
    int width = 0;
    int height = 0;
    bool streaming = false;

    MetalTexture()
    {
        for (auto& use : texture_use)
            use = std::make_shared<MetalResourceUse>();
    }

    ~MetalTexture()
    {
        for (id<MTLTexture> texture : textures)
            [texture release];
    }
};

struct MetalNV12Texture {
    static constexpr int kSlots = 3;
    id<MTLTexture> y_tex[kSlots]  = {nil, nil, nil};
    id<MTLTexture> uv_tex[kSlots] = {nil, nil, nil};
    std::shared_ptr<MetalResourceUse> texture_use[kSlots];
    uint64_t last_usage_frame[kSlots] = {};
    uint32_t slot_width[kSlots] = {};
    uint32_t slot_height[kSlots] = {};
    uint32_t width  = 0;
    uint32_t height = 0;
    int slot        = 0;

    MetalNV12Texture()
    {
        for (auto& use : texture_use)
            use = std::make_shared<MetalResourceUse>();
    }

    ~MetalNV12Texture()
    {
        for (int i = 0; i < kSlots; ++i) {
            [y_tex[i] release];
            [uv_tex[i] release];
        }
    }
};

#define MAX_GRADIENT_STOPS 16
struct GradientUniforms {
    simd_float4 stop_colors[MAX_GRADIENT_STOPS];
    float       stop_positions[MAX_GRADIENT_STOPS];
    simd_float2 p;
    simd_float2 v;
    int         func;
    int         num_stops;
    float       _pad[2];
};
static_assert(sizeof(GradientUniforms) == 352);
static_assert(offsetof(GradientUniforms, p) == 320 && offsetof(GradientUniforms, func) == 336);

enum class GradientFunc : int {
    Linear          = 0,
    Radial          = 1,
    Conic           = 2,
    RepeatingLinear = 3,
    RepeatingRadial = 4,
    RepeatingConic  = 5,
};

struct CompiledShader_Metal {
    GradientUniforms uniforms = {};
};

struct StencilClipState {
    enum class Pass { WriteGeometry, FillViewportThenClearGeometry, IncrementWhereEqual };
    struct Plan {
        Pass    pass;
        uint8_t fill_ref;
        uint8_t geometry_ref;
    };

    uint8_t ref = 0;

    Plan plan(Rml::ClipMaskOperation operation)
    {
        switch (operation) {
        case Rml::ClipMaskOperation::Set:
            return {Pass::WriteGeometry, 0, ++ref};
        case Rml::ClipMaskOperation::SetInverse:
            return {Pass::FillViewportThenClearGeometry, ++ref, 0};
        case Rml::ClipMaskOperation::Intersect:
            break;
        }
        return {Pass::IncrementWhereEqual, 0, ref++};
    }
};

struct StencilBindCache {
    id<MTLDepthStencilState> state = nil;
    uint32_t reference = 0;
    bool reference_valid = false;

    void bind(id<MTLRenderCommandEncoder> encoder, id<MTLDepthStencilState> wanted, uint32_t wanted_reference)
    {
        if (state != wanted) {
            [encoder setDepthStencilState:wanted];
            state = wanted;
        }
        if (!reference_valid || reference != wanted_reference) {
            [encoder setStencilReferenceValue:wanted_reference];
            reference = wanted_reference;
            reference_valid = true;
        }
    }
};

struct RenderInterface_Metal::Data {
    id<MTLDevice>              device = nil;

    id<MTLRenderPipelineState> pipeline_color = nil;
    id<MTLRenderPipelineState> pipeline_texture = nil;
    id<MTLRenderPipelineState> pipeline_stencil = nil;
    id<MTLRenderPipelineState> pipeline_gradient = nil;
    id<MTLRenderPipelineState> pipeline_nv12 = nil;
    id<MTLSamplerState>        sampler = nil;

    id<MTLDepthStencilState>   dss_normal = nil;
    id<MTLDepthStencilState>   dss_test = nil;
    id<MTLDepthStencilState>   dss_write = nil;
    id<MTLDepthStencilState>   dss_write_incr = nil;

    id<MTLCommandBuffer>          current_command_buffer   = nil;
    id<MTLRenderCommandEncoder>   current_encoder          = nil;

    int viewport_width  = 0;
    int viewport_height = 0;
    int viewport_top    = 0;

    bool scissor_enabled = false;
    bool scissor_empty = false;
    MTLScissorRect scissor_rect = {0, 0, 1, 1};

    Rml::Matrix4f transform;
    bool has_transform = false;

    StencilClipState clip;
    id<MTLDepthStencilState> active_dss = nil;
    uint32_t active_stencil_ref = 0;

    id<MTLRenderPipelineState> bound_pipeline = nil;
    StencilBindCache stencil_binding;
    id<MTLTexture> bound_fragment_texture_0 = nil;
    id<MTLTexture> bound_fragment_texture_1 = nil;
    bool sampler_bound = false;

    simd_float4x4 projection = matrix_identity_float4x4;
    uint64_t frame = 0;
    bool valid = false;

};

// RmlUi origin is top-left, +Y down. Metal NDC origin is center, +Y up.
static simd_float4x4 OrthoMatrix(float w, float h)
{
    RMLUI_ASSERT(w > 0.0f && h > 0.0f);
    float sx = 2.0f / w;
    float sy = -2.0f / h;
    simd_float4x4 m = matrix_identity_float4x4;
    m.columns[0][0] = sx;
    m.columns[1][1] = sy;
    m.columns[3][0] = -1.0f;
    m.columns[3][1] =  1.0f;
    return m;
}

static void BindPipeline(id<MTLRenderCommandEncoder> encoder,
                         id<MTLRenderPipelineState>& bound,
                         id<MTLRenderPipelineState> pipeline)
{
    if (bound != pipeline) {
        [encoder setRenderPipelineState:pipeline];
        bound = pipeline;
    }
}

static void BindFragmentTexture(id<MTLRenderCommandEncoder> encoder,
                                id<MTLTexture>& bound,
                                id<MTLTexture> texture,
                                NSUInteger index)
{
    if (bound != texture) {
        [encoder setFragmentTexture:texture atIndex:index];
        bound = texture;
    }
}

static simd_float4x4 RmlMatrixToSimd(const Rml::Matrix4f& m)
{
    simd_float4x4 result;
    // Rml::Matrix4f is column-major, same as simd
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            result.columns[col][row] = m[col][row];
    return result;
}

static id<MTLRenderPipelineState> BuildPipeline(id<MTLDevice> device,
                                                 id<MTLLibrary> library,
                                                 NSString* frag_name,
                                                 MTLPixelFormat color_format,
                                                 MTLPixelFormat depth_stencil_format,
                                                 BOOL write_color)
{
    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    desc.label = [@"RmlUi " stringByAppendingString:frag_name];
    id<MTLFunction> vertex_function = [library newFunctionWithName:@"rmlui_vertex"];
    id<MTLFunction> fragment_function = [library newFunctionWithName:frag_name];
    desc.vertexFunction = vertex_function;
    desc.fragmentFunction = fragment_function;

    if (!desc.vertexFunction)
        NSLog(@"[RmlUi] Metal: 'rmlui_vertex' not found in library");
    if (!desc.fragmentFunction)
        NSLog(@"[RmlUi] Metal: '%@' not found in library", frag_name);
    if (!desc.vertexFunction || !desc.fragmentFunction) {
        [vertex_function release];
        [fragment_function release];
        [desc release];
        return nil;
    }

    static_assert(std::is_standard_layout_v<Rml::Vertex>);
    static_assert(sizeof(int) == sizeof(uint32_t));

    MTLVertexDescriptor* vd = [MTLVertexDescriptor new];
    vd.attributes[0].format      = MTLVertexFormatFloat2;
    vd.attributes[0].offset      = offsetof(Rml::Vertex, position);
    vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format      = MTLVertexFormatUChar4Normalized;
    vd.attributes[1].offset      = offsetof(Rml::Vertex, colour);
    vd.attributes[1].bufferIndex = 0;
    vd.attributes[2].format      = MTLVertexFormatFloat2;
    vd.attributes[2].offset      = offsetof(Rml::Vertex, tex_coord);
    vd.attributes[2].bufferIndex = 0;
    vd.layouts[0].stride         = sizeof(Rml::Vertex);
    vd.layouts[0].stepFunction   = MTLVertexStepFunctionPerVertex;
    desc.vertexDescriptor = vd;

    // Stencil attachment format (must match MTKView.depthStencilPixelFormat)
    if (depth_stencil_format != MTLPixelFormatInvalid) {
        desc.depthAttachmentPixelFormat   = depth_stencil_format;
        desc.stencilAttachmentPixelFormat = depth_stencil_format;
    }

    MTLRenderPipelineColorAttachmentDescriptor* ca = desc.colorAttachments[0];
    ca.pixelFormat = color_format;
    if (write_color) {
        ca.blendingEnabled             = YES;
        ca.sourceRGBBlendFactor        = MTLBlendFactorOne;
        ca.destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
        ca.sourceAlphaBlendFactor      = MTLBlendFactorOne;
        ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    } else {
        ca.writeMask = MTLColorWriteMaskNone;
    }

    NSError* error = nil;
    id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (!pso) {
        const char* description = error ? error.localizedDescription.UTF8String : "unknown error";
        Rml::Log::Message(Rml::Log::LT_ERROR, "Metal pipeline error: %s",
                          description);
    }
    [vd release];
    [vertex_function release];
    [fragment_function release];
    [desc release];
    return pso;
}

static id<MTLDepthStencilState> BuildDSS(id<MTLDevice> device,
                                          MTLCompareFunction compare,
                                          MTLStencilOperation pass_op,
                                          uint32_t write_mask)
{
    MTLStencilDescriptor* s = [MTLStencilDescriptor new];
    s.stencilCompareFunction    = compare;
    s.stencilFailureOperation   = MTLStencilOperationKeep;
    s.depthFailureOperation     = MTLStencilOperationKeep;
    s.depthStencilPassOperation = pass_op;
    s.readMask                  = 0xff;
    s.writeMask                 = write_mask;

    MTLDepthStencilDescriptor* dsd = [MTLDepthStencilDescriptor new];
    dsd.depthCompareFunction = MTLCompareFunctionAlways;
    dsd.depthWriteEnabled    = NO;
    dsd.frontFaceStencil     = s;
    dsd.backFaceStencil      = s;
    id<MTLDepthStencilState> state = [device newDepthStencilStateWithDescriptor:dsd];
    [dsd release];
    [s release];
    return state;
}

RenderInterface_Metal::RenderInterface_Metal(id<MTLDevice> device, MTKView* view)
{
    m_data = new Data();
    if (!device || !view) {
        Rml::Log::Message(Rml::Log::LT_ERROR, "Metal renderer requires a valid device and MTKView");
        return;
    }
    m_data->device = [device retain];

    // Compile shaders from embedded source at runtime.
    // This avoids relying on Xcode's "Compile Metal Sources" build phase.
    NSError* error = nil;
    MTLCompileOptions* opts = [MTLCompileOptions new];
    opts.languageVersion = MTLLanguageVersion2_4;
    id<MTLLibrary> library = [device newLibraryWithSource:kRmlUiShaderSource
                                                  options:opts
                                                    error:&error];
    [opts release];
    if (!library) {
        Rml::Log::Message(Rml::Log::LT_ERROR, "Metal shader compilation failed: %s",
                          error ? error.localizedDescription.UTF8String : "unknown error");
        return;
    }

    MTLPixelFormat color_fmt   = view.colorPixelFormat;
    MTLPixelFormat stencil_fmt = view.depthStencilPixelFormat;
    NSLog(@"[RmlUi] Building Metal pipelines (color=%lu stencil=%lu)",
          (unsigned long)color_fmt, (unsigned long)stencil_fmt);
    m_data->pipeline_color    = BuildPipeline(device, library, @"rmlui_fragment_color",    color_fmt, stencil_fmt, YES);
    m_data->pipeline_texture  = BuildPipeline(device, library, @"rmlui_fragment_texture",  color_fmt, stencil_fmt, YES);
    m_data->pipeline_stencil  = BuildPipeline(device, library, @"rmlui_fragment_color",    color_fmt, stencil_fmt, NO);
    m_data->pipeline_gradient = BuildPipeline(device, library, @"rmlui_fragment_gradient", color_fmt, stencil_fmt, YES);
    m_data->pipeline_nv12     = BuildPipeline(device, library, @"rmlui_fragment_nv12",     color_fmt, stencil_fmt, YES);
    [library release];
    NSLog(@"[RmlUi] Pipelines: color=%s texture=%s stencil=%s gradient=%s nv12=%s",
          m_data->pipeline_color    ? "OK" : "FAILED",
          m_data->pipeline_texture  ? "OK" : "FAILED",
          m_data->pipeline_stencil  ? "OK" : "FAILED",
          m_data->pipeline_gradient ? "OK" : "FAILED",
          m_data->pipeline_nv12     ? "OK" : "FAILED");

    m_data->dss_normal     = BuildDSS(device, MTLCompareFunctionAlways, MTLStencilOperationKeep,           0);
    m_data->dss_test       = BuildDSS(device, MTLCompareFunctionEqual,  MTLStencilOperationKeep,           0);
    m_data->dss_write      = BuildDSS(device, MTLCompareFunctionAlways, MTLStencilOperationReplace,     0xff);
    m_data->dss_write_incr = BuildDSS(device, MTLCompareFunctionEqual,  MTLStencilOperationIncrementClamp, 0xff);
    m_data->active_dss     = m_data->dss_normal;

    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter    = MTLSamplerMinMagFilterLinear;
    sd.magFilter    = MTLSamplerMinMagFilterLinear;
    sd.mipFilter    = MTLSamplerMipFilterNearest;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.label        = @"RmlUi linear clamp sampler";
    m_data->sampler = [device newSamplerStateWithDescriptor:sd];
    [sd release];

    m_data->transform = Rml::Matrix4f::Identity();
    m_data->valid = m_data->pipeline_color && m_data->pipeline_texture &&
        m_data->pipeline_stencil && m_data->pipeline_gradient && m_data->pipeline_nv12 &&
        m_data->dss_normal && m_data->dss_test && m_data->dss_write &&
        m_data->dss_write_incr && m_data->sampler;

    if (!m_data->valid)
        Rml::Log::Message(Rml::Log::LT_ERROR, "Metal renderer initialization is incomplete");
}

RenderInterface_Metal::~RenderInterface_Metal()
{
	if (m_data) {
		if (m_data->current_encoder)
			[m_data->current_encoder endEncoding];
		[m_data->pipeline_color release];
		[m_data->pipeline_texture release];
		[m_data->pipeline_stencil release];
		[m_data->pipeline_gradient release];
		[m_data->pipeline_nv12 release];
		[m_data->dss_normal release];
		[m_data->dss_test release];
		[m_data->dss_write release];
		[m_data->dss_write_incr release];
		[m_data->sampler release];
		[m_data->device release];
	}
    delete m_data;
}

RenderInterface_Metal::operator bool() const
{
    return m_data && m_data->valid;
}

bool RenderInterface_Metal::IsFrameActive() const
{
    return m_data && m_data->current_encoder;
}

void RenderInterface_Metal::SetViewport(int width, int height)
{
    if (!m_data) return;
    m_data->viewport_width  = std::max(width, 0);
    m_data->viewport_height = std::max(height, 0);
    if (m_data->viewport_width > 0 && m_data->viewport_height > 0)
        m_data->projection = OrthoMatrix((float)m_data->viewport_width, (float)m_data->viewport_height);
    m_data->scissor_rect = {0, (NSUInteger)m_data->viewport_top,
                            (NSUInteger)m_data->viewport_width, (NSUInteger)m_data->viewport_height};
    m_data->scissor_empty = (m_data->viewport_width == 0 || m_data->viewport_height == 0);
}

void RenderInterface_Metal::SetViewportTopOffset(int top)
{
    if (!m_data) return;
    m_data->viewport_top = std::max(top, 0);
    if (!m_data->scissor_enabled) {
        m_data->scissor_rect = {0, (NSUInteger)m_data->viewport_top,
                                (NSUInteger)m_data->viewport_width,
                                (NSUInteger)m_data->viewport_height};
    }
}

void RenderInterface_Metal::BeginFrame(id<MTLCommandBuffer> command_buffer,
                                        MTLRenderPassDescriptor* pass_descriptor)
{
    if (!m_data || !m_data->valid || !command_buffer || !pass_descriptor ||
        m_data->viewport_width <= 0 || m_data->viewport_height <= 0)
        return;

    if (m_data->current_encoder) {
        Rml::Log::Message(Rml::Log::LT_WARNING, "Metal BeginFrame called before EndFrame");
        [m_data->current_encoder endEncoding];
        m_data->current_encoder = nil;
    }

    id<MTLTexture> color_texture = pass_descriptor.colorAttachments[0].texture;
    if (!color_texture || m_data->viewport_top >= (int)color_texture.height) {
        Rml::Log::Message(Rml::Log::LT_WARNING, "Metal frame skipped: invalid color attachment or viewport offset");
        return;
    }

    const int safe_width = std::min(m_data->viewport_width, (int)color_texture.width);
    const int safe_height = std::min(m_data->viewport_height,
                                     (int)color_texture.height - m_data->viewport_top);
    if (safe_width <= 0 || safe_height <= 0)
        return;
    if (safe_width != m_data->viewport_width || safe_height != m_data->viewport_height) {
        Rml::Log::Message(Rml::Log::LT_WARNING,
                          "Metal viewport (%d x %d at y=%d) exceeds drawable (%lu x %lu); clamping",
                          m_data->viewport_width, m_data->viewport_height, m_data->viewport_top,
                          (unsigned long)color_texture.width, (unsigned long)color_texture.height);
        SetViewport(safe_width, safe_height);
    }

    m_data->current_command_buffer = command_buffer;
    m_data->current_encoder = [command_buffer renderCommandEncoderWithDescriptor:pass_descriptor];
    if (!m_data->current_encoder) {
        m_data->current_command_buffer = nil;
        Rml::Log::Message(Rml::Log::LT_ERROR, "Metal failed to create a render command encoder");
        return;
    }
    if (++m_data->frame == 0)
        ++m_data->frame;

    MTLViewport vp = {0, (double)m_data->viewport_top,
                      (double)m_data->viewport_width, (double)m_data->viewport_height,
                      0.0, 1.0};
    [m_data->current_encoder setViewport:vp];

    m_data->scissor_enabled = false;
    m_data->scissor_empty = false;
    MTLScissorRect full = {0, (NSUInteger)m_data->viewport_top,
                           (NSUInteger)m_data->viewport_width, (NSUInteger)m_data->viewport_height};
    [m_data->current_encoder setScissorRect:full];

    m_data->has_transform = false;
    m_data->transform = Rml::Matrix4f::Identity();

    m_data->clip = {};
    m_data->active_dss        = m_data->dss_normal;
    m_data->active_stencil_ref = 0;
    m_data->stencil_binding = {};
    m_data->stencil_binding.bind(m_data->current_encoder, m_data->dss_normal, 0);

    m_data->bound_pipeline = nil;
    m_data->bound_fragment_texture_0 = nil;
    m_data->bound_fragment_texture_1 = nil;
    m_data->sampler_bound = false;
}

void RenderInterface_Metal::EndFrame()
{
    if (!m_data || !m_data->current_encoder) return;
    [m_data->current_encoder endEncoding];
    m_data->current_encoder = nil;
    m_data->current_command_buffer = nil;
}

Rml::CompiledGeometryHandle
RenderInterface_Metal::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                        Rml::Span<const int> indices)
{
    if (!m_data || !m_data->valid || vertices.empty() || indices.empty()) return 0;
    if (vertices.size() > std::numeric_limits<size_t>::max() / sizeof(Rml::Vertex) ||
        indices.size() > std::numeric_limits<size_t>::max() / sizeof(int))
        return 0;

	auto* geo = new MetalGeometry();

    size_t v_size = vertices.size() * sizeof(Rml::Vertex);
    size_t i_size = indices.size() * sizeof(int);

    geo->vertex_buffers[0] = [m_data->device newBufferWithBytes:vertices.data()
                                                          length:v_size
                                                         options:MTLResourceStorageModeShared];
    geo->index_buffer  = [m_data->device newBufferWithBytes:indices.data()
                                                      length:i_size
                                                     options:MTLResourceStorageModeShared];
    geo->index_count   = (NSUInteger)indices.size();

    if (!geo->vertex_buffers[0] || !geo->index_buffer) {
        delete geo;
        Rml::Log::Message(Rml::Log::LT_ERROR, "Metal failed to allocate geometry buffers");
        return 0;
    }
    geo->vertex_buffers[0].label = @"RmlUi geometry vertices";
    geo->index_buffer.label = @"RmlUi geometry indices";

    return reinterpret_cast<Rml::CompiledGeometryHandle>(geo);
}

void RenderInterface_Metal::RenderGeometry(Rml::CompiledGeometryHandle handle,
                                            Rml::Vector2f translation,
                                            Rml::TextureHandle texture)
{
    if (!m_data || !m_data->current_encoder || !handle) return;

    if (!m_data->pipeline_color || !m_data->pipeline_texture) {
        NSLog(@"[RmlUi] RenderGeometry skipped: Metal pipelines not initialized. "
               "Check that RmlUi.metal compiled into the app bundle (default.metallib).");
        return;
    }

    auto* geo = reinterpret_cast<MetalGeometry*>(handle);

    id<MTLRenderCommandEncoder> enc = m_data->current_encoder;

    if (m_data->scissor_enabled && m_data->scissor_empty)
        return;
    m_data->stencil_binding.bind(enc, m_data->active_dss, m_data->active_stencil_ref);

    bool has_texture = (texture != 0);
    BindPipeline(enc, m_data->bound_pipeline,
                 has_texture ? m_data->pipeline_texture : m_data->pipeline_color);

    Uniforms u;
    if (m_data->has_transform) {
        simd_float4x4 model = RmlMatrixToSimd(m_data->transform);
        u.transform = simd_mul(m_data->projection, model);
    } else {
        u.transform = m_data->projection;
    }
    u.translation = {translation.x, translation.y};
    u._padding    = {0.0f, 0.0f};

    id<MTLBuffer> vertex_buffer = geo->vertex_buffers[geo->active_slot];
    if (!vertex_buffer || !geo->index_buffer || geo->index_count == 0)
        return;
    [enc setVertexBuffer:vertex_buffer offset:0 atIndex:0];
    [enc setVertexBytes:&u length:sizeof(u) atIndex:1];

    if (has_texture) {
        auto* tex = reinterpret_cast<MetalTexture*>(texture);
        id<MTLTexture> metal_texture = tex->textures[tex->active_slot];
        if (!metal_texture)
            return;
        BindFragmentTexture(enc, m_data->bound_fragment_texture_0, metal_texture, 0);
        if (!m_data->sampler_bound) {
            [enc setFragmentSamplerState:m_data->sampler atIndex:0];
            m_data->sampler_bound = true;
        }
        if (tex->streaming)
            MarkResourceUsed(tex->texture_use[tex->active_slot],
                             tex->last_usage_frame[tex->active_slot],
                             m_data->frame, m_data->current_command_buffer);
    }

    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                    indexCount:geo->index_count
                     indexType:MTLIndexTypeUInt32
                   indexBuffer:geo->index_buffer
             indexBufferOffset:0];

    if (geo->dynamic)
        MarkResourceUsed(geo->vertex_use[geo->active_slot],
                         geo->last_usage_frame[geo->active_slot],
                         m_data->frame, m_data->current_command_buffer);
}

void RenderInterface_Metal::ReleaseGeometry(Rml::CompiledGeometryHandle handle)
{
    if (!handle) return;
    auto* geo = reinterpret_cast<MetalGeometry*>(handle);
    delete geo;
}

void RenderInterface_Metal::UpdateGeometryVertices(Rml::CompiledGeometryHandle handle,
                                                    Rml::Span<const Rml::Vertex> vertices)
{
    if (!m_data || !handle || vertices.empty()) return;
    auto* geo = reinterpret_cast<MetalGeometry*>(handle);
    if (vertices.size() > std::numeric_limits<size_t>::max() / sizeof(Rml::Vertex))
        return;
    size_t byte_size = vertices.size() * sizeof(Rml::Vertex);
    const int slot = (geo->active_slot + 1) % MetalGeometry::kSlots;
    const bool replace_untracked_buffer = geo->vertex_buffers[slot] && !geo->safe_to_overwrite[slot];
    if (!replace_untracked_buffer)
        WaitUntilResourceIdle(geo->vertex_use[slot]);

    if (!geo->vertex_buffers[slot] || replace_untracked_buffer ||
        byte_size > geo->vertex_buffers[slot].length) {
        id<MTLBuffer> replacement = [m_data->device newBufferWithBytes:vertices.data()
                                                                  length:byte_size
                                                                 options:MTLResourceStorageModeShared];
        if (!replacement) {
            Rml::Log::Message(Rml::Log::LT_ERROR, "Metal failed to resize a dynamic vertex buffer");
            return;
        }
        replacement.label = @"RmlUi dynamic geometry vertices";
        [geo->vertex_buffers[slot] release];
        geo->vertex_buffers[slot] = replacement;
    } else {
        std::memcpy(geo->vertex_buffers[slot].contents, vertices.data(), byte_size);
    }

    geo->safe_to_overwrite[slot] = true;
    geo->active_slot = slot;
    geo->dynamic = true;
}

static bool GetRGBAByteSize(int width, int height, size_t& byte_size)
{
    if (width <= 0 || height <= 0)
        return false;
    const size_t w = (size_t)width;
    const size_t h = (size_t)height;
    if (w > std::numeric_limits<size_t>::max() / h ||
        w * h > std::numeric_limits<size_t>::max() / 4)
        return false;
    byte_size = w * h * 4;
    return true;
}

static id<MTLTexture> MakeRGBATexture(id<MTLDevice> device, int width, int height)
{
    if (width <= 0 || height <= 0)
        return nil;
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                     width:(NSUInteger)width
                                    height:(NSUInteger)height
                                 mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    texture.label = @"RmlUi RGBA texture";
    return texture;
}

static void UploadRGBA(id<MTLTexture> texture, const void* bytes, int width, int height)
{
    [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
               mipmapLevel:0
                 withBytes:bytes
               bytesPerRow:(NSUInteger)width * 4];
}

Rml::TextureHandle RenderInterface_Metal::LoadTexture(Rml::Vector2i& texture_dimensions,
                                                       const Rml::String& source)
{
    if (!m_data) return 0;

    if (source.size() > 4 && source.substr(source.size() - 4) == ".svg") {
        std::string full = source;
        if (![[NSFileManager defaultManager] fileExistsAtPath:
              [NSString stringWithUTF8String:full.c_str()]]) {
            NSString* name = [[NSString stringWithUTF8String:source.c_str()] lastPathComponent];
            NSString* bp   = [[NSBundle mainBundle] pathForResource:name ofType:nil];
            if (bp) full = bp.UTF8String;
        }

        auto doc = lunasvg::Document::loadFromFile(full);
        if (!doc) return 0;

        float sw = (float)doc->width();
        float sh = (float)doc->height();
        if (sw <= 0) sw = 24.0f;
        if (sh <= 0) sh = 24.0f;
        float scale = std::min(256.0f / std::max(sw, sh), 4.0f);
        int   w = std::max(1, (int)(sw * scale + 0.5f));
        int   h = std::max(1, (int)(sh * scale + 0.5f));

        auto bitmap = doc->renderToBitmap(w, h);
        if (!bitmap.valid() || !bitmap.data()) return 0;

        // lunasvg outputs BGRA; Metal expects RGBA — swap R and B channels.
        uint8_t* bdata = bitmap.data();
        for (int i = 0; i < w * h * 4; i += 4)
            std::swap(bdata[i], bdata[i + 2]);

        texture_dimensions = {w, h};
        const size_t byte_size = (size_t)w * (size_t)h * 4;
        return GenerateTexture(
            {reinterpret_cast<const Rml::byte*>(bdata), byte_size}, texture_dimensions);
    }

    NSString* path = [NSString stringWithUTF8String:source.c_str()];
#if TARGET_OS_IOS
    UIImage* image = [UIImage imageNamed:path];
    if (!image) {
        image = [UIImage imageWithContentsOfFile:path];
    }
    if (!image) {
        NSString* bundle_path = [[NSBundle mainBundle] pathForResource:path ofType:nil];
        if (bundle_path)
            image = [UIImage imageWithContentsOfFile:bundle_path];
    }
    if (!image) return 0;
    CGImageRef cg_image = image.CGImage;
#else
    NSImage* nsimage = [[[NSImage alloc] initWithContentsOfFile:path] autorelease];
    if (!nsimage)
        nsimage = [NSImage imageNamed:path];
    if (!nsimage) {
        NSString* bundle_path = [[NSBundle mainBundle] pathForResource:path ofType:nil];
        if (bundle_path)
            nsimage = [[[NSImage alloc] initWithContentsOfFile:bundle_path] autorelease];
    }
    if (!nsimage) return 0;
    CGImageRef cg_image = [nsimage CGImageForProposedRect:nil context:nil hints:nil];
#endif
    if (!cg_image) return 0;
    size_t w = CGImageGetWidth(cg_image);
    size_t h = CGImageGetHeight(cg_image);
    if (w == 0 || h == 0 || w > (size_t)std::numeric_limits<int>::max() ||
        h > (size_t)std::numeric_limits<int>::max())
        return 0;
    texture_dimensions = {(int)w, (int)h};

    size_t pixel_bytes = 0;
    if (!GetRGBAByteSize((int)w, (int)h, pixel_bytes))
        return 0;
    std::vector<uint8_t> pixels(pixel_bytes, 0);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    if (!cs) return 0;
    const CGBitmapInfo bitmap_info = static_cast<CGBitmapInfo>(
        static_cast<uint32_t>(kCGBitmapByteOrder32Big) |
        static_cast<uint32_t>(kCGImageAlphaPremultipliedLast));
    CGContextRef ctx = CGBitmapContextCreate(pixels.data(), w, h, 8, w * 4, cs,
                                             bitmap_info);
    if (!ctx) {
        CGColorSpaceRelease(cs);
        return 0;
    }
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), cg_image);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);

    return GenerateTexture({reinterpret_cast<const Rml::byte*>(pixels.data()), pixels.size()},
                           texture_dimensions);
}

Rml::TextureHandle RenderInterface_Metal::GenerateTexture(Rml::Span<const Rml::byte> source,
                                                           Rml::Vector2i source_dimensions)
{
    if (!m_data || !m_data->valid) return 0;

    int w = source_dimensions.x;
    int h = source_dimensions.y;
    size_t required_size = 0;
    if (!GetRGBAByteSize(w, h, required_size) || source.size() < required_size)
        return 0;

    auto* tex = new MetalTexture();
    tex->textures[0] = MakeRGBATexture(m_data->device, w, h);
    if (!tex->textures[0]) {
        delete tex;
        return 0;
    }
    UploadRGBA(tex->textures[0], source.data(), w, h);
    tex->width = w;
    tex->height = h;
    return reinterpret_cast<Rml::TextureHandle>(tex);
}

void RenderInterface_Metal::UpdateTextureData(Rml::TextureHandle texture_handle,
                                               Rml::Span<const Rml::byte> source_data,
                                               Rml::Vector2i source_dimensions)
{
    if (!m_data || !texture_handle)
        return;

    auto* texture = reinterpret_cast<MetalTexture*>(texture_handle);
    const int width = source_dimensions.x;
    const int height = source_dimensions.y;
    size_t required_size = 0;
    if (width != texture->width || height != texture->height ||
        !GetRGBAByteSize(width, height, required_size) || source_data.size() < required_size) {
        Rml::Log::Message(Rml::Log::LT_WARNING,
                          "Metal texture update rejected: dimensions or source size do not match");
        return;
    }

    const int slot = (texture->active_slot + 1) % MetalTexture::kSlots;
    const bool replace_untracked_texture = texture->textures[slot] && !texture->safe_to_overwrite[slot];
    if (!replace_untracked_texture)
        WaitUntilResourceIdle(texture->texture_use[slot]);
    if (!texture->textures[slot] || replace_untracked_texture) {
        id<MTLTexture> replacement = MakeRGBATexture(m_data->device, width, height);
        if (!replacement) {
            Rml::Log::Message(Rml::Log::LT_ERROR, "Metal failed to allocate a streaming RGBA texture");
            return;
        }
        replacement.label = @"RmlUi streaming RGBA texture";
        [texture->textures[slot] release];
        texture->textures[slot] = replacement;
    }

    UploadRGBA(texture->textures[slot], source_data.data(), width, height);
    texture->safe_to_overwrite[slot] = true;
    texture->active_slot = slot;
    texture->streaming = true;
}

void RenderInterface_Metal::ReleaseTexture(Rml::TextureHandle texture_handle)
{
    if (!texture_handle) return;
    auto* tex = reinterpret_cast<MetalTexture*>(texture_handle);
    delete tex;
}

void RenderInterface_Metal::EnableScissorRegion(bool enable)
{
    if (!m_data || !m_data->current_encoder) return;
    m_data->scissor_enabled = enable;

    if (!enable) {
        MTLScissorRect full = {0, (NSUInteger)m_data->viewport_top,
                               (NSUInteger)m_data->viewport_width, (NSUInteger)m_data->viewport_height};
        [m_data->current_encoder setScissorRect:full];
    } else {
        [m_data->current_encoder setScissorRect:m_data->scissor_rect];
    }
}

void RenderInterface_Metal::SetScissorRegion(Rml::Rectanglei region)
{
    if (!m_data) return;

    int vw = m_data->viewport_width;
    int vh = m_data->viewport_height;
    if (vw <= 0 || vh <= 0) {
        m_data->scissor_empty = true;
        return;
    }

    const int x0_unclamped = std::max(region.Left(), 0);
    const int y0_unclamped = std::max(region.Top(), 0);
    const int x1 = std::min(region.Right(), vw);
    const int y1 = std::min(region.Bottom(), vh);
    m_data->scissor_empty = (x1 <= x0_unclamped || y1 <= y0_unclamped);

    // Metal validation requires an in-bounds rectangle. Empty scissors are
    // represented by a safe 1x1 rectangle and draw suppression in the encoder.
    const int x0 = std::clamp(x0_unclamped, 0, vw - 1);
    const int y0 = std::clamp(y0_unclamped, 0, vh - 1);
    const int w = m_data->scissor_empty ? 1 : x1 - x0;
    const int h = m_data->scissor_empty ? 1 : y1 - y0;

    m_data->scissor_rect = {(NSUInteger)x0, (NSUInteger)(m_data->viewport_top + y0),
                            (NSUInteger)w, (NSUInteger)h};

    if (m_data->scissor_enabled && m_data->current_encoder)
        [m_data->current_encoder setScissorRect:m_data->scissor_rect];
}

void RenderInterface_Metal::EnableClipMask(bool enable)
{
    if (!m_data) return;
    if (enable) {
        m_data->active_dss         = m_data->dss_test;
        m_data->active_stencil_ref = m_data->clip.ref;
    } else {
        m_data->active_dss         = m_data->dss_normal;
        m_data->active_stencil_ref = 0;
    }
}

static void EncodeStencilDraw(id<MTLRenderCommandEncoder> enc,
                               MetalGeometry* geo,
                               Rml::Vector2f translation,
                               bool has_transform,
                               const Rml::Matrix4f& transform,
                               int vp_w, int vp_h,
                               id<MTLRenderPipelineState> pipeline,
                               id<MTLDepthStencilState> dss,
                               uint32_t ref,
                               id<MTLRenderPipelineState>& bound_pipeline,
                               StencilBindCache& stencil_binding,
                               uint64_t frame,
                               id<MTLCommandBuffer> command_buffer,
                               bool skip_draw)
{
    if (skip_draw)
        return;
    BindPipeline(enc, bound_pipeline, pipeline);
    stencil_binding.bind(enc, dss, ref);

    Uniforms u;
    simd_float4x4 proj = OrthoMatrix((float)vp_w, (float)vp_h);
    if (has_transform)
        u.transform = simd_mul(proj, RmlMatrixToSimd(transform));
    else
        u.transform = proj;
    u.translation = {translation.x, translation.y};
    u._padding    = {0.0f, 0.0f};

    id<MTLBuffer> vertex_buffer = geo->vertex_buffers[geo->active_slot];
    if (!vertex_buffer || !geo->index_buffer || geo->index_count == 0)
        return;
    [enc setVertexBuffer:vertex_buffer offset:0 atIndex:0];
    [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                    indexCount:geo->index_count
                     indexType:MTLIndexTypeUInt32
                   indexBuffer:geo->index_buffer
             indexBufferOffset:0];

    if (geo->dynamic)
        MarkResourceUsed(geo->vertex_use[geo->active_slot],
                         geo->last_usage_frame[geo->active_slot],
                         frame, command_buffer);
}

static void FillViewportStencil(id<MTLRenderCommandEncoder> enc,
                                 id<MTLRenderPipelineState> pipeline,
                                 id<MTLDepthStencilState> dss,
                                 uint32_t ref, int vp_w, int vp_h,
                                 id<MTLRenderPipelineState>& bound_pipeline,
                                 StencilBindCache& stencil_binding,
                                 bool skip_draw)
{
    if (skip_draw)
        return;
    float w = (float)vp_w, h = (float)vp_h;
    Rml::Vertex verts[4] = {
        {{0,0}, {0,0,0,0}, {0,0}},
        {{w,0}, {0,0,0,0}, {1,0}},
        {{0,h}, {0,0,0,0}, {0,1}},
        {{w,h}, {0,0,0,0}, {1,1}},
    };

    BindPipeline(enc, bound_pipeline, pipeline);
    stencil_binding.bind(enc, dss, ref);

    Uniforms u;
    u.transform   = OrthoMatrix(w, h);
    u.translation = {0.0f, 0.0f};
    u._padding    = {0.0f, 0.0f};

    [enc setVertexBytes:verts length:sizeof(verts) atIndex:0];
    [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}

void RenderInterface_Metal::RenderToClipMask(Rml::ClipMaskOperation operation,
                                              Rml::CompiledGeometryHandle handle,
                                              Rml::Vector2f translation)
{
    if (!m_data || !m_data->current_encoder || !handle) return;
    if (!m_data->pipeline_stencil) return;

    auto* geo = reinterpret_cast<MetalGeometry*>(handle);
    id<MTLRenderCommandEncoder> enc = m_data->current_encoder;
    const int w = m_data->viewport_width, h = m_data->viewport_height;
    const bool skip_draw = m_data->scissor_enabled && m_data->scissor_empty;

    const StencilClipState::Plan plan = m_data->clip.plan(operation);
    switch (plan.pass) {
    case StencilClipState::Pass::WriteGeometry:
        EncodeStencilDraw(enc, geo, translation,
                          m_data->has_transform, m_data->transform, w, h,
                          m_data->pipeline_stencil, m_data->dss_write, plan.geometry_ref,
                          m_data->bound_pipeline, m_data->stencil_binding,
                          m_data->frame, m_data->current_command_buffer, skip_draw);
        break;

    case StencilClipState::Pass::FillViewportThenClearGeometry:
        FillViewportStencil(enc, m_data->pipeline_stencil, m_data->dss_write,
                            plan.fill_ref, w, h,
                            m_data->bound_pipeline, m_data->stencil_binding,
                            skip_draw);
        EncodeStencilDraw(enc, geo, translation,
                          m_data->has_transform, m_data->transform, w, h,
                          m_data->pipeline_stencil, m_data->dss_write, plan.geometry_ref,
                          m_data->bound_pipeline, m_data->stencil_binding,
                          m_data->frame, m_data->current_command_buffer, skip_draw);
        break;

    case StencilClipState::Pass::IncrementWhereEqual:
        EncodeStencilDraw(enc, geo, translation,
                          m_data->has_transform, m_data->transform, w, h,
                          m_data->pipeline_stencil, m_data->dss_write_incr, plan.geometry_ref,
                          m_data->bound_pipeline, m_data->stencil_binding,
                          m_data->frame, m_data->current_command_buffer, skip_draw);
        break;
    }

    m_data->active_stencil_ref = m_data->clip.ref;
}

void RenderInterface_Metal::SetTransform(const Rml::Matrix4f* transform)
{
    if (!m_data) return;
    if (transform) {
        m_data->transform     = *transform;
        m_data->has_transform = true;
    } else {
        m_data->transform     = Rml::Matrix4f::Identity();
        m_data->has_transform = false;
    }
}

Rml::CompiledShaderHandle RenderInterface_Metal::CompileShader(const Rml::String& name,
                                                                const Rml::Dictionary& parameters)
{
    auto* shader = new CompiledShader_Metal();
    GradientUniforms& g = shader->uniforms;

    auto load_stops = [&]() -> bool {
        auto it = parameters.find("color_stop_list");
        if (it == parameters.end()) return false;
        const auto& stop_list = it->second.GetReference<Rml::ColorStopList>();
        g.num_stops = Rml::Math::Min((int)stop_list.size(), MAX_GRADIENT_STOPS);
        for (int i = 0; i < g.num_stops; i++) {
            const Rml::ColourbPremultiplied& c = stop_list[i].color;
            g.stop_colors[i] = {c[0]/255.f, c[1]/255.f, c[2]/255.f, c[3]/255.f};
            g.stop_positions[i] = stop_list[i].position.number;
        }
        return g.num_stops > 0;
    };

    bool stops_loaded = false;

    if (name == "linear-gradient") {
        const bool rep = Rml::Get(parameters, "repeating", false);
        g.func = (int)(rep ? GradientFunc::RepeatingLinear : GradientFunc::Linear);
        Rml::Vector2f p0 = Rml::Get(parameters, "p0", Rml::Vector2f(0.f));
        Rml::Vector2f p1 = Rml::Get(parameters, "p1", Rml::Vector2f(0.f));
        g.p = {p0.x, p0.y};
        g.v = {p1.x - p0.x, p1.y - p0.y};
        stops_loaded = load_stops();
    }
    else if (name == "radial-gradient") {
        const bool rep = Rml::Get(parameters, "repeating", false);
        g.func = (int)(rep ? GradientFunc::RepeatingRadial : GradientFunc::Radial);
        Rml::Vector2f center = Rml::Get(parameters, "center", Rml::Vector2f(0.f));
        Rml::Vector2f radius = Rml::Get(parameters, "radius", Rml::Vector2f(1.f));
        g.p = {center.x, center.y};
        constexpr float min_radius = 1.0e-6f;
        g.v = {1.f / std::max(std::abs(radius.x), min_radius),
               1.f / std::max(std::abs(radius.y), min_radius)};
        stops_loaded = load_stops();
    }
    else if (name == "conic-gradient") {
        const bool rep = Rml::Get(parameters, "repeating", false);
        g.func = (int)(rep ? GradientFunc::RepeatingConic : GradientFunc::Conic);
        Rml::Vector2f center = Rml::Get(parameters, "center", Rml::Vector2f(0.f));
        float angle = Rml::Get(parameters, "angle", 0.f);
        g.p = {center.x, center.y};
        g.v = {Rml::Math::Cos(angle), Rml::Math::Sin(angle)};
        stops_loaded = load_stops();
    }
    else {
        Rml::Log::Message(Rml::Log::LT_WARNING, "RmlUi Metal: unsupported shader '%s'", name.c_str());
        delete shader;
        return {};
    }

    if (!stops_loaded) {
        Rml::Log::Message(Rml::Log::LT_WARNING, "RmlUi Metal: gradient has no color stops");
        delete shader;
        return {};
    }

    return reinterpret_cast<Rml::CompiledShaderHandle>(shader);
}

void RenderInterface_Metal::RenderShader(Rml::CompiledShaderHandle shader_handle,
                                          Rml::CompiledGeometryHandle geometry_handle,
                                          Rml::Vector2f translation,
                                          Rml::TextureHandle)
{
    if (!m_data || !m_data->current_encoder || !shader_handle || !geometry_handle) return;
    if (!m_data->pipeline_gradient) return;

    auto* shader = reinterpret_cast<CompiledShader_Metal*>(shader_handle);
    auto* geo    = reinterpret_cast<MetalGeometry*>(geometry_handle);
    id<MTLRenderCommandEncoder> enc = m_data->current_encoder;
    if (m_data->scissor_enabled && m_data->scissor_empty)
        return;

    m_data->stencil_binding.bind(enc, m_data->active_dss, m_data->active_stencil_ref);
    BindPipeline(enc, m_data->bound_pipeline, m_data->pipeline_gradient);

    Uniforms u;
    u.transform   = m_data->has_transform
        ? simd_mul(m_data->projection, RmlMatrixToSimd(m_data->transform))
        : m_data->projection;
    u.translation = {translation.x, translation.y};
    u._padding    = {0.f, 0.f};

    id<MTLBuffer> vertex_buffer = geo->vertex_buffers[geo->active_slot];
    if (!vertex_buffer || !geo->index_buffer || geo->index_count == 0)
        return;
    [enc setVertexBuffer:vertex_buffer offset:0 atIndex:0];
    [enc setVertexBytes:&u length:sizeof(u) atIndex:1];

    [enc setFragmentBytes:&shader->uniforms length:sizeof(GradientUniforms) atIndex:2];

    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                    indexCount:geo->index_count
                     indexType:MTLIndexTypeUInt32
                   indexBuffer:geo->index_buffer
             indexBufferOffset:0];

    if (geo->dynamic)
        MarkResourceUsed(geo->vertex_use[geo->active_slot],
                         geo->last_usage_frame[geo->active_slot],
                         m_data->frame, m_data->current_command_buffer);
}

void RenderInterface_Metal::ReleaseShader(Rml::CompiledShaderHandle handle)
{
    delete reinterpret_cast<CompiledShader_Metal*>(handle);
}

static id<MTLTexture> MakeTexture(id<MTLDevice> device,
                                   MTLPixelFormat fmt,
                                   uint32_t width, uint32_t height)
{
    if (!device || width == 0 || height == 0)
        return nil;
    MTLTextureDescriptor* td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:fmt
                                     width:width
                                    height:height
                                 mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device newTextureWithDescriptor:td];
    texture.label = (fmt == MTLPixelFormatR8Unorm) ? @"RmlUi NV12 luma" : @"RmlUi NV12 chroma";
    return texture;
}

static void UploadPlane(id<MTLTexture> tex,
                        const uint8_t* data, uint32_t bytes_per_row,
                        uint32_t width, uint32_t height)
{
    [tex replaceRegion:MTLRegionMake2D(0, 0, width, height)
           mipmapLevel:0
             withBytes:data
           bytesPerRow:bytes_per_row];
}

static uint32_t NV12ChromaWidth(uint32_t width) { return (width + 1) / 2; }
static uint32_t NV12ChromaHeight(uint32_t height) { return (height + 1) / 2; }

static bool ValidateNV12Upload(const uint8_t* y_data, uint32_t y_stride,
                               const uint8_t* uv_data, uint32_t uv_stride,
                               uint32_t width, uint32_t height)
{
    if (!y_data || !uv_data || width == 0 || height == 0)
        return false;
    return y_stride >= width && uv_stride >= NV12ChromaWidth(width) * 2;
}

uintptr_t RenderInterface_Metal::GenerateNV12Texture(
    const uint8_t* y_data, uint32_t y_stride,
    const uint8_t* uv_data, uint32_t uv_stride,
    uint32_t width, uint32_t height)
{
    if (!m_data || !m_data->valid ||
        !ValidateNV12Upload(y_data, y_stride, uv_data, uv_stride, width, height))
        return 0;
    auto* t = new MetalNV12Texture();
    t->width  = width;
    t->height = height;
    t->slot   = 0;
    t->y_tex[0]  = MakeTexture(m_data->device, MTLPixelFormatR8Unorm, width, height);
    t->uv_tex[0] = MakeTexture(m_data->device, MTLPixelFormatRG8Unorm,
                               NV12ChromaWidth(width), NV12ChromaHeight(height));
    if (!t->y_tex[0] || !t->uv_tex[0]) {
        delete t;
        Rml::Log::Message(Rml::Log::LT_ERROR, "Metal failed to allocate NV12 textures");
        return 0;
    }
    t->slot_width[0] = width;
    t->slot_height[0] = height;
    UploadPlane(t->y_tex[0],  y_data,  y_stride,  width,     height);
    UploadPlane(t->uv_tex[0], uv_data, uv_stride,
                NV12ChromaWidth(width), NV12ChromaHeight(height));
    return reinterpret_cast<uintptr_t>(t);
}

void RenderInterface_Metal::UpdateNV12Texture(uintptr_t handle,
    const uint8_t* y_data, uint32_t y_stride,
    const uint8_t* uv_data, uint32_t uv_stride,
    uint32_t width, uint32_t height)
{
    if (!m_data || !handle ||
        !ValidateNV12Upload(y_data, y_stride, uv_data, uv_stride, width, height))
        return;
    auto* t = reinterpret_cast<MetalNV12Texture*>(handle);
    const int slot = (t->slot + 1) % MetalNV12Texture::kSlots;
    WaitUntilResourceIdle(t->texture_use[slot]);

    if (!t->y_tex[slot] || !t->uv_tex[slot] ||
        t->slot_width[slot] != width || t->slot_height[slot] != height) {
        id<MTLTexture> new_y = MakeTexture(m_data->device, MTLPixelFormatR8Unorm, width, height);
        id<MTLTexture> new_uv = MakeTexture(m_data->device, MTLPixelFormatRG8Unorm,
                                            NV12ChromaWidth(width), NV12ChromaHeight(height));
        if (!new_y || !new_uv) {
            [new_y release];
            [new_uv release];
            Rml::Log::Message(Rml::Log::LT_ERROR, "Metal failed to resize NV12 textures");
            return;
        }
        [t->y_tex[slot] release];
        [t->uv_tex[slot] release];
        t->y_tex[slot] = new_y;
        t->uv_tex[slot] = new_uv;
        t->slot_width[slot] = width;
        t->slot_height[slot] = height;
    }

    UploadPlane(t->y_tex[slot], y_data, y_stride, width, height);
    UploadPlane(t->uv_tex[slot], uv_data, uv_stride,
                NV12ChromaWidth(width), NV12ChromaHeight(height));
    t->slot = slot;
    t->width = width;
    t->height = height;
}

void RenderInterface_Metal::ReleaseNV12Texture(uintptr_t handle)
{
    if (!handle) return;
    delete reinterpret_cast<MetalNV12Texture*>(handle);
}

void RenderInterface_Metal::RenderNV12Geometry(Rml::CompiledGeometryHandle geometry,
                                                Rml::Vector2f translation,
                                                uintptr_t nv12_handle)
{
    if (!m_data || !m_data->current_encoder || !geometry || !nv12_handle) return;
    if (!m_data->pipeline_nv12) return;

    auto* geo = reinterpret_cast<MetalGeometry*>(geometry);
    auto* t   = reinterpret_cast<MetalNV12Texture*>(nv12_handle);
    id<MTLRenderCommandEncoder> enc = m_data->current_encoder;
    if (m_data->scissor_enabled && m_data->scissor_empty)
        return;

    m_data->stencil_binding.bind(enc, m_data->active_dss, m_data->active_stencil_ref);
    BindPipeline(enc, m_data->bound_pipeline, m_data->pipeline_nv12);

    Uniforms u;
    u.transform   = m_data->has_transform
        ? simd_mul(m_data->projection, RmlMatrixToSimd(m_data->transform))
        : m_data->projection;
    u.translation = {translation.x, translation.y};
    u._padding    = {0.0f, 0.0f};

    id<MTLBuffer> vertex_buffer = geo->vertex_buffers[geo->active_slot];
    if (!vertex_buffer || !geo->index_buffer || geo->index_count == 0 ||
        !t->y_tex[t->slot] || !t->uv_tex[t->slot])
        return;
    [enc setVertexBuffer:vertex_buffer offset:0 atIndex:0];
    [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
    BindFragmentTexture(enc, m_data->bound_fragment_texture_0, t->y_tex[t->slot], 0);
    BindFragmentTexture(enc, m_data->bound_fragment_texture_1, t->uv_tex[t->slot], 1);
    if (!m_data->sampler_bound) {
        [enc setFragmentSamplerState:m_data->sampler atIndex:0];
        m_data->sampler_bound = true;
    }

    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                    indexCount:geo->index_count
                     indexType:MTLIndexTypeUInt32
                   indexBuffer:geo->index_buffer
             indexBufferOffset:0];

    MarkResourceUsed(t->texture_use[t->slot], t->last_usage_frame[t->slot],
                     m_data->frame, m_data->current_command_buffer);
    if (geo->dynamic)
        MarkResourceUsed(geo->vertex_use[geo->active_slot],
                         geo->last_usage_frame[geo->active_slot],
                         m_data->frame, m_data->current_command_buffer);
}
