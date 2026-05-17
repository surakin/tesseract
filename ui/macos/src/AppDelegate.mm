#import "AppDelegate.h"
#import "MainWindowController.h"

@implementation AppDelegate {
    MainWindowController* _windowController;
}

- (void)applicationWillFinishLaunching:(NSNotification*)note {
    // Raise the existing instance and abort if we are a duplicate.
    // LSMultipleInstancesProhibited in Info.plist covers Finder/Dock launches;
    // this handles command-line and IDE launches where LaunchServices is bypassed.
    NSString* bundleId = NSBundle.mainBundle.bundleIdentifier;
    NSRunningApplication* myself = NSRunningApplication.currentApplication;
    if (bundleId) {
        for (NSRunningApplication* other in
                [NSRunningApplication runningApplicationsWithBundleIdentifier:bundleId]) {
            if ([other isEqual:myself]) continue;
            [other activateWithOptions:NSApplicationActivateIgnoringOtherApps];
            [NSApp terminate:nil];
            return;
        }
    }
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
    _windowController = [[MainWindowController alloc] init];
    [_windowController showWindow:self];
    [_windowController.window makeKeyAndOrderFront:self];
    [NSApp activateIgnoringOtherApps:YES];

    [self _installMenuBar];

    // Start the login flow after the window is on screen so the
    // browser-redirect prompt doesn't open behind a still-loading shell.
    dispatch_async(dispatch_get_main_queue(), ^{
        [_windowController beginLogin];
    });
}

- (void)_installMenuBar {
    NSMenu* mainMenu = [[NSMenu alloc] initWithTitle:@""];

    // ── Application menu ──────────────────────────────────────────────
    NSMenuItem* appItem  = [[NSMenuItem alloc] init];
    NSMenu*     appMenu  = [[NSMenu alloc] initWithTitle:@"Tesseract"];
    [appMenu addItemWithTitle:@"About Tesseract"
                        action:@selector(orderFrontStandardAboutPanel:)
                 keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Hide Tesseract"
                        action:@selector(hide:)
                 keyEquivalent:@"h"];
    [appMenu addItemWithTitle:@"Quit Tesseract"
                        action:@selector(terminate:)
                 keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    [mainMenu addItem:appItem];

    // ── Edit menu ─────────────────────────────────────────────────────
    NSMenuItem* editItem = [[NSMenuItem alloc] init];
    NSMenu*     editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
    [editMenu addItemWithTitle:@"Undo"   action:@selector(undo:)   keyEquivalent:@"z"];
    [editMenu addItemWithTitle:@"Redo"   action:@selector(redo:)   keyEquivalent:@"Z"];
    [editMenu addItem:[NSMenuItem separatorItem]];
    [editMenu addItemWithTitle:@"Cut"    action:@selector(cut:)    keyEquivalent:@"x"];
    [editMenu addItemWithTitle:@"Copy"   action:@selector(copy:)   keyEquivalent:@"c"];
    [editMenu addItemWithTitle:@"Paste"  action:@selector(paste:)  keyEquivalent:@"v"];
    [editMenu addItemWithTitle:@"Select All"
                         action:@selector(selectAll:)
                  keyEquivalent:@"a"];
    [editMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* emojiItem = [editMenu addItemWithTitle:@"Insert Emoji…"
                                                 action:@selector(showEmojiPicker:)
                                          keyEquivalent:@"e"];
    emojiItem.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    editItem.submenu = editMenu;
    [mainMenu addItem:editItem];

    // ── Window menu ───────────────────────────────────────────────────
    NSMenuItem* winItem = [[NSMenuItem alloc] init];
    NSMenu*     winMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    [winMenu addItemWithTitle:@"Minimize"
                         action:@selector(performMiniaturize:)
                  keyEquivalent:@"m"];
    [winMenu addItemWithTitle:@"Zoom"
                         action:@selector(performZoom:)
                  keyEquivalent:@""];
    winItem.submenu = winMenu;
    [mainMenu addItem:winItem];
    NSApp.windowsMenu = winMenu;

    NSApp.mainMenu = mainMenu;
}

- (void)applicationWillTerminate:(NSNotification*)note {
    [_windowController stopSync];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    return YES;
}

// First-responder pass-through: when no other responder handles
// Cmd-E (Insert Emoji), route it to the active window controller.
- (void)showEmojiPicker:(id)sender {
    [_windowController showEmojiPicker:sender];
}

@end
