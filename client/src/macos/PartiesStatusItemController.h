#pragma once

#import <AppKit/AppKit.h>

#include <functional>

@interface PartiesStatusItemController : NSObject <NSWindowDelegate, NSMenuItemValidation>

- (instancetype)initWithWindow:(NSWindow*)window
                       enabled:(BOOL)enabled
                      saveMode:(std::function<bool(bool)>)saveMode;
- (NSMenuItem*)trayModeMenuItem;
- (void)showWindow:(id)sender;
- (void)toggleTrayMode:(id)sender;

@end
