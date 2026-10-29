// macOS picture-in-picture panel lifecycle: a borderless, non-activating
// floating panel on every Space and over fullscreen apps, which stays up while
// the main window is miniaturized, ordered out (menu-bar mode) or the app is
// hidden, keeps the stream aspect ratio, reports its geometry, and reopens on
// a visible screen.

#import "PartiesPipPanelController.h"

#include <cstdio>

namespace {

bool passed = true;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "PiP panel: %s\n", message);
        passed = false;
    }
}

bool inside(NSRect outer, NSRect inner)
{
    return NSContainsRect(outer, inner);
}

void spin(NSTimeInterval seconds)
{
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
}

}

int main()
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [NSApp finishLaunching];
        if (NSScreen.screens.count == 0) {
            std::fprintf(stderr, "A window server with a screen is required for this test\n");
            return 77;
        }

        NSWindow* mainWindow = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(100, 100, 640, 400)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        mainWindow.releasedWhenClosed = NO;
        [mainWindow makeKeyAndOrderFront:nil];

        NSView* content = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 384, 216)];
        PartiesPipPanelController* controller = [[PartiesPipPanelController alloc] initWithContentView:content];
        int geometryReports = 0;
        NSRect reported = NSZeroRect;
        [controller setGeometryChanged:[&](NSRect frame) {
            ++geometryReports;
            reported = frame;
        }];
        NSPanel* panel = [[controller panel] retain];

        // Window kind.
        check(!panel.visible && ![controller isVisible], "panel was created visible");
        check((panel.styleMask & NSWindowStyleMaskNonactivatingPanel) != 0 &&
              (panel.styleMask & NSWindowStyleMaskTitled) == 0 &&
              (panel.styleMask & NSWindowStyleMaskResizable) != 0,
            "panel is not a borderless, resizable, non-activating panel");
        check(panel.level >= NSFloatingWindowLevel && panel.floatingPanel, "panel does not float");
        check((panel.collectionBehavior & NSWindowCollectionBehaviorCanJoinAllSpaces) != 0 &&
              (panel.collectionBehavior & NSWindowCollectionBehaviorFullScreenAuxiliary) != 0,
            "panel is not on every Space or over fullscreen apps");
        check(!panel.hidesOnDeactivate && !panel.canHide, "panel hides with the application");
        check(panel.contentView == content, "content view was not installed");

        // First open: bottom-right of the main screen's visible frame.
        const NSRect visible = NSScreen.screens.firstObject.visibleFrame;
        const NSRect first = [PartiesPipPanelController placementForRemembered:nullptr aspect:16.0 / 9.0];
        check(inside(visible, first) && NSMaxX(first) > NSMidX(visible) && NSMinY(first) < NSMidY(visible),
            "first placement is not bottom-right on the main screen");

        // Show without activating or taking key status.
        [controller showWithFrame:first];
        spin(0.1);
        check(panel.visible && [controller isVisible], "show did not order the panel in");
        check(!panel.keyWindow && NSApp.keyWindow != panel, "showing the panel took key status");

        // Stays up while the main window is miniaturized, ordered out to the
        // menu bar, or the application is hidden.
        [mainWindow miniaturize:nil];
        spin(0.6);
        check(panel.visible, "panel hid with the miniaturized main window");
        [mainWindow deminiaturize:nil];
        [mainWindow orderOut:nil];
        spin(0.1);
        check(panel.visible, "panel hid with the main window in menu-bar mode");
        [NSApp hide:nil];
        spin(0.3);
        check(panel.visible, "panel hid with the application");
        [NSApp unhide:nil];

        // Aspect ratio follows the stream; the end of a resize reports the frame.
        [controller setAspectRatio:NSMakeSize(4, 3)];
        check(NSEqualSizes(panel.contentAspectRatio, NSMakeSize(4, 3)), "aspect ratio was not applied");
        [controller setAspectRatio:NSMakeSize(0, 3)];
        check(NSEqualSizes(panel.contentAspectRatio, NSMakeSize(4, 3)), "an invalid aspect ratio was applied");
        [controller windowDidEndLiveResize:[NSNotification notificationWithName:NSWindowDidEndLiveResizeNotification
                                                                         object:panel]];
        check(geometryReports >= 1 && NSEqualRects(reported, panel.frame), "resize end did not report the frame");

        // A remembered frame on a screen that is gone reopens fully visible.
        NSArray<NSValue*>* screens = @[[NSValue valueWithRect:NSMakeRect(0, 0, 1440, 875)]];
        const NSRect stranded = NSMakeRect(-4000, 3000, 480, 270);
        const NSRect clamped = [PartiesPipPanelController placementForRemembered:&stranded
                                                                          aspect:16.0 / 9.0
                                                                   visibleFrames:screens];
        check(inside(NSMakeRect(0, 0, 1440, 875), clamped) && clamped.size.width == 480,
            "remembered frame was not clamped onto a visible screen");
        const NSRect kept = NSMakeRect(200, 120, 400, 225);
        check(NSEqualRects([PartiesPipPanelController placementForRemembered:&kept
                                                                       aspect:16.0 / 9.0
                                                                visibleFrames:screens], kept),
            "visible remembered frame was moved");

        // Hide and reopen reuse the same panel.
        [controller hide];
        check(!panel.visible && ![controller cursorInside], "hide left the panel visible");
        [controller showWithFrame:kept];
        spin(0.1);
        check(panel.visible && [controller panel] == panel, "reopen did not reuse the panel");

        // Teardown leaves no dangling delegate.
        [controller release];
        check(panel.delegate == nil && !panel.visible, "teardown left the panel up or its delegate dangling");
        [panel release];
        [content release];
        [mainWindow close];
        [mainWindow release];
    }
    return passed ? 0 : 1;
}
