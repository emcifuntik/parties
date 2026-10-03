#import "PartiesStatusItemController.h"

#include <utility>

@implementation PartiesStatusItemController {
    NSWindow* _window;
    NSStatusItem* _statusItem;
    BOOL _enabled;
    std::function<bool(bool)> _saveMode;
}

- (instancetype)initWithWindow:(NSWindow*)window
                       enabled:(BOOL)enabled
                      saveMode:(std::function<bool(bool)>)saveMode
{
    self = [super init];
    if (!self)
        return nil;

    _window = [window retain];
    _window.delegate = self;
    _enabled = enabled;
    _saveMode = std::move(saveMode);
    _statusItem = [[[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength] retain];

    NSImage* image = [NSImage imageWithSystemSymbolName:@"bubble.left.and.bubble.right"
                               accessibilityDescription:@"Parties"];
    [image setTemplate:YES];
    _statusItem.button.image = image;
    if (!image)
        _statusItem.button.title = @"Parties";
    _statusItem.button.toolTip = @"Parties";
    [_statusItem.button setAccessibilityLabel:@"Parties"];

    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Parties"];
    NSMenuItem* showItem = [menu addItemWithTitle:@"Show Parties"
                                           action:@selector(showWindow:)
                                    keyEquivalent:@""];
    showItem.target = self;
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItem:[self trayModeMenuItem]];
    [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* quitItem = [menu addItemWithTitle:@"Quit Parties"
                                           action:@selector(terminate:)
                                    keyEquivalent:@"q"];
    quitItem.target = NSApp;
    quitItem.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    _statusItem.menu = menu;
    [menu release];

    return self;
}

- (NSMenuItem*)trayModeMenuItem
{
    NSMenuItem* item = [[[NSMenuItem alloc]
        initWithTitle:@"Keep Running After Closing Window"
               action:@selector(toggleTrayMode:)
        keyEquivalent:@""] autorelease];
    item.target = self;
    item.state = _enabled ? NSControlStateValueOn : NSControlStateValueOff;
    return item;
}

- (BOOL)validateMenuItem:(NSMenuItem*)menuItem
{
    if (menuItem.action == @selector(toggleTrayMode:))
        menuItem.state = _enabled ? NSControlStateValueOn : NSControlStateValueOff;
    return YES;
}

- (void)showWindow:(id)sender
{
    [NSApp unhide:nil];
    if (_window.miniaturized)
        [_window deminiaturize:nil];
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)toggleTrayMode:(id)sender
{
    if (_saveMode && !_saveMode(!_enabled))
        return;
    _enabled = !_enabled;
    [_statusItem.menu update];
    [NSApp.mainMenu update];
    if (!_enabled && (!_window.visible || NSApp.hidden))
        [self showWindow:nil];
}

- (BOOL)windowShouldClose:(NSWindow*)sender
{
    if (!_enabled || !_statusItem.visible)
        return YES;
    [sender orderOut:nil];
    return NO;
}

- (void)dealloc
{
    if (_window.delegate == self)
        _window.delegate = nil;
    if (_statusItem)
        [[NSStatusBar systemStatusBar] removeStatusItem:_statusItem];
    [_statusItem release];
    [_window release];
    [super dealloc];
}

@end
