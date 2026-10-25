#import "AppDelegate.h"
#import "PartiesViewController.h"

#ifdef SENTRY_COCOA_ENABLED
#import <Sentry/Sentry.h>
#import <Sentry/Sentry-Swift.h>

static constexpr double kProductionTracesSampleRate  = 1.0;
static constexpr double kDevelopmentTracesSampleRate = 1.0;
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
        options.tracesSampleRate = @(kProductionTracesSampleRate);
#else
        options.environment = @"development";
        options.tracesSampleRate = @(kDevelopmentTracesSampleRate);
#endif
        options.enableCrashHandler = YES;
        options.enableAppHangTracking = YES;
        options.enableWatchdogTerminationTracking = YES;
        options.sendDefaultPii = YES;
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
