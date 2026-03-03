#import "AppDelegate.h"
#import "PartiesViewController.h"

@implementation AppDelegate

- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions
{
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [[PartiesViewController alloc] init];
    [self.window makeKeyAndVisible];
    return YES;
}

@end
