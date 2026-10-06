#pragma once

#import <AppKit/AppKit.h>

#include <functional>

// The macOS picture-in-picture window: a borderless, non-activating floating
// panel that is visible on every Space and over fullscreen applications, is
// not hidden with the application (Cmd-H) or the menu-bar mode, and keeps the
// stream's aspect ratio while the user resizes it. It hosts any content view;
// the application supplies a Metal view rendering ui/pip.rml.
@interface PartiesPipPanelController : NSObject <NSWindowDelegate>

- (instancetype)initWithContentView:(NSView*)contentView;

// Called after the user finished moving or resizing the panel (screen points).
- (void)setGeometryChanged:(std::function<void(NSRect)>)geometryChanged;

- (NSPanel*)panel;
// Show at `frame` without activating the application or taking key status.
- (void)showWithFrame:(NSRect)frame;
- (void)hide;
- (BOOL)isVisible;
// Stream aspect ratio enforced while resizing.
- (void)setAspectRatio:(NSSize)aspect;
// True while the mouse is over the panel.
- (BOOL)cursorInside;

// Where to open: the remembered frame clamped fully onto a visible screen, or
// the bottom-right of the main screen. Uses NSScreen visible frames.
+ (NSRect)placementForRemembered:(const NSRect*)remembered aspect:(double)aspect;
// Same, against explicit visible frames (primary first); used by tests.
+ (NSRect)placementForRemembered:(const NSRect*)remembered
                          aspect:(double)aspect
                   visibleFrames:(NSArray<NSValue*>*)frames;

@end
