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

void macos_updater_check_in_background()
{
#ifdef SPARKLE_ENABLED
    [g_updaterController.updater checkForUpdatesInBackground];
#endif
}
