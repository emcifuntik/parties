#import "ScreenShareViewController.h"

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <CoreVideo/CoreVideo.h>
#import <simd/simd.h>

#include <mutex>

// ── Embedded MSL shaders ──────────────────────────────────────────────────────

static NSString* const kVideoShaderSrc = @R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

// NDC rect that letterboxes / pillarboxes the video.
// x0,y0 = top-left corner; x1,y1 = bottom-right corner (all in NDC, -1..1).
struct VideoUniforms {
    float x0, y0, x1, y1;
};

vertex VertexOut vs_video(uint vid [[vertex_id]],
                          constant VideoUniforms& u [[buffer(0)]])
{
    // Two-triangle strip covering the NDC quad.
    float2 pos[4] = {
        {u.x0, u.y1},  // bottom-left
        {u.x1, u.y1},  // bottom-right
        {u.x0, u.y0},  // top-left
        {u.x1, u.y0},  // top-right
    };
    float2 uvs[4] = {
        {0, 0}, {1, 0}, {0, 1}, {1, 1}  // vertical flip only to match server frame orientation
    };
    VertexOut out;
    out.position = float4(pos[vid], 0, 1);
    out.uv       = uvs[vid];
    return out;
}

// NV12 → RGB (BT.601 limited range)
fragment float4 fs_video(VertexOut      in  [[stage_in]],
                         texture2d<float> y_tex  [[texture(0)]],
                         texture2d<float> uv_tex [[texture(1)]])
{
    constexpr sampler s(filter::linear);
    float y  = y_tex.sample(s, in.uv).r;
    float2 uv = uv_tex.sample(s, in.uv).rg - 0.5h;
    float r = y + 1.402  * uv.y;
    float g = y - 0.344  * uv.x - 0.714 * uv.y;
    float b = y + 1.772  * uv.x;
    return float4(r, g, b, 1.0);
}
)MSL";

// ── VideoUniforms struct (must match shader) ──────────────────────────────────

struct VideoUniforms {
    float x0, y0, x1, y1;
};

// ── ScreenShareViewController ─────────────────────────────────────────────────

@implementation ScreenShareViewController {
    MTKView*                _mtkView;
    id<MTLDevice>           _device;
    id<MTLCommandQueue>     _queue;
    id<MTLRenderPipelineState> _pipeline;
    CVMetalTextureCacheRef  _tex_cache;

    // Double-buffer: decoder thread writes _pending, render thread reads _render.
    CVPixelBufferRef        _pending_buf;
    CVPixelBufferRef        _render_buf;
    std::mutex              _buf_mutex;
}

// ── View lifecycle ─────────────────────────────────────────────────────────────

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor blackColor];

    _device = MTLCreateSystemDefaultDevice();
    _queue  = [_device newCommandQueue];

    // Build pipeline.
    NSError* err = nil;
    id<MTLLibrary> lib = [_device newLibraryWithSource:kVideoShaderSrc
                                               options:nil
                                                 error:&err];
    if (!lib) {
        NSLog(@"[ScreenShare] Shader compile error: %@", err);
        return;
    }

    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction   = [lib newFunctionWithName:@"vs_video"];
    desc.fragmentFunction = [lib newFunctionWithName:@"fs_video"];
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

    _pipeline = [_device newRenderPipelineStateWithDescriptor:desc error:&err];
    if (!_pipeline) {
        NSLog(@"[ScreenShare] Pipeline error: %@", err);
        return;
    }

    // Create Metal texture cache for zero-copy CVPixelBuffer access.
    CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, _device, nil, &_tex_cache);

    // MTKView.
    _mtkView = [[MTKView alloc] initWithFrame:self.view.bounds device:_device];
    _mtkView.autoresizingMask = UIViewAutoresizingFlexibleWidth |
                                UIViewAutoresizingFlexibleHeight;
    _mtkView.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    _mtkView.clearColor       = MTLClearColorMake(0, 0, 0, 1);
    _mtkView.delegate         = self;
    _mtkView.paused           = NO;
    _mtkView.enableSetNeedsDisplay = NO;
    [self.view addSubview:_mtkView];

    // Dismiss on tap.
    UITapGestureRecognizer* tap =
        [[UITapGestureRecognizer alloc] initWithTarget:self
                                                action:@selector(_handleTap)];
    [self.view addGestureRecognizer:tap];
}

- (void)viewDidDisappear:(BOOL)animated
{
    [super viewDidDisappear:animated];
    if (self.onDismissed) self.onDismissed();
}

- (void)dealloc
{
    if (_tex_cache)   { CVMetalTextureCacheFlush(_tex_cache, 0); CFRelease(_tex_cache); }
    if (_pending_buf) CFRelease(_pending_buf);
    if (_render_buf)  CFRelease(_render_buf);
}

// ── Orientation ───────────────────────────────────────────────────────────────

- (UIInterfaceOrientationMask)supportedInterfaceOrientations
{
    return UIInterfaceOrientationMaskLandscape;
}

- (UIInterfaceOrientation)preferredInterfaceOrientationForPresentation
{
    return UIInterfaceOrientationLandscapeRight;
}

- (BOOL)prefersStatusBarHidden { return YES; }

// ── Pixel buffer delivery (any thread) ───────────────────────────────────────

- (void)setPixelBuffer:(CVPixelBufferRef)buffer
{
    if (!buffer) return;
    CFRetain(buffer);

    std::lock_guard<std::mutex> lock(_buf_mutex);
    if (_pending_buf) CFRelease(_pending_buf);
    _pending_buf = buffer;
}

// ── Tap to dismiss ────────────────────────────────────────────────────────────

- (void)_handleTap
{
    [self dismissViewControllerAnimated:YES completion:nil];
}

// ── MTKViewDelegate ───────────────────────────────────────────────────────────

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {}

- (void)drawInMTKView:(MTKView*)view
{
    // Swap pending → render buffer.
    {
        std::lock_guard<std::mutex> lock(_buf_mutex);
        if (_pending_buf) {
            if (_render_buf) CFRelease(_render_buf);
            _render_buf  = _pending_buf;
            _pending_buf = nullptr;
        }
    }

    id<CAMetalDrawable> drawable = view.currentDrawable;
    if (!drawable) return;

    MTLRenderPassDescriptor* rpd = view.currentRenderPassDescriptor;
    if (!rpd) return;

    id<MTLCommandBuffer>      cmd = [_queue commandBuffer];
    id<MTLRenderCommandEncoder> enc =
        [cmd renderCommandEncoderWithDescriptor:rpd];

    if (_render_buf && _pipeline && _tex_cache) {
        // Build Y and UV Metal textures from the NV12 pixel buffer.
        size_t vid_w = CVPixelBufferGetWidth(_render_buf);
        size_t vid_h = CVPixelBufferGetHeight(_render_buf);

        CVMetalTextureRef y_ref  = nullptr;
        CVMetalTextureRef uv_ref = nullptr;

        CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, _tex_cache, _render_buf,
            nil, MTLPixelFormatR8Unorm, vid_w, vid_h, 0, &y_ref);

        CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, _tex_cache, _render_buf,
            nil, MTLPixelFormatRG8Unorm, vid_w / 2, vid_h / 2, 1, &uv_ref);

        if (y_ref && uv_ref) {
            id<MTLTexture> y_tex  = CVMetalTextureGetTexture(y_ref);
            id<MTLTexture> uv_tex = CVMetalTextureGetTexture(uv_ref);

            // Compute aspect-fit NDC rect.
            CGSize draw_sz = view.drawableSize;
            float draw_w   = (float)draw_sz.width;
            float draw_h   = (float)draw_sz.height;
            float vid_ar   = (float)vid_w / (float)vid_h;
            float draw_ar  = draw_w / draw_h;

            float ndc_w, ndc_h;
            if (vid_ar > draw_ar) {
                ndc_w = 2.0f;
                ndc_h = 2.0f * draw_ar / vid_ar;
            } else {
                ndc_h = 2.0f;
                ndc_w = 2.0f * vid_ar / draw_ar;
            }

            VideoUniforms uni;
            uni.x0 = -ndc_w * 0.5f;
            uni.y0 = -ndc_h * 0.5f;
            uni.x1 =  ndc_w * 0.5f;
            uni.y1 =  ndc_h * 0.5f;

            [enc setRenderPipelineState:_pipeline];
            [enc setVertexBytes:&uni length:sizeof(uni) atIndex:0];
            [enc setFragmentTexture:y_tex  atIndex:0];
            [enc setFragmentTexture:uv_tex atIndex:1];
            [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip
                    vertexStart:0 vertexCount:4];
        }

        if (y_ref)  CFRelease(y_ref);
        if (uv_ref) CFRelease(uv_ref);
    }

    [enc endEncoding];
    [cmd presentDrawable:drawable];
    [cmd commit];
}

@end
