#pragma once

#import <AppKit/AppKit.h>

#include <functional>

@interface PartiesPipPanelController : NSObject <NSWindowDelegate>

- (instancetype)initWithContentView:(NSView*)contentView;

- (void)setGeometryChanged:(std::function<void(NSRect)>)geometryChanged;

- (NSPanel*)panel;
- (void)showWithFrame:(NSRect)frame;
- (void)hide;
- (BOOL)isVisible;
- (void)setAspectRatio:(NSSize)aspect;
- (BOOL)cursorInside;

+ (NSRect)placementForRemembered:(const NSRect*)remembered aspect:(double)aspect;
+ (NSRect)placementForRemembered:(const NSRect*)remembered
                          aspect:(double)aspect
                   visibleFrames:(NSArray<NSValue*>*)frames;

@end
