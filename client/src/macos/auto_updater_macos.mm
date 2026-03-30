// Sparkle auto-updater wrapper for macOS.
// SPUStandardUpdaterController handles the entire update flow:
// check → download → verify EdDSA signature → install → relaunch.

#import <Foundation/Foundation.h>

#ifdef SPARKLE_ENABLED
#import <Sparkle/Sparkle.h>

static SPUStandardUpdaterController* g_updaterController = nil;
#endif

void macos_updater_init()
{
#ifdef SPARKLE_ENABLED
    g_updaterController = [[SPUStandardUpdaterController alloc]
        initWithStartingUpdater:YES
        updaterDelegate:nil
        userDriverDelegate:nil];
#endif
}

void macos_updater_check_now()
{
#ifdef SPARKLE_ENABLED
    [g_updaterController checkForUpdates:nil];
#endif
}
