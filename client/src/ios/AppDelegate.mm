#import "AppDelegate.h"
#import "PartiesViewController.h"

#ifdef SENTRY_COCOA_ENABLED
#import <Sentry/Sentry.h>
#import <Sentry/Sentry-Swift.h>
#endif

@implementation AppDelegate

- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions
{
#ifdef SENTRY_COCOA_ENABLED
#ifdef SENTRY_DSN_VALUE
    [SentrySDK startWithConfigureOptions:^(SentryOptions *options) {
        options.dsn = @SENTRY_DSN_VALUE;
#ifdef PARTIES_RETAIL
        options.environment = @"production";
#else
        options.environment = @"development";
#endif
        options.enableCrashHandler = YES;
        options.enableAppHangTracking = YES;
        options.enableWatchdogTerminationTracking = YES;
        options.sendDefaultPii = YES;

        // Tracing — lower in production
        options.tracesSampleRate = @1.0;
    }];
#endif
#endif

    UIWindow* window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window = window;
    [window release];

    PartiesViewController* viewController = [[PartiesViewController alloc] init];
    self.window.rootViewController = viewController;
    [viewController release];
    [self.window makeKeyAndVisible];
    return YES;
}

- (void)dealloc
{
    self.window = nil;
    [super dealloc];
}

@end
