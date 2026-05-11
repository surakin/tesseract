#import "AppDelegate.h"
#import "MainWindowController.h"

@implementation AppDelegate {
    MainWindowController* _mainWC;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    _mainWC = [[MainWindowController alloc] init];
    [_mainWC.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    // Run after the window is on-screen so the sheet attaches properly.
    dispatch_async(dispatch_get_main_queue(), ^{
        [_mainWC doLogin];
    });
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    return YES;
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    [_mainWC stopSync];
    return NSTerminateNow;
}

@end
