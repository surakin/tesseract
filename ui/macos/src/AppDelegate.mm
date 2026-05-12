#import "AppDelegate.h"
#import "MainViewController.h"

@implementation AppDelegate {
    MainViewController* _root;
}

- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
    self.window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    _root = [[MainViewController alloc] init];
    self.window.rootViewController = _root;
    [self.window makeKeyAndVisible];

    // Run after the window is on-screen so the modal sheet attaches properly.
    dispatch_async(dispatch_get_main_queue(), ^{
        [_root doLogin];
    });
    return YES;
}

- (void)applicationWillTerminate:(UIApplication*)application {
    [_root stopSync];
}

@end
