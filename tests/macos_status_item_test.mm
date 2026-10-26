#import "PartiesStatusItemController.h"

#include <cstdio>

namespace {

bool passed = true;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        passed = false;
    }
}

}

int main()
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [NSApp finishLaunching];
        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, 240, 160)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        window.releasedWhenClosed = NO;
        bool savedMode = true;
        bool saveSucceeds = true;
        auto saveMode = [&](bool enabled) {
            if (!saveSucceeds)
                return false;
            savedMode = enabled;
            return true;
        };
        PartiesStatusItemController* controller = [[PartiesStatusItemController alloc]
            initWithWindow:window
                   enabled:savedMode
                  saveMode:saveMode];
        [controller showWindow:nil];
        if ([controller windowShouldClose:window]) {
            [controller release];
            [window close];
            [window release];
            std::fprintf(stderr, "A macOS menu bar is required for this test\n");
            return 77;
        }
        check(!window.visible, "closing in tray mode did not hide the window");
        NSMenuItem* item = [[controller trayModeMenuItem] retain];
        check(item.state == NSControlStateValueOn, "tray mode was not initially checked");
        check(item.target == controller && item.action == @selector(toggleTrayMode:),
            "application menu toggle is not wired");

        [controller toggleTrayMode:nil];
        [controller validateMenuItem:item];
        check(!savedMode && window.visible && item.state == NSControlStateValueOff,
            "disabling tray mode did not save, restore, and update its checkmark");
        check([controller windowShouldClose:window], "disabled mode prevented normal close");
        [item release];
        [controller release];
        check(window.delegate == nil, "tray cleanup left a dangling window delegate");

        controller = [[PartiesStatusItemController alloc]
            initWithWindow:window
                   enabled:savedMode
                  saveMode:saveMode];
        check([controller windowShouldClose:window], "saved disabled mode was ignored");
        [controller toggleTrayMode:nil];
        check(savedMode && ![controller windowShouldClose:window] && !window.visible,
            "tray mode could not be enabled again");
        saveSucceeds = false;
        [controller toggleTrayMode:nil];
        check(![controller windowShouldClose:window] && savedMode,
            "failed save changed the close behavior");
        [controller showWindow:nil];
        check(window.visible, "Show Parties did not restore the window");
        [window miniaturize:nil];
        [controller showWindow:nil];
        check(window.visible && !window.miniaturized, "Show Parties left the window minimized");
        [controller release];
        [window close];
        [window release];
        return passed ? 0 : 1;
    }
}
