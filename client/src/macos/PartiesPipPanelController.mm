#import "PartiesPipPanelController.h"

#include <client/pip_geometry.h>

#include <utility>
#include <vector>

using parties::client::PipRect;

namespace {

PipRect to_pip(NSRect rect)
{
    return {rect.origin.x, rect.origin.y, rect.size.width, rect.size.height};
}

NSRect to_ns(const PipRect& rect)
{
    return NSMakeRect(rect.x, rect.y, rect.width, rect.height);
}

} // namespace

@implementation PartiesPipPanelController {
    NSPanel* _panel;
    std::function<void(NSRect)> _geometryChanged;
}

- (instancetype)initWithContentView:(NSView*)contentView
{
    self = [super init];
    if (!self)
        return nil;

    // Borderless: no title bar or chrome. Non-activating: clicking the
    // overlay or dragging never steals focus from the app the user is in.
    _panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 384, 216)
                                        styleMask:NSWindowStyleMaskBorderless |
                                                  NSWindowStyleMaskNonactivatingPanel |
                                                  NSWindowStyleMaskResizable
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
    _panel.floatingPanel = YES;
    _panel.level = NSFloatingWindowLevel;
    // Every Space, and over other applications' fullscreen Spaces.
    _panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                NSWindowCollectionBehaviorFullScreenAuxiliary |
                                NSWindowCollectionBehaviorIgnoresCycle;
    _panel.hidesOnDeactivate = NO;
    _panel.becomesKeyOnlyIfNeeded = YES;
    // Hiding the application (Cmd-H) or its main window must not take PiP away.
    _panel.canHide = NO;
    _panel.releasedWhenClosed = NO;
    _panel.movableByWindowBackground = NO;   // the content view drags explicitly
    _panel.acceptsMouseMovedEvents = YES;
    _panel.hasShadow = YES;
    _panel.opaque = YES;
    _panel.backgroundColor = NSColor.blackColor;
    _panel.minSize = NSMakeSize(parties::client::PipSizeLimits{}.min_width, 108);
    _panel.contentAspectRatio = NSMakeSize(16, 9);
    _panel.title = @"Parties picture-in-picture";
    _panel.contentView = contentView;
    _panel.delegate = self;
    return self;
}

- (void)setGeometryChanged:(std::function<void(NSRect)>)geometryChanged
{
    _geometryChanged = std::move(geometryChanged);
}

- (NSPanel*)panel
{
    return _panel;
}

- (void)showWithFrame:(NSRect)frame
{
    [_panel setFrame:frame display:YES];
    // orderFrontRegardless shows the panel without activating the app.
    [_panel orderFrontRegardless];
}

- (void)hide
{
    [_panel orderOut:nil];
}

- (BOOL)isVisible
{
    return _panel.visible;
}

- (void)setAspectRatio:(NSSize)aspect
{
    if (aspect.width <= 0 || aspect.height <= 0)
        return;
    _panel.contentAspectRatio = aspect;
}

- (BOOL)cursorInside
{
    return _panel.visible && NSPointInRect(NSEvent.mouseLocation, _panel.frame);
}

+ (NSRect)placementForRemembered:(const NSRect*)remembered aspect:(double)aspect
{
    NSMutableArray<NSValue*>* frames = [NSMutableArray array];
    for (NSScreen* screen in NSScreen.screens)   // the first screen is the main one
        [frames addObject:[NSValue valueWithRect:screen.visibleFrame]];
    return [self placementForRemembered:remembered aspect:aspect visibleFrames:frames];
}

+ (NSRect)placementForRemembered:(const NSRect*)remembered
                          aspect:(double)aspect
                   visibleFrames:(NSArray<NSValue*>*)frames
{
    std::vector<PipRect> areas;
    for (NSValue* value in frames)
        areas.push_back(to_pip(value.rectValue));
    if (remembered)
        return to_ns(parties::client::pip_clamp_to_work_areas(
            parties::client::pip_fit_aspect(to_pip(*remembered), aspect), areas));
    // AppKit's origin is bottom-left, so "bottom-right" is the low-y corner.
    const PipRect area = areas.empty() ? PipRect{0, 0, 1280, 800} : areas.front();
    PipRect rect = parties::client::pip_default_rect(area, aspect, 384, 24);
    rect.y = area.y + 24;
    return to_ns(rect);
}

// ── NSWindowDelegate ──────────────────────────────────────────────────────────

- (void)windowDidMove:(NSNotification*)notification
{
    if (_geometryChanged && _panel.visible && !_panel.inLiveResize)
        _geometryChanged(_panel.frame);
}

- (void)windowDidEndLiveResize:(NSNotification*)notification
{
    if (_geometryChanged)
        _geometryChanged(_panel.frame);
}

- (void)dealloc
{
    if (_panel.delegate == self)
        _panel.delegate = nil;
    [_panel orderOut:nil];
    [_panel release];
    [super dealloc];
}

@end
