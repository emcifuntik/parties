#import "context_menu_macos.h"

#include <client/app_core.h>

#include <algorithm>
#include <cmath>
#include <utility>

using parties::client::MacOSContextAction;
using parties::client::MacOSUserMenuCallbacks;
using parties::client::UserContextWindowRequest;

static NSString* NSStringFromUTF8(const std::string& value)
{
    NSString* result = [NSString stringWithUTF8String:value.c_str()];
    return result ?: @"";
}

static NSString* RoleName(int role)
{
    switch (role) {
    case 0: return @"Owner";
    case 1: return @"Administrator";
    case 2: return @"Moderator";
    default: return @"Member";
    }
}

static NSTextField* MakeLabel(NSString* text, NSFont* font, NSColor* color)
{
    NSTextField* label = [NSTextField labelWithString:text ?: @""];
    label.font = font;
    label.textColor = color;
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    return label;
}

@interface PartiesMenuActionTarget : NSObject
- (instancetype)initWithCallback:(std::function<void(int)>)callback;
- (void)invoke:(NSMenuItem*)sender;
@end

@implementation PartiesMenuActionTarget {
    std::function<void(int)> _callback;
}

- (instancetype)initWithCallback:(std::function<void(int)>)callback
{
    self = [super init];
    if (self)
        _callback = std::move(callback);
    return self;
}

- (void)invoke:(NSMenuItem*)sender
{
    if (_callback)
        _callback((int)sender.tag);
}

- (void)dealloc
{
    _callback = {};
    [super dealloc];
}

@end

@interface PartiesUserPopoverController : NSViewController
- (instancetype)initWithRequest:(const UserContextWindowRequest&)request
                       callbacks:(MacOSUserMenuCallbacks)callbacks;
- (void)setOwningPopover:(NSPopover*)popover;
- (NSStackView*)sliderRowWithTitle:(NSString*)title
                            slider:(NSSlider*)slider
                        valueLabel:(NSTextField*)value;
- (void)updateVolumeValue;
- (void)updateMusicValue;
- (void)updateCompressionTargetValue;
- (void)volumeChanged:(NSSlider*)sender;
- (void)musicChanged:(NSSlider*)sender;
- (void)compressionChanged:(NSButton*)sender;
- (void)compressionTargetChanged:(NSSlider*)sender;
- (void)roleChanged:(NSPopUpButton*)sender;
- (void)kickUser:(id)sender;
@end

@implementation PartiesUserPopoverController {
    MacOSUserMenuCallbacks _callbacks;
    NSPopover* _owningPopover; // Assign: the popover retains this controller.
    NSSlider* _volumeSlider;
    NSTextField* _volumeValue;
    NSSlider* _musicSlider;
    NSTextField* _musicValue;
    NSButton* _compressionToggle;
    NSSlider* _compressionTargetSlider;
    NSTextField* _compressionTargetValue;
    NSPopUpButton* _rolePopup;
}

- (NSStackView*)sliderRowWithTitle:(NSString*)title
                            slider:(NSSlider*)slider
                         valueLabel:(NSTextField*)value
{
    NSTextField* titleLabel = MakeLabel(title, [NSFont systemFontOfSize:12.0], NSColor.secondaryLabelColor);
    [titleLabel.widthAnchor constraintEqualToConstant:92.0].active = YES;
    [value.widthAnchor constraintEqualToConstant:48.0].active = YES;
    [slider.widthAnchor constraintGreaterThanOrEqualToConstant:120.0].active = YES;

    NSStackView* row = [NSStackView stackViewWithViews:@[titleLabel, slider, value]];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.alignment = NSLayoutAttributeCenterY;
    row.spacing = 8.0;
    return row;
}

- (instancetype)initWithRequest:(const UserContextWindowRequest&)request
                       callbacks:(MacOSUserMenuCallbacks)callbacks
{
    self = [super init];
    if (!self) return nil;

    _callbacks = std::move(callbacks);

    const CGFloat width = 340.0;
    NSView* root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, width, 1.0)];
    root.wantsLayer = YES;

    NSTextField* name = MakeLabel(NSStringFromUTF8(request.name),
                                  [NSFont boldSystemFontOfSize:15.0], NSColor.labelColor);
    NSString* statusText = RoleName(request.role);
    if (!request.channel_name.empty()) {
        statusText = [NSString stringWithFormat:@"%@ · %@", statusText,
                      NSStringFromUTF8(request.channel_name)];
    }
    NSTextField* status = MakeLabel(statusText, [NSFont systemFontOfSize:11.0],
                                    NSColor.secondaryLabelColor);
    NSStackView* identity = [NSStackView stackViewWithViews:@[name, status]];
    identity.orientation = NSUserInterfaceLayoutOrientationVertical;
    identity.alignment = NSLayoutAttributeLeading;
    identity.spacing = 2.0;

    _volumeValue = MakeLabel(@"", [NSFont monospacedDigitSystemFontOfSize:11.0 weight:NSFontWeightRegular],
                                  NSColor.secondaryLabelColor);
    _volumeSlider = [NSSlider sliderWithValue:request.volume minValue:0.0 maxValue:2.0
                                       target:self action:@selector(volumeChanged:)];
    _volumeSlider.continuous = YES;
    [self updateVolumeValue];

    _musicValue = MakeLabel(@"", [NSFont monospacedDigitSystemFontOfSize:11.0 weight:NSFontWeightRegular],
                                 NSColor.secondaryLabelColor);
    _musicSlider = [NSSlider sliderWithValue:request.music_volume minValue:0.0 maxValue:2.0
                                      target:self action:@selector(musicChanged:)];
    _musicSlider.continuous = YES;
    [self updateMusicValue];

    _compressionToggle = [NSButton checkboxWithTitle:@"Normalize this user"
                                               target:self action:@selector(compressionChanged:)];
    _compressionToggle.state = request.compression ? NSControlStateValueOn : NSControlStateValueOff;

    _compressionTargetValue = MakeLabel(@"", [NSFont monospacedDigitSystemFontOfSize:11.0 weight:NSFontWeightRegular],
                                             NSColor.secondaryLabelColor);
    _compressionTargetSlider = [NSSlider sliderWithValue:request.compression_target
                                                minValue:0.1 maxValue:1.0
                                                  target:self action:@selector(compressionTargetChanged:)];
    _compressionTargetSlider.continuous = YES;
    _compressionTargetSlider.enabled = request.compression;
    [self updateCompressionTargetValue];

    NSStackView* content = [NSStackView stackViewWithViews:@[
        identity,
        [self sliderRowWithTitle:@"Voice" slider:_volumeSlider valueLabel:_volumeValue],
        [self sliderRowWithTitle:@"Music" slider:_musicSlider valueLabel:_musicValue],
        _compressionToggle,
        [self sliderRowWithTitle:@"Target" slider:_compressionTargetSlider
                      valueLabel:_compressionTargetValue]
    ]];
    content.orientation = NSUserInterfaceLayoutOrientationVertical;
    content.alignment = NSLayoutAttributeLeading;
    content.spacing = 10.0;
    content.edgeInsets = NSEdgeInsetsMake(16.0, 16.0, 16.0, 16.0);
    content.frame = root.bounds;
    content.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    if (request.can_manage_roles) {
        NSTextField* roleLabel = MakeLabel(@"Role", [NSFont systemFontOfSize:12.0],
                                           NSColor.secondaryLabelColor);
        [roleLabel.widthAnchor constraintEqualToConstant:92.0].active = YES;
        _rolePopup = [[[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO] autorelease];
        [_rolePopup addItemsWithTitles:@[@"Owner", @"Administrator", @"Moderator", @"Member"]];
        [_rolePopup selectItemAtIndex:std::clamp(request.role, 0, 3)];
        _rolePopup.target = self;
        _rolePopup.action = @selector(roleChanged:);
        NSStackView* roleRow = [NSStackView stackViewWithViews:@[roleLabel, _rolePopup]];
        roleRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        roleRow.alignment = NSLayoutAttributeCenterY;
        roleRow.spacing = 8.0;
        [content addArrangedSubview:roleRow];
    }

    if (request.can_kick) {
        NSButton* kick = [NSButton buttonWithTitle:@"Remove from Party"
                                            target:self action:@selector(kickUser:)];
        kick.bezelStyle = NSBezelStyleRounded;
        kick.contentTintColor = NSColor.systemRedColor;
        [content addArrangedSubview:kick];
    }

    // Size the popover to the actual arranged content. A fixed container was
    // substantially taller than the base controls and left a large empty band
    // above the identity block whenever role/kick actions were unavailable.
    const CGFloat height = std::ceil(content.fittingSize.height);
    root.frame = NSMakeRect(0, 0, width, height);
    content.frame = root.bounds;
    [root addSubview:content];
    self.view = root;
    [root release];
    self.preferredContentSize = NSMakeSize(width, height);
    return self;
}

- (void)setOwningPopover:(NSPopover*)popover
{
    _owningPopover = popover;
}

- (void)updateVolumeValue
{
    _volumeValue.stringValue = [NSString stringWithFormat:@"%.2f×", _volumeSlider.doubleValue];
}

- (void)updateMusicValue
{
    _musicValue.stringValue = [NSString stringWithFormat:@"%.2f×", _musicSlider.doubleValue];
}

- (void)updateCompressionTargetValue
{
    _compressionTargetValue.stringValue =
        [NSString stringWithFormat:@"%.2f", _compressionTargetSlider.doubleValue];
}

- (void)volumeChanged:(NSSlider*)sender
{
    [self updateVolumeValue];
    if (_callbacks.set_volume)
        _callbacks.set_volume((float)sender.doubleValue);
}

- (void)musicChanged:(NSSlider*)sender
{
    [self updateMusicValue];
    if (_callbacks.set_music_volume)
        _callbacks.set_music_volume((float)sender.doubleValue);
}

- (void)compressionChanged:(NSButton*)sender
{
    const bool enabled = sender.state == NSControlStateValueOn;
    _compressionTargetSlider.enabled = enabled;
    if (_callbacks.set_compression)
        _callbacks.set_compression(enabled, (float)_compressionTargetSlider.doubleValue);
}

- (void)compressionTargetChanged:(NSSlider*)sender
{
    [self updateCompressionTargetValue];
    if (_callbacks.set_compression)
        _callbacks.set_compression(_compressionToggle.state == NSControlStateValueOn,
                                   (float)sender.doubleValue);
}

- (void)roleChanged:(NSPopUpButton*)sender
{
    if (_callbacks.set_role)
        _callbacks.set_role((int)sender.indexOfSelectedItem);
    [_owningPopover close];
}

- (void)kickUser:(id)sender
{
    if (_callbacks.kick)
        _callbacks.kick();
    [_owningPopover close];
}

- (void)dealloc
{
    _callbacks = {};
    [super dealloc];
}

@end

namespace parties::client {

struct MacOSContextMenuController::State {
    NSView* anchor_view = nil; // Assign: owned by the view controller.
    NSPoint anchor_point = NSZeroPoint;
    NSPopover* popover = nil;
    uint64_t generation = 0;
    bool alive = true;

    explicit State(NSView* view) : anchor_view(view) {}

    void ClosePopover()
    {
        if (popover) {
            [popover close];
            [popover release];
            popover = nil;
        }
    }

    ~State()
    {
        ClosePopover();
    }
};

MacOSContextMenuController::MacOSContextMenuController(NSView* anchor_view)
    : state_(std::make_shared<State>(anchor_view))
{
}

MacOSContextMenuController::~MacOSContextMenuController()
{
    if (state_) {
        state_->alive = false;
        ++state_->generation;
        state_->ClosePopover();
    }
    state_.reset();
}

void MacOSContextMenuController::SetAnchorPoint(NSPoint point)
{
    if (state_)
        state_->anchor_point = point;
}

void MacOSContextMenuController::ShowActions(const std::string& title,
                                             std::vector<MacOSContextAction> actions,
                                             std::function<void(int)> on_action)
{
    if (!state_ || !state_->anchor_view || actions.empty()) return;

    const auto state = state_;
    const uint64_t generation = ++state->generation;
    const NSPoint anchor_point = state->anchor_point;
    const std::string menu_title = title;

    dispatch_async(dispatch_get_main_queue(), ^{
        if (!state->alive || state->generation != generation || !state->anchor_view)
            return;

        state->ClosePopover();
        NSMenu* menu = [[[NSMenu alloc] initWithTitle:NSStringFromUTF8(menu_title)] autorelease];
        menu.autoenablesItems = NO;
        PartiesMenuActionTarget* target =
            [[PartiesMenuActionTarget alloc] initWithCallback:on_action];

        for (const MacOSContextAction& action : actions) {
            if (action.separator) {
                [menu addItem:[NSMenuItem separatorItem]];
                continue;
            }

            NSMenuItem* item = [[[NSMenuItem alloc]
                initWithTitle:NSStringFromUTF8(action.title)
                       action:@selector(invoke:)
                keyEquivalent:@""] autorelease];
            item.target = target;
            item.tag = action.id;
            item.enabled = action.enabled;
            if (!action.detail.empty())
                item.toolTip = NSStringFromUTF8(action.detail);
            if (action.danger) {
                item.attributedTitle = [[[NSAttributedString alloc]
                    initWithString:item.title
                        attributes:@{NSForegroundColorAttributeName: NSColor.systemRedColor}]
                    autorelease];
            }
            [menu addItem:item];
        }

        [menu popUpMenuPositioningItem:nil atLocation:anchor_point inView:state->anchor_view];
        [target release];
    });
}

void MacOSContextMenuController::ShowUser(const UserContextWindowRequest& request,
                                          MacOSUserMenuCallbacks callbacks)
{
    if (!state_ || !state_->anchor_view) return;

    const auto state = state_;
    const uint64_t generation = ++state->generation;
    const NSPoint anchor_point = state->anchor_point;
    const UserContextWindowRequest request_copy = request;

    dispatch_async(dispatch_get_main_queue(), ^{
        if (!state->alive || state->generation != generation || !state->anchor_view)
            return;

        state->ClosePopover();
        PartiesUserPopoverController* controller =
            [[PartiesUserPopoverController alloc] initWithRequest:request_copy
                                                        callbacks:callbacks];
        if (!controller) return;

        NSPopover* popover = [[NSPopover alloc] init];
        popover.behavior = NSPopoverBehaviorTransient;
        popover.animates = YES;
        popover.contentViewController = controller;
        [controller setOwningPopover:popover];
        [controller release];
        state->popover = popover;

        NSRect anchor = NSMakeRect(anchor_point.x, anchor_point.y, 1.0, 1.0);
        [popover showRelativeToRect:anchor
                            ofView:state->anchor_view
                     preferredEdge:NSRectEdgeMaxY];
    });
}

void MacOSContextMenuController::Close()
{
    if (!state_) return;
    ++state_->generation;
    state_->ClosePopover();
}

} // namespace parties::client
