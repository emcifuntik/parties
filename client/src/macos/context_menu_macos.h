#pragma once

#import <AppKit/AppKit.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace parties::client {

struct UserContextWindowRequest;

struct MacOSContextAction {
    int id = 0;
    std::string title;
    std::string detail;
    bool danger = false;
    bool enabled = true;
    bool separator = false;
};

struct MacOSUserMenuCallbacks {
    std::function<void(float)> set_volume;
    std::function<void(float)> set_music_volume;
    std::function<void(bool, float)> set_compression;
    std::function<void(int)> set_role;
    std::function<void()> kick;
};

// Presents native macOS context UI anchored to the last secondary click.
// Simple actions use NSMenu, while the rich per-user controls use NSPopover.
class MacOSContextMenuController {
public:
    explicit MacOSContextMenuController(NSView* anchor_view);
    ~MacOSContextMenuController();

    MacOSContextMenuController(const MacOSContextMenuController&) = delete;
    MacOSContextMenuController& operator=(const MacOSContextMenuController&) = delete;

    void SetAnchorPoint(NSPoint point);
    void ShowActions(const std::string& title,
                     std::vector<MacOSContextAction> actions,
                     std::function<void(int)> on_action);
    void ShowUser(const UserContextWindowRequest& request,
                  MacOSUserMenuCallbacks callbacks);
    void Close();

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace parties::client
