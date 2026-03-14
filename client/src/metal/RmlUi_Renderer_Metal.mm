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

// ---- Embedded Metal shader source -----------------------------------------------
// Compiled at runtime via newLibraryWithSource: — avoids Xcode build phase issues.
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
    float4 color    [[attribute(1)]]; // MTLVertexFormatUChar4Normalized → normalized float4
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
    out.color            = in.color; // already normalized to [0,1] by the vertex fetch unit
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

// ---- NV12 video shader -----------------------------------------------------------
// Y plane: texture(0) R8Unorm, full resolution
// UV plane: texture(1) RG8Unorm, half resolution (U=r, V=g)
// BT.709 limited range YCbCr -> RGB
fragment float4 rmlui_fragment_nv12(VertexOut in          [[stage_in]],
                                    texture2d<float> y_tex  [[texture(0)]],
                                    texture2d<float> uv_tex [[texture(1)]],
                                    sampler samp            [[sampler(0)]])
{
    float  y  = y_tex.sample(samp, in.texcoord).r;
    float2 uv = uv_tex.sample(samp, in.texcoord).rg;

    // BT.709 limited range offsets/scales
    float Y  = (y    - 16.0/255.0) * (255.0/219.0);
    float Cb = (uv.r - 128.0/255.0) * (255.0/224.0);
    float Cr = (uv.g - 128.0/255.0) * (255.0/224.0);

    float r = Y + 1.5748 * Cr;
    float g = Y - 0.1873 * Cb - 0.4681 * Cr;
    float b = Y + 1.8556 * Cb;
    return float4(clamp(float3(r, g, b), 0.0, 1.0), 1.0);
}

// ---- Gradient shader -------------------------------------------------------------
#define GRADIENT_MAX_STOPS        16
#define GRADIENT_LINEAR           0
#define GRADIENT_RADIAL           1
#define GRADIENT_CONIC            2
#define GRADIENT_REPEATING_LINEAR 3
#define GRADIENT_REPEATING_RADIAL 4
#define GRADIENT_REPEATING_CONIC  5

struct GradientUniforms {
    float4 stop_colors[GRADIENT_MAX_STOPS];    // 256 bytes  (offset 0)
    float  stop_positions[GRADIENT_MAX_STOPS]; //  64 bytes  (offset 256)
    float2 p;                                  //   8 bytes  (offset 320)
    float2 v;                                  //   8 bytes  (offset 328)
    int    func;                               //   4 bytes  (offset 336)
    int    num_stops;                          //   4 bytes  (offset 340)
    float2 _pad;                               //   8 bytes  (offset 344) → total 352
};

fragment float4 rmlui_fragment_gradient(VertexOut in [[stage_in]],
                                        constant GradientUniforms& g [[buffer(2)]])
{
    float t = 0.0;

    if (g.func == GRADIENT_LINEAR || g.func == GRADIENT_REPEATING_LINEAR) {
        float2 v = g.v;
        float dist_sq = dot(v, v);
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
        t = t0 + fmod(t - t0, t1 - t0);
    }

    float4 color = g.stop_colors[0];
    for (int i = 1; i < g.num_stops; i++)
        color = mix(color, g.stop_colors[i], smoothstep(g.stop_positions[i-1], g.stop_positions[i], t));

    return in.color * color;
}
)MSL";

// ---- Internal data types --------------------------------------------------------

struct Uniforms {
    simd_float4x4 transform;
    simd_float2   translation;
    simd_float2   _padding;
};

struct MetalGeometry {
    id<MTLBuffer> vertex_buffer;
    id<MTLBuffer> index_buffer;
    NSUInteger    index_count;
};

struct MetalTexture {
    id<MTLTexture> texture;
    int width;
    int height;
};

// Triple-buffered NV12 texture pair.
// Each frame we advance to the next slot (slot = (slot+1) % kSlots).
// The slot being written to was last used 2 frames ago, so the GPU
// has had ≥2 frame-times to finish reading it — safe to overwrite
// without explicit CPU/GPU synchronization.
struct MetalNV12Texture {
    static constexpr int kSlots = 3;
    id<MTLTexture> y_tex[kSlots]  = {nil, nil, nil};
    id<MTLTexture> uv_tex[kSlots] = {nil, nil, nil};
    uint32_t width  = 0;
    uint32_t height = 0;
    int slot        = 0;  // most recently written slot; render from here
};

// Gradient uniforms — layout must match GradientUniforms in the MSL shader exactly.
#define MAX_GRADIENT_STOPS 16
struct GradientUniforms {
    simd_float4 stop_colors[MAX_GRADIENT_STOPS];    // offset   0, 256 bytes
    float       stop_positions[MAX_GRADIENT_STOPS]; // offset 256,  64 bytes
    simd_float2 p;                                  // offset 320,   8 bytes
    simd_float2 v;                                  // offset 328,   8 bytes
    int         func;                               // offset 336,   4 bytes
    int         num_stops;                          // offset 340,   4 bytes
    float       _pad[2];                            // offset 344,   8 bytes → total 352
};

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

// ---- Renderer private data -------------------------------------------------------

struct RenderInterface_Metal::Data {
    id<MTLDevice>              device;
    id<MTLCommandQueue>        command_queue;

    id<MTLRenderPipelineState> pipeline_color;    // untextured, color writes enabled
    id<MTLRenderPipelineState> pipeline_texture;  // textured, color writes enabled
    id<MTLRenderPipelineState> pipeline_stencil;  // untextured, color writes disabled (stencil only)
    id<MTLRenderPipelineState> pipeline_gradient; // gradient shader (linear/radial/conic)
    id<MTLRenderPipelineState> pipeline_nv12;     // NV12 video (Y + UV planes)
    id<MTLSamplerState>        sampler;

    // Depth-stencil states
    id<MTLDepthStencilState>   dss_normal;       // no stencil test/write
    id<MTLDepthStencilState>   dss_test;         // equal test, no write
    id<MTLDepthStencilState>   dss_write;        // always pass, replace write
    id<MTLDepthStencilState>   dss_write_incr;   // equal test, increment write (Intersect)

    id<MTLCommandBuffer>          current_command_buffer   = nil;
    id<MTLRenderCommandEncoder>   current_encoder          = nil;

    int viewport_width  = 0;
    int viewport_height = 0;
    int viewport_top    = 0;  // Y offset into framebuffer (safe area / Dynamic Island)

    bool scissor_enabled = false;
    MTLScissorRect scissor_rect = {0, 0, 1, 1};

    Rml::Matrix4f transform;         // current model transform (identity = nullptr)
    bool has_transform = false;

    // Clip mask state (stencil-based, incremented per layer to avoid clearing)
    uint8_t  stencil_ref       = 0;
    id<MTLDepthStencilState> active_dss;         // current DSS for RenderGeometry
    uint32_t active_stencil_ref = 0;
};

// ---- Helper: build orthographic projection ---------------------------------------
// RmlUi origin is top-left, +Y down. Metal NDC origin is center, +Y up.
// We map pixel coords -> NDC by: x' = 2x/W - 1, y' = 1 - 2y/H
static simd_float4x4 OrthoMatrix(float w, float h)
{
    float sx = 2.0f / w;
    float sy = -2.0f / h;
    simd_float4x4 m = matrix_identity_float4x4;
    m.columns[0][0] = sx;
    m.columns[1][1] = sy;
    m.columns[3][0] = -1.0f;
    m.columns[3][1] =  1.0f;
    return m;
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

// ---- Build pipeline states -------------------------------------------------------

static id<MTLRenderPipelineState> BuildPipeline(id<MTLDevice> device,
                                                 id<MTLLibrary> library,
                                                 NSString* frag_name,
                                                 MTLPixelFormat color_format,
                                                 MTLPixelFormat depth_stencil_format,
                                                 BOOL write_color)
{
    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction   = [library newFunctionWithName:@"rmlui_vertex"];
    desc.fragmentFunction = [library newFunctionWithName:frag_name];

    if (!desc.vertexFunction)
        NSLog(@"[RmlUi] Metal: 'rmlui_vertex' not found in library");
    if (!desc.fragmentFunction)
        NSLog(@"[RmlUi] Metal: '%@' not found in library", frag_name);
    if (!desc.vertexFunction || !desc.fragmentFunction)
        return nil;

    // Vertex layout: matches Rml::Vertex (20 bytes per vertex)
    MTLVertexDescriptor* vd = [MTLVertexDescriptor new];
    // attribute 0: float2 position, offset 0
    vd.attributes[0].format      = MTLVertexFormatFloat2;
    vd.attributes[0].offset      = 0;
    vd.attributes[0].bufferIndex = 0;
    // attribute 1: uchar4 color (normalized), offset 8
    vd.attributes[1].format      = MTLVertexFormatUChar4Normalized;
    vd.attributes[1].offset      = 8;
    vd.attributes[1].bufferIndex = 0;
    // attribute 2: float2 texcoord, offset 12
    vd.attributes[2].format      = MTLVertexFormatFloat2;
    vd.attributes[2].offset      = 12;
    vd.attributes[2].bufferIndex = 0;
    // buffer 0: stride 20
    vd.layouts[0].stride         = 20;
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
        // Premultiplied alpha blending
        ca.blendingEnabled             = YES;
        ca.sourceRGBBlendFactor        = MTLBlendFactorOne;
        ca.destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
        ca.sourceAlphaBlendFactor      = MTLBlendFactorOne;
        ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    } else {
        // Stencil-only pipeline: suppress all color output
        ca.writeMask = MTLColorWriteMaskNone;
    }

    NSError* error = nil;
    id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (!pso)
        Rml::Log::Message(Rml::Log::LT_ERROR, "Metal pipeline error: %s",
                          error.localizedDescription.UTF8String);
    return pso;
}

// ---- Build depth-stencil states -------------------------------------------------

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
    return [device newDepthStencilStateWithDescriptor:dsd];
}

// ---- Constructor / Destructor ----------------------------------------------------

RenderInterface_Metal::RenderInterface_Metal(id<MTLDevice> device, MTKView* view)
{
    m_data = new Data();
    m_data->device = device;
    m_data->command_queue = [device newCommandQueue];

    // Compile shaders from embedded source at runtime.
    // This avoids relying on Xcode's "Compile Metal Sources" build phase.
    NSError* error = nil;
    MTLCompileOptions* opts = [MTLCompileOptions new];
    opts.languageVersion = MTLLanguageVersion2_4;
    id<MTLLibrary> library = [device newLibraryWithSource:kRmlUiShaderSource
                                                  options:opts
                                                    error:&error];
    if (!library) {
        NSLog(@"[RmlUi] Metal shader compilation failed: %@",
              error.localizedDescription);
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
    NSLog(@"[RmlUi] Pipelines: color=%s texture=%s stencil=%s gradient=%s nv12=%s",
          m_data->pipeline_color    ? "OK" : "FAILED",
          m_data->pipeline_texture  ? "OK" : "FAILED",
          m_data->pipeline_stencil  ? "OK" : "FAILED",
          m_data->pipeline_gradient ? "OK" : "FAILED",
          m_data->pipeline_nv12     ? "OK" : "FAILED");

    // Depth-stencil states
    m_data->dss_normal     = BuildDSS(device, MTLCompareFunctionAlways, MTLStencilOperationKeep,           0);
    m_data->dss_test       = BuildDSS(device, MTLCompareFunctionEqual,  MTLStencilOperationKeep,           0);
    m_data->dss_write      = BuildDSS(device, MTLCompareFunctionAlways, MTLStencilOperationReplace,     0xff);
    m_data->dss_write_incr = BuildDSS(device, MTLCompareFunctionEqual,  MTLStencilOperationIncrementClamp, 0xff);
    m_data->active_dss     = m_data->dss_normal;

    // Bilinear sampler
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter    = MTLSamplerMinMagFilterLinear;
    sd.magFilter    = MTLSamplerMinMagFilterLinear;
    sd.mipFilter    = MTLSamplerMipFilterNearest;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    m_data->sampler = [device newSamplerStateWithDescriptor:sd];

    m_data->transform = Rml::Matrix4f::Identity();
}

RenderInterface_Metal::~RenderInterface_Metal()
{
    delete m_data;
}

// ---- Frame control ---------------------------------------------------------------

void RenderInterface_Metal::SetViewport(int width, int height, bool /*force*/)
{
    if (!m_data) return;
    m_data->viewport_width  = width;
    m_data->viewport_height = height;
    // Reset scissor to full viewport (in framebuffer coords, offset by viewport_top)
    m_data->scissor_rect = {0, (NSUInteger)m_data->viewport_top,
                            (NSUInteger)width, (NSUInteger)height};
}

void RenderInterface_Metal::SetViewportTopOffset(int top)
{
    if (!m_data) return;
    m_data->viewport_top = top;
}

void RenderInterface_Metal::BeginFrame(id<MTLCommandBuffer> command_buffer,
                                        MTLRenderPassDescriptor* pass_descriptor)
{
    if (!m_data) return;
    m_data->current_command_buffer = command_buffer;
    m_data->current_encoder = [command_buffer renderCommandEncoderWithDescriptor:pass_descriptor];

    // Apply viewport — Y origin is offset by viewport_top to push below the Dynamic Island.
    MTLViewport vp = {0, (double)m_data->viewport_top,
                      (double)m_data->viewport_width, (double)m_data->viewport_height,
                      0.0, 1.0};
    [m_data->current_encoder setViewport:vp];

    // Disable scissor initially — full area the viewport occupies in the framebuffer.
    m_data->scissor_enabled = false;
    MTLScissorRect full = {0, (NSUInteger)m_data->viewport_top,
                           (NSUInteger)m_data->viewport_width, (NSUInteger)m_data->viewport_height};
    [m_data->current_encoder setScissorRect:full];

    m_data->has_transform = false;
    m_data->transform = Rml::Matrix4f::Identity();

    // Reset stencil clip mask state for this frame
    m_data->stencil_ref       = 0;
    m_data->active_dss        = m_data->dss_normal;
    m_data->active_stencil_ref = 0;
    [m_data->current_encoder setDepthStencilState:m_data->dss_normal];
    [m_data->current_encoder setStencilReferenceValue:0];
}

void RenderInterface_Metal::EndFrame()
{
    if (!m_data || !m_data->current_encoder) return;
    [m_data->current_encoder endEncoding];
    m_data->current_encoder = nil;
    m_data->current_command_buffer = nil;
}

// ---- Geometry --------------------------------------------------------------------

Rml::CompiledGeometryHandle
RenderInterface_Metal::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                        Rml::Span<const int> indices)
{
    if (!m_data) return 0;

    auto* geo = new MetalGeometry();

    size_t v_size = vertices.size() * sizeof(Rml::Vertex);
    size_t i_size = indices.size() * sizeof(int);

    geo->vertex_buffer = [m_data->device newBufferWithBytes:vertices.data()
                                                      length:v_size
                                                     options:MTLResourceStorageModeShared];
    geo->index_buffer  = [m_data->device newBufferWithBytes:indices.data()
                                                      length:i_size
                                                     options:MTLResourceStorageModeShared];
    geo->index_count   = (NSUInteger)indices.size();

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

    // Apply current clip mask stencil state
    [enc setDepthStencilState:m_data->active_dss];
    [enc setStencilReferenceValue:m_data->active_stencil_ref];

    // Choose pipeline
    bool has_texture = (texture != 0);
    [enc setRenderPipelineState:has_texture ? m_data->pipeline_texture : m_data->pipeline_color];

    // Build uniforms
    Uniforms u;
    simd_float4x4 proj = OrthoMatrix((float)m_data->viewport_width, (float)m_data->viewport_height);
    if (m_data->has_transform) {
        simd_float4x4 model = RmlMatrixToSimd(m_data->transform);
        u.transform = simd_mul(proj, model);
    } else {
        u.transform = proj;
    }
    u.translation = {translation.x, translation.y};
    u._padding    = {0.0f, 0.0f};

    [enc setVertexBuffer:geo->vertex_buffer offset:0 atIndex:0];
    [enc setVertexBytes:&u length:sizeof(u) atIndex:1];

    if (has_texture) {
        auto* tex = reinterpret_cast<MetalTexture*>(texture);
        [enc setFragmentTexture:tex->texture atIndex:0];
        [enc setFragmentSamplerState:m_data->sampler atIndex:0];
    }

    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                    indexCount:geo->index_count
                     indexType:MTLIndexTypeUInt32
                   indexBuffer:geo->index_buffer
             indexBufferOffset:0];
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
    if (!handle) return;
    auto* geo = reinterpret_cast<MetalGeometry*>(handle);
    size_t byte_size = vertices.size() * sizeof(Rml::Vertex);
    if (byte_size <= geo->vertex_buffer.length)
        memcpy(geo->vertex_buffer.contents, vertices.data(), byte_size);
}

// ---- Textures --------------------------------------------------------------------

Rml::TextureHandle RenderInterface_Metal::LoadTexture(Rml::Vector2i& texture_dimensions,
                                                       const Rml::String& source)
{
    if (!m_data) return 0;

    // ── SVG: rasterize with lunasvg ─────────────────────────────────────────
    if (source.size() > 4 && source.substr(source.size() - 4) == ".svg") {
        // Resolve full path: RmlUi usually provides an absolute path already.
        std::string full = source;
        if (![[NSFileManager defaultManager] fileExistsAtPath:
              [NSString stringWithUTF8String:full.c_str()]]) {
            NSString* name = [[NSString stringWithUTF8String:source.c_str()] lastPathComponent];
            NSString* bp   = [[NSBundle mainBundle] pathForResource:name ofType:nil];
            if (bp) full = bp.UTF8String;
        }

        auto doc = lunasvg::Document::loadFromFile(full);
        if (!doc) return 0;

        // Render at 4× the SVG's logical pixel size, capped at 256px per side.
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
        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                         width:w height:h mipmapped:NO];
        id<MTLTexture> mtl_tex = [m_data->device newTextureWithDescriptor:td];
        [mtl_tex replaceRegion:MTLRegionMake2D(0,0,w,h)
                   mipmapLevel:0 withBytes:bdata bytesPerRow:w*4];
        auto* tex = new MetalTexture{mtl_tex, w, h};
        return reinterpret_cast<Rml::TextureHandle>(tex);
    }

    // ── Raster image (PNG/JPG) via platform image class ──────────────────────
    NSString* path = [NSString stringWithUTF8String:source.c_str()];
#if TARGET_OS_IOS
    UIImage* image = [UIImage imageNamed:path];
    if (!image) {
        image = [UIImage imageWithContentsOfFile:
                 [[NSBundle mainBundle] pathForResource:path ofType:nil]];
    }
    if (!image) return 0;
    CGImageRef cg_image = image.CGImage;
#else
    NSImage* nsimage = [NSImage imageNamed:path];
    if (!nsimage) {
        nsimage = [[NSImage alloc] initWithContentsOfFile:
                   [[NSBundle mainBundle] pathForResource:path ofType:nil]];
    }
    if (!nsimage) return 0;
    CGImageRef cg_image = [nsimage CGImageForProposedRect:nil context:nil hints:nil];
#endif
    size_t w = CGImageGetWidth(cg_image);
    size_t h = CGImageGetHeight(cg_image);
    texture_dimensions = {(int)w, (int)h};

    // Render CGImage into an RGBA bitmap
    std::vector<uint8_t> pixels(w * h * 4, 0);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(pixels.data(), w, h, 8, w * 4, cs,
                                             kCGImageAlphaPremultipliedLast);
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), cg_image);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);

    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                   width:w
                                                                                  height:h
                                                                               mipmapped:NO];
    id<MTLTexture> mtl_tex = [m_data->device newTextureWithDescriptor:td];
    [mtl_tex replaceRegion:MTLRegionMake2D(0, 0, w, h)
               mipmapLevel:0
                 withBytes:pixels.data()
               bytesPerRow:w * 4];

    auto* tex = new MetalTexture{mtl_tex, (int)w, (int)h};
    return reinterpret_cast<Rml::TextureHandle>(tex);
}

Rml::TextureHandle RenderInterface_Metal::GenerateTexture(Rml::Span<const Rml::byte> source,
                                                           Rml::Vector2i source_dimensions)
{
    if (!m_data) return 0;

    int w = source_dimensions.x;
    int h = source_dimensions.y;

    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                   width:w
                                                                                  height:h
                                                                               mipmapped:NO];
    id<MTLTexture> mtl_tex = [m_data->device newTextureWithDescriptor:td];
    [mtl_tex replaceRegion:MTLRegionMake2D(0, 0, w, h)
               mipmapLevel:0
                 withBytes:source.data()
               bytesPerRow:w * 4];

    auto* tex = new MetalTexture{mtl_tex, w, h};
    return reinterpret_cast<Rml::TextureHandle>(tex);
}

void RenderInterface_Metal::ReleaseTexture(Rml::TextureHandle texture_handle)
{
    if (!texture_handle) return;
    auto* tex = reinterpret_cast<MetalTexture*>(texture_handle);
    delete tex;
}

// ---- Scissor / Clip --------------------------------------------------------------

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

    // Intersect region with [0, 0, vw, vh]
    int x0 = Rml::Math::Max(region.Left(),   0);
    int y0 = Rml::Math::Max(region.Top(),    0);
    int x1 = Rml::Math::Min(region.Right(),  vw);
    int y1 = Rml::Math::Min(region.Bottom(), vh);

    // Clamp origin to be strictly inside the viewport so x0+w and y0+h can never
    // exceed the render-pass dimensions (Metal validation requirement).
    x0 = Rml::Math::Min(x0, vw - 1);
    y0 = Rml::Math::Min(y0, vh - 1);

    // Width/height: at least 1px, at most reaching the far edge.
    int w = Rml::Math::Max(x1 - x0, 1);
    int h = Rml::Math::Max(y1 - y0, 1);
    w = Rml::Math::Min(w, vw - x0);
    h = Rml::Math::Min(h, vh - y0);

    // Context Y=0 maps to framebuffer Y=viewport_top, so offset the scissor rect Y.
    m_data->scissor_rect = {(NSUInteger)x0, (NSUInteger)(m_data->viewport_top + y0),
                            (NSUInteger)w, (NSUInteger)h};

    if (m_data->scissor_enabled && m_data->current_encoder)
        [m_data->current_encoder setScissorRect:m_data->scissor_rect];
}

// ---- Clip mask (stencil-based) ---------------------------------------------------
//
// Strategy: increment stencil_ref instead of clearing each frame.
//   Set:       stencil_ref++; write stencil_ref where geometry (replace, test=always)
//   Intersect: write stencil_ref where prev-layer pixels AND geometry (incr, test=equal(prev))
//   SetInverse: stencil_ref++; fill viewport with stencil_ref, then punch out geometry with 0
//
// After each RenderToClipMask, active_stencil_ref is updated so RenderGeometry
// tests == stencil_ref (passes only inside the clip region).

void RenderInterface_Metal::EnableClipMask(bool enable)
{
    if (!m_data) return;
    if (enable) {
        m_data->active_dss         = m_data->dss_test;
        m_data->active_stencil_ref = (uint32_t)m_data->stencil_ref;
    } else {
        m_data->active_dss         = m_data->dss_normal;
        m_data->active_stencil_ref = 0;
    }
}

/// Draw geometry to stencil without color output.
static void EncodeStencilDraw(id<MTLRenderCommandEncoder> enc,
                               MetalGeometry* geo,
                               Rml::Vector2f translation,
                               bool has_transform,
                               const Rml::Matrix4f& transform,
                               int vp_w, int vp_h,
                               id<MTLRenderPipelineState> pipeline,
                               id<MTLDepthStencilState> dss,
                               uint32_t ref)
{
    [enc setRenderPipelineState:pipeline];
    [enc setDepthStencilState:dss];
    [enc setStencilReferenceValue:ref];

    Uniforms u;
    simd_float4x4 proj = OrthoMatrix((float)vp_w, (float)vp_h);
    if (has_transform)
        u.transform = simd_mul(proj, RmlMatrixToSimd(transform));
    else
        u.transform = proj;
    u.translation = {translation.x, translation.y};
    u._padding    = {0.0f, 0.0f};

    [enc setVertexBuffer:geo->vertex_buffer offset:0 atIndex:0];
    [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                    indexCount:geo->index_count
                     indexType:MTLIndexTypeUInt32
                   indexBuffer:geo->index_buffer
             indexBufferOffset:0];
}

/// Fill the entire viewport with a given stencil reference value (for SetInverse).
static void FillViewportStencil(id<MTLDevice> device,
                                 id<MTLRenderCommandEncoder> enc,
                                 id<MTLRenderPipelineState> pipeline,
                                 id<MTLDepthStencilState> dss,
                                 uint32_t ref, int vp_w, int vp_h)
{
    float w = (float)vp_w, h = (float)vp_h;
    Rml::Vertex verts[4] = {
        {{0,0}, {0,0,0,0}, {0,0}},
        {{w,0}, {0,0,0,0}, {1,0}},
        {{w,h}, {0,0,0,0}, {1,1}},
        {{0,h}, {0,0,0,0}, {0,1}},
    };
    int idx[6] = {0,1,2, 0,2,3};

    id<MTLBuffer> vbuf = [device newBufferWithBytes:verts length:sizeof(verts) options:MTLResourceStorageModeShared];
    id<MTLBuffer> ibuf = [device newBufferWithBytes:idx   length:sizeof(idx)   options:MTLResourceStorageModeShared];

    [enc setRenderPipelineState:pipeline];
    [enc setDepthStencilState:dss];
    [enc setStencilReferenceValue:ref];

    Uniforms u;
    u.transform   = OrthoMatrix(w, h);
    u.translation = {0.0f, 0.0f};
    u._padding    = {0.0f, 0.0f};

    [enc setVertexBuffer:vbuf offset:0 atIndex:0];
    [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                    indexCount:6
                     indexType:MTLIndexTypeUInt32
                   indexBuffer:ibuf
             indexBufferOffset:0];
}

void RenderInterface_Metal::RenderToClipMask(Rml::ClipMaskOperation operation,
                                              Rml::CompiledGeometryHandle handle,
                                              Rml::Vector2f translation)
{
    using Rml::ClipMaskOperation;
    if (!m_data || !m_data->current_encoder || !handle) return;
    if (!m_data->pipeline_stencil) return;

    auto* geo = reinterpret_cast<MetalGeometry*>(handle);
    id<MTLRenderCommandEncoder> enc = m_data->current_encoder;
    const int w = m_data->viewport_width, h = m_data->viewport_height;

    switch (operation) {
    case ClipMaskOperation::Set:
        m_data->stencil_ref++;
        EncodeStencilDraw(enc, geo, translation,
                          m_data->has_transform, m_data->transform, w, h,
                          m_data->pipeline_stencil, m_data->dss_write,
                          (uint32_t)m_data->stencil_ref);
        break;

    case ClipMaskOperation::SetInverse:
        m_data->stencil_ref++;
        // Fill viewport with stencil_ref, then punch geometry out with 0
        FillViewportStencil(m_data->device, enc,
                            m_data->pipeline_stencil, m_data->dss_write,
                            (uint32_t)m_data->stencil_ref, w, h);
        EncodeStencilDraw(enc, geo, translation,
                          m_data->has_transform, m_data->transform, w, h,
                          m_data->pipeline_stencil, m_data->dss_write, 0);
        break;

    case ClipMaskOperation::Intersect:
        // Test equal to current ref, increment to ref+1 where geometry overlaps
        EncodeStencilDraw(enc, geo, translation,
                          m_data->has_transform, m_data->transform, w, h,
                          m_data->pipeline_stencil, m_data->dss_write_incr,
                          (uint32_t)m_data->stencil_ref);
        m_data->stencil_ref++;
        break;
    }

    // Update the active test reference so RenderGeometry clips to the new layer
    m_data->active_stencil_ref = (uint32_t)m_data->stencil_ref;
}

// ---- Transform -------------------------------------------------------------------

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

// ---- Shader (gradient) -----------------------------------------------------------

Rml::CompiledShaderHandle RenderInterface_Metal::CompileShader(const Rml::String& name,
                                                                const Rml::Dictionary& parameters)
{
    auto* shader = new CompiledShader_Metal();
    GradientUniforms& g = shader->uniforms;

    // Parse the color stop list (common to all gradient types)
    auto load_stops = [&]() {
        auto it = parameters.find("color_stop_list");
        if (it == parameters.end()) return;
        const auto& stop_list = it->second.GetReference<Rml::ColorStopList>();
        g.num_stops = Rml::Math::Min((int)stop_list.size(), MAX_GRADIENT_STOPS);
        for (int i = 0; i < g.num_stops; i++) {
            const Rml::ColourbPremultiplied& c = stop_list[i].color;
            g.stop_colors[i] = {c[0]/255.f, c[1]/255.f, c[2]/255.f, c[3]/255.f};
            g.stop_positions[i] = stop_list[i].position.number;
        }
    };

    if (name == "linear-gradient") {
        const bool rep = Rml::Get(parameters, "repeating", false);
        g.func = (int)(rep ? GradientFunc::RepeatingLinear : GradientFunc::Linear);
        Rml::Vector2f p0 = Rml::Get(parameters, "p0", Rml::Vector2f(0.f));
        Rml::Vector2f p1 = Rml::Get(parameters, "p1", Rml::Vector2f(0.f));
        g.p = {p0.x, p0.y};
        g.v = {p1.x - p0.x, p1.y - p0.y};
        load_stops();
    }
    else if (name == "radial-gradient") {
        const bool rep = Rml::Get(parameters, "repeating", false);
        g.func = (int)(rep ? GradientFunc::RepeatingRadial : GradientFunc::Radial);
        Rml::Vector2f center = Rml::Get(parameters, "center", Rml::Vector2f(0.f));
        Rml::Vector2f radius = Rml::Get(parameters, "radius", Rml::Vector2f(1.f));
        g.p = {center.x, center.y};
        g.v = {1.f / radius.x, 1.f / radius.y};  // inverse radius, same as GL3
        load_stops();
    }
    else if (name == "conic-gradient") {
        const bool rep = Rml::Get(parameters, "repeating", false);
        g.func = (int)(rep ? GradientFunc::RepeatingConic : GradientFunc::Conic);
        Rml::Vector2f center = Rml::Get(parameters, "center", Rml::Vector2f(0.f));
        float angle = Rml::Get(parameters, "angle", 0.f);
        g.p = {center.x, center.y};
        g.v = {Rml::Math::Cos(angle), Rml::Math::Sin(angle)};
        load_stops();
    }
    else {
        Rml::Log::Message(Rml::Log::LT_WARNING, "RmlUi Metal: unsupported shader '%s'", name.c_str());
        delete shader;
        return {};
    }

    return reinterpret_cast<Rml::CompiledShaderHandle>(shader);
}

void RenderInterface_Metal::RenderShader(Rml::CompiledShaderHandle shader_handle,
                                          Rml::CompiledGeometryHandle geometry_handle,
                                          Rml::Vector2f translation,
                                          Rml::TextureHandle /*texture*/)
{
    if (!m_data || !m_data->current_encoder || !shader_handle || !geometry_handle) return;
    if (!m_data->pipeline_gradient) return;

    auto* shader = reinterpret_cast<CompiledShader_Metal*>(shader_handle);
    auto* geo    = reinterpret_cast<MetalGeometry*>(geometry_handle);
    id<MTLRenderCommandEncoder> enc = m_data->current_encoder;

    [enc setDepthStencilState:m_data->active_dss];
    [enc setStencilReferenceValue:m_data->active_stencil_ref];
    [enc setRenderPipelineState:m_data->pipeline_gradient];

    Uniforms u;
    simd_float4x4 proj = OrthoMatrix((float)m_data->viewport_width, (float)m_data->viewport_height);
    u.transform   = m_data->has_transform ? simd_mul(proj, RmlMatrixToSimd(m_data->transform)) : proj;
    u.translation = {translation.x, translation.y};
    u._padding    = {0.f, 0.f};

    [enc setVertexBuffer:geo->vertex_buffer offset:0 atIndex:0];
    [enc setVertexBytes:&u length:sizeof(u) atIndex:1];

    // Gradient uniforms in fragment buffer(2)
    [enc setFragmentBytes:&shader->uniforms length:sizeof(GradientUniforms) atIndex:2];

    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                    indexCount:geo->index_count
                     indexType:MTLIndexTypeUInt32
                   indexBuffer:geo->index_buffer
             indexBufferOffset:0];
}

void RenderInterface_Metal::ReleaseShader(Rml::CompiledShaderHandle handle)
{
    delete reinterpret_cast<CompiledShader_Metal*>(handle);
}

// ---- NV12 video texture ----------------------------------------------------------

static id<MTLTexture> MakeTexture(id<MTLDevice> device,
                                   MTLPixelFormat fmt,
                                   uint32_t width, uint32_t height)
{
    MTLTextureDescriptor* td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:fmt
                                     width:width
                                    height:height
                                 mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    return [device newTextureWithDescriptor:td];
}

// stride parameters are bytes-per-row (as returned by CVPixelBufferGetBytesPerRowOfPlane)
static void UploadPlane(id<MTLTexture> tex,
                        const uint8_t* data, uint32_t bytes_per_row,
                        uint32_t width, uint32_t height)
{
    [tex replaceRegion:MTLRegionMake2D(0, 0, width, height)
           mipmapLevel:0
             withBytes:data
           bytesPerRow:bytes_per_row];
}

uintptr_t RenderInterface_Metal::GenerateNV12Texture(
    const uint8_t* y_data, uint32_t y_stride,
    const uint8_t* uv_data, uint32_t uv_stride,
    uint32_t width, uint32_t height)
{
    if (!m_data) return 0;
    auto* t = new MetalNV12Texture();
    t->width  = width;
    t->height = height;
    t->slot   = 0;
    for (int i = 0; i < MetalNV12Texture::kSlots; i++) {
        t->y_tex[i]  = MakeTexture(m_data->device, MTLPixelFormatR8Unorm,  width,     height);
        t->uv_tex[i] = MakeTexture(m_data->device, MTLPixelFormatRG8Unorm, width / 2, height / 2);
    }
    // Seed slot 0 with the initial frame data; other slots will be filled in naturally.
    UploadPlane(t->y_tex[0],  y_data,  y_stride,  width,     height);
    UploadPlane(t->uv_tex[0], uv_data, uv_stride, width / 2, height / 2);
    return reinterpret_cast<uintptr_t>(t);
}

void RenderInterface_Metal::UpdateNV12Texture(uintptr_t handle,
    const uint8_t* y_data, uint32_t y_stride,
    const uint8_t* uv_data, uint32_t uv_stride,
    uint32_t width, uint32_t height)
{
    if (!handle) return;
    auto* t = reinterpret_cast<MetalNV12Texture*>(handle);
    // Advance to the next slot — it was last rendered 2+ frames ago, safe to overwrite.
    t->slot = (t->slot + 1) % MetalNV12Texture::kSlots;
    UploadPlane(t->y_tex[t->slot],  y_data,  y_stride,  width,     height);
    UploadPlane(t->uv_tex[t->slot], uv_data, uv_stride, width / 2, height / 2);
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

    [enc setDepthStencilState:m_data->active_dss];
    [enc setStencilReferenceValue:m_data->active_stencil_ref];
    [enc setRenderPipelineState:m_data->pipeline_nv12];

    Uniforms u;
    simd_float4x4 proj = OrthoMatrix((float)m_data->viewport_width, (float)m_data->viewport_height);
    u.transform   = m_data->has_transform ? simd_mul(proj, RmlMatrixToSimd(m_data->transform)) : proj;
    u.translation = {translation.x, translation.y};
    u._padding    = {0.0f, 0.0f};

    [enc setVertexBuffer:geo->vertex_buffer offset:0 atIndex:0];
    [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
    [enc setFragmentTexture:t->y_tex[t->slot]  atIndex:0];
    [enc setFragmentTexture:t->uv_tex[t->slot] atIndex:1];
    [enc setFragmentSamplerState:m_data->sampler atIndex:0];

    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                    indexCount:geo->index_count
                     indexType:MTLIndexTypeUInt32
                   indexBuffer:geo->index_buffer
             indexBufferOffset:0];
}
