#import "pip_host_macos.h"
#import "PartiesPipPanelController.h"

#import "../metal/RmlUi_Renderer_Metal.h"

#include <client/pip_window_model.h>
#include <client/video_element.h>

#include <RmlUi/Core.h>

#include <algorithm>
#include <cmath>
#include <memory>

using parties::client::PipRect;
using parties::client::PipWindowModel;
using parties::client::VideoElement;

namespace {

constexpr const char* kContextName = "picture-in-picture";

// The keys the overlay's volume control uses (see PipWindowModel).
Rml::Input::KeyIdentifier pip_key(unsigned short key_code)
{
    switch (key_code) {
    case 0x7B: return Rml::Input::KI_LEFT;
    case 0x7C: return Rml::Input::KI_RIGHT;
    case 0x7D: return Rml::Input::KI_DOWN;
    case 0x7E: return Rml::Input::KI_UP;
    case 0x74: return Rml::Input::KI_PRIOR;
    case 0x79: return Rml::Input::KI_NEXT;
    case 0x73: return Rml::Input::KI_HOME;
    case 0x77: return Rml::Input::KI_END;
    default:   return Rml::Input::KI_UNKNOWN;
    }
}

} // namespace

// The PiP surface. Overlay actions receive clicks; everywhere else the mouse
// drags the panel. The panel is non-activating, so the first click counts.
// Clicking a keyboard control (the volume slider) makes the panel key without
// activating the application, so it receives the arrow keys.
@interface PartiesPipView : MTKView
@property (nonatomic, assign) Rml::Context* rmlContext;
@property (nonatomic, assign) PipWindowModel* pipModel;
// True between a press on an overlay action and its release.
@property (nonatomic, readonly) BOOL pointerActive;
@end

@implementation PartiesPipView

- (BOOL)acceptsFirstMouse:(NSEvent*)event { return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }
- (BOOL)acceptsFirstResponder { return YES; }

- (Rml::Vector2f)rmlPoint:(NSEvent*)event
{
    const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    const float scale = (float)self.window.backingScaleFactor;
    return {(float)point.x * scale, (float)(self.bounds.size.height - point.y) * scale};
}

- (void)mouseMoved:(NSEvent*)event
{
    if (!_rmlContext) return;
    const auto point = [self rmlPoint:event];
    _rmlContext->ProcessMouseMove((int)point.x, (int)point.y, 0);
}

- (void)mouseDown:(NSEvent*)event
{
    if (!_rmlContext) return;
    const auto point = [self rmlPoint:event];
    _rmlContext->ProcessMouseMove((int)point.x, (int)point.y, 0);
    const bool overlay = _pipModel && _pipModel->overlay_visible.get();
    Rml::Element* target = overlay ? _rmlContext->GetElementAtPoint(point) : nullptr;
    if (PipWindowModel::is_action(target)) {
        if (PipWindowModel::takes_keyboard(target)) {
            [self.window makeKeyWindow];
            [self.window makeFirstResponder:self];
        }
        _pointerActive = YES;
        _rmlContext->ProcessMouseButtonDown(0, 0);
        return;
    }
    // Everything that is not an action moves the panel.
    [self.window performWindowDragWithEvent:event];
}

- (void)mouseDragged:(NSEvent*)event
{
    // AppKit keeps sending drags to this view outside the panel, so a slider
    // drag follows the cursor anywhere.
    if (!_rmlContext || !_pointerActive) return;
    const auto point = [self rmlPoint:event];
    _rmlContext->ProcessMouseMove((int)point.x, (int)point.y, 0);
}

- (void)mouseUp:(NSEvent*)event
{
    _pointerActive = NO;
    if (_rmlContext) _rmlContext->ProcessMouseButtonUp(0, 0);
}

- (void)keyDown:(NSEvent*)event
{
    const auto key = pip_key(event.keyCode);
    if (_rmlContext && key != Rml::Input::KI_UNKNOWN) _rmlContext->ProcessKeyDown(key, 0);
}

- (void)keyUp:(NSEvent*)event
{
    const auto key = pip_key(event.keyCode);
    if (_rmlContext && key != Rml::Input::KI_UNKNOWN) _rmlContext->ProcessKeyUp(key, 0);
}

@end

@implementation PartiesPipHost {
    id<MTLDevice> _device;
    id<MTLCommandQueue> _commandQueue;
    MacPipActions _actions;
    PartiesPipView* _view;
    PartiesPipPanelController* _panel;
    std::unique_ptr<RenderInterface_Metal> _renderer;
    std::unique_ptr<PipWindowModel> _model;
    Rml::Context* _context;
    Rml::ElementDocument* _document;
    VideoElement* _video;
    double _fittedAspect;
    uint64_t _saveGeneration;
    bool _failed;
}

- (instancetype)initWithDevice:(id<MTLDevice>)device actions:(MacPipActions)actions
{
    self = [super init];
    if (!self)
        return nil;
    _device = [device retain];
    _actions = std::move(actions);
    return self;
}

- (BOOL)ensureSurface
{
    if (_context) return YES;
    if (_failed || !_device) return NO;

    _view = [[PartiesPipView alloc] initWithFrame:NSMakeRect(0, 0, 384, 216) device:_device];
    _view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    _view.depthStencilPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    _view.clearColor = MTLClearColorMake(0, 0, 0, 1);
    _view.preferredFramesPerSecond = 60;
    _view.enableSetNeedsDisplay = NO;
    _view.paused = YES;
    _view.delegate = self;
    NSTrackingArea* tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect
               owner:_view
            userInfo:nil];
    [_view addTrackingArea:tracking];
    [tracking release];

    _commandQueue = [_device newCommandQueue];
    _renderer = std::make_unique<RenderInterface_Metal>(_device, _view);
    if (!_commandQueue || !*_renderer) {
        NSLog(@"[Parties] Picture-in-picture renderer creation failed");
        _renderer.reset();   // no context used it yet
        _failed = true;
        return NO;
    }

    _panel = [[PartiesPipPanelController alloc] initWithContentView:_view];
    PartiesPipHost* host = self;
    [_panel setGeometryChanged:[host](NSRect frame) {
        // A drag can report several moves; persist only the last one.
        const uint64_t generation = ++host->_saveGeneration;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.4 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            if (generation == host->_saveGeneration && host->_actions.save_geometry)
                host->_actions.save_geometry({frame.origin.x, frame.origin.y,
                                              frame.size.width, frame.size.height});
        });
    }];

    const CGSize drawable = _view.drawableSize;
    const int width = std::max(1, (int)drawable.width);
    const int height = std::max(1, (int)drawable.height);
    _renderer->SetViewport(width, height);
    _context = Rml::CreateContext(kContextName, {width, height}, _renderer.get());
    if (!_context) {
        NSLog(@"[Parties] Picture-in-picture RmlUi context creation failed");
        _failed = true;
        return NO;
    }
    VideoElement::RegisterContextRenderInterface(_context, _renderer.get());
    _context->SetDensityIndependentPixelRatio((float)NSScreen.mainScreen.backingScaleFactor);
    _view.rmlContext = _context;

    _model = std::make_unique<PipWindowModel>();
    _model->on_return = [host] { if (host->_actions.return_to_main) host->_actions.return_to_main(); };
    _model->on_volume_changed = [host](float volume) {
        if (host->_actions.set_volume) host->_actions.set_volume(volume);
    };
    _model->on_close = [host] { if (host->_actions.close) host->_actions.close(); };
    if (!_model->init(_context) || !(_document = _context->LoadDocument("ui/pip.rml"))) {
        NSLog(@"[Parties] Picture-in-picture document failed to load");
        _failed = true;
        [self prepareShutdown];
        return NO;
    }
    _document->SetClass("platform-macos", true);
    _document->SetClass("platform-desktop", true);
    _document->Show();
    _view.pipModel = _model.get();
    _video = rmlui_dynamic_cast<VideoElement*>(_document->GetElementById("pip-video"));
    return YES;
}

- (BOOL)showWithTitle:(const std::string&)title
{
    if (![self ensureSurface]) return NO;
    _model->title = title;
    _panel.panel.title = [NSString stringWithUTF8String:title.c_str()] ?: @"Parties picture-in-picture";
    if (_panel.isVisible) return YES;   // switching streams keeps the panel where it is

    const double aspect = _fittedAspect > 0 ? _fittedAspect : parties::client::kPipDefaultAspect;
    NSRect frame;
    const auto remembered = _actions.load_geometry ? _actions.load_geometry() : std::nullopt;
    if (remembered) {
        const NSRect saved = NSMakeRect(remembered->x, remembered->y, remembered->width, remembered->height);
        frame = [PartiesPipPanelController placementForRemembered:&saved aspect:aspect];
    } else {
        frame = [PartiesPipPanelController placementForRemembered:nullptr aspect:aspect];
    }
    _view.paused = NO;
    [_panel showWithFrame:frame];
    return YES;
}

- (void)hide
{
    if (!_panel.isVisible) return;
    const NSRect frame = _panel.panel.frame;
    if (_actions.save_geometry)
        _actions.save_geometry({frame.origin.x, frame.origin.y, frame.size.width, frame.size.height});
    [_panel hide];
    _view.paused = YES;
    if (_model) _model->overlay_visible = false;
}

- (BOOL)isVisible
{
    return _panel && _panel.isVisible;
}

- (VideoElement*)videoSurface
{
    return _video;
}

- (ExtendedRenderInterface*)renderer
{
    return _renderer.get();
}

- (void)fitPanelToVideo
{
    if (!_video || _video->frame_width() == 0 || _video->frame_height() == 0) return;
    const double aspect = (double)_video->frame_width() / _video->frame_height();
    if (std::fabs(aspect - _fittedAspect) < 0.005) return;
    _fittedAspect = aspect;
    [_panel setAspectRatio:NSMakeSize(_video->frame_width(), _video->frame_height())];
    const NSRect current = _panel.panel.frame;
    [_panel.panel setFrame:[PartiesPipPanelController placementForRemembered:&current aspect:aspect]
                   display:YES];
}

// ── MTKViewDelegate ───────────────────────────────────────────────────────────

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size
{
    if (!_renderer || !_context) return;
    _renderer->SetViewport((int)size.width, (int)size.height);
    _context->SetDimensions({(int)size.width, (int)size.height});
    const float scale = (float)view.window.backingScaleFactor;
    if (scale >= 1.0f) _context->SetDensityIndependentPixelRatio(scale);
}

- (void)drawInMTKView:(MTKView*)view
{
    if (!_renderer || !_context || !_panel.isVisible) return;
    // Stay visible while a control is in use: a volume drag that left the
    // panel, or keyboard adjustment while the panel is key.
    _model->overlay_visible = [_panel cursorInside] || _view.pointerActive || _panel.panel.keyWindow;
    if (_actions.volume) _model->volume = _actions.volume();
    [self fitPanelToVideo];

    MTLRenderPassDescriptor* pass = view.currentRenderPassDescriptor;
    if (!pass) return;
    id<MTLCommandBuffer> buffer = [_commandQueue commandBuffer];
    _renderer->BeginFrame(buffer, pass);
    _context->Update();
    _context->Render();
    _renderer->EndFrame();
    [buffer presentDrawable:view.currentDrawable];
    [buffer commit];
}

// ── Teardown ──────────────────────────────────────────────────────────────────

- (void)prepareShutdown
{
    [_panel hide];
    _view.paused = YES;
    _view.rmlContext = nullptr;
    _view.pipModel = nullptr;
    if (_context) {
        _context->UnloadAllDocuments();
        Rml::RemoveContext(kContextName);
        VideoElement::UnregisterContextRenderInterface(_context);
        _context = nullptr;
    }
    _document = nullptr;
    _video = nullptr;
    _model.reset();
}

- (void)shutdown
{
    // Rml::Shutdown has completed: no render manager refers to the renderer.
    _view.delegate = nil;
    _renderer.reset();
    [_panel release];
    _panel = nil;
    [_view release];
    _view = nil;
    [_commandQueue release];
    _commandQueue = nil;
}

- (void)dealloc
{
    [self shutdown];
    [_device release];
    [super dealloc];
}

@end
