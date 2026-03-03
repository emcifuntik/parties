#import "RmlUi_Platform_iOS.h"

#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#include <RmlUi/Core/Log.h>

SystemInterface_iOS::SystemInterface_iOS() = default;
SystemInterface_iOS::~SystemInterface_iOS() = default;

double SystemInterface_iOS::GetElapsedTime()
{
    return CACurrentMediaTime();
}

bool SystemInterface_iOS::LogMessage(Rml::Log::Type type, const Rml::String& message)
{
    const char* prefix = "";
    switch (type) {
        case Rml::Log::LT_ERROR:   prefix = "[RmlUi][ERROR] "; break;
        case Rml::Log::LT_WARNING: prefix = "[RmlUi][WARN]  "; break;
        case Rml::Log::LT_INFO:    prefix = "[RmlUi][INFO]  "; break;
        case Rml::Log::LT_DEBUG:   prefix = "[RmlUi][DEBUG] "; break;
        default: break;
    }
    NSLog(@"%s%s", prefix, message.c_str());
    return true; // return true to allow default processing to continue
}

void SystemInterface_iOS::SetClipboardText(const Rml::String& text)
{
    [UIPasteboard generalPasteboard].string = [NSString stringWithUTF8String:text.c_str()];
}

void SystemInterface_iOS::GetClipboardText(Rml::String& text)
{
    NSString* str = [UIPasteboard generalPasteboard].string;
    text = str ? str.UTF8String : "";
}

void SystemInterface_iOS::ActivateKeyboard(Rml::Vector2f /*caret_position*/, float /*line_height*/)
{
    // The text input element in the view controller should call becomeFirstResponder.
    // Post a notification so the view controller can act on it.
    [[NSNotificationCenter defaultCenter]
        postNotificationName:@"RmlUiActivateKeyboard"
                      object:nil];
}

void SystemInterface_iOS::DeactivateKeyboard()
{
    [[NSNotificationCenter defaultCenter]
        postNotificationName:@"RmlUiDeactivateKeyboard"
                      object:nil];
}
