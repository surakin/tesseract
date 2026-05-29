#import "AppDelegate.h"
#import "MainWindowController.h"

#import "tk_locale.h"
#include "tesseract/paths.h"
#include "tesseract/settings.h"

@implementation AppDelegate
{
    MainWindowController* _windowController;
}

- (void)applicationWillFinishLaunching:(NSNotification*)note
{
    // Raise the existing instance and abort if we are a duplicate.
    // LSMultipleInstancesProhibited in Info.plist covers Finder/Dock launches;
    // this handles command-line and IDE launches where LaunchServices is bypassed.
    NSString* bundleId = NSBundle.mainBundle.bundleIdentifier;
    NSRunningApplication* myself = NSRunningApplication.currentApplication;
    if (bundleId)
    {
        for (NSRunningApplication* other in [NSRunningApplication
                 runningApplicationsWithBundleIdentifier:bundleId])
        {
            if ([other isEqual:myself])
            {
                continue;
            }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            [other activateWithOptions:NSApplicationActivateIgnoringOtherApps];
#pragma clang diagnostic pop
            [NSApp terminate:nil];
            return;
        }
    }
}

- (void)applicationDidFinishLaunching:(NSNotification*)note
{
    // Load persisted settings before set_locale so the saved language
    // preference overrides the OS default when the user has set one.
    tesseract::Settings::instance().load_from_disk(tesseract::config_dir());

    // i18n: initialise locale before any views are constructed.
    {
        std::string lang = tesseract::Settings::instance().language;
        if (lang == "auto" || lang.empty())
        {
            NSString* os_lang = NSLocale.preferredLanguages.firstObject ?: @"en";
            os_lang = [os_lang stringByReplacingOccurrencesOfString:@"-" withString:@"_"];
            lang = [os_lang UTF8String];
        }
        NSString* resDir = NSBundle.mainBundle.resourcePath;
        std::string i18n_dir = (resDir ? std::string([resDir UTF8String]) : std::string{}) + "/i18n";
        tk::set_locale(i18n_dir, lang);
    }

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

- (void)_installMenuBar
{
    NSMenu* mainMenu = [[NSMenu alloc] initWithTitle:@""];

    // ── Application menu ──────────────────────────────────────────────
    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@"Tesseract"];
    [appMenu addItemWithTitle:TkTr("About Tesseract")
                       action:@selector(orderFrontStandardAboutPanel:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:TkTr("Hide Tesseract")
                       action:@selector(hide:)
                keyEquivalent:@"h"];
    [appMenu addItemWithTitle:TkTr("Quit Tesseract")
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    [mainMenu addItem:appItem];

    // ── Edit menu ─────────────────────────────────────────────────────
    NSMenuItem* editItem = [[NSMenuItem alloc] init];
    NSMenu* editMenu = [[NSMenu alloc] initWithTitle:TkTr("Edit")];
    [editMenu addItemWithTitle:TkTr("Undo")
                        action:@selector(undo:)
                 keyEquivalent:@"z"];
    [editMenu addItemWithTitle:TkTr("Redo")
                        action:@selector(redo:)
                 keyEquivalent:@"Z"];
    [editMenu addItem:[NSMenuItem separatorItem]];
    [editMenu addItemWithTitle:TkTr("Cut")
                        action:@selector(cut:)
                 keyEquivalent:@"x"];
    [editMenu addItemWithTitle:TkTr("Copy")
                        action:@selector(copy:)
                 keyEquivalent:@"c"];
    [editMenu addItemWithTitle:TkTr("Paste")
                        action:@selector(paste:)
                 keyEquivalent:@"v"];
    [editMenu addItemWithTitle:TkTr("Select All")
                        action:@selector(selectAll:)
                 keyEquivalent:@"a"];
    [editMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* emojiItem =
        [editMenu addItemWithTitle:TkTr("Insert Emoji\xe2\x80\xa6")
                            action:@selector(showEmojiPicker:)
                     keyEquivalent:@"e"];
    emojiItem.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    editItem.submenu = editMenu;
    [mainMenu addItem:editItem];

    // ── Window menu ───────────────────────────────────────────────────
    NSMenuItem* winItem = [[NSMenuItem alloc] init];
    NSMenu* winMenu = [[NSMenu alloc] initWithTitle:TkTr("Window")];
    [winMenu addItemWithTitle:TkTr("Minimize")
                       action:@selector(performMiniaturize:)
                keyEquivalent:@"m"];
    [winMenu addItemWithTitle:TkTr("Zoom")
                       action:@selector(performZoom:)
                keyEquivalent:@""];
    winItem.submenu = winMenu;
    [mainMenu addItem:winItem];
    NSApp.windowsMenu = winMenu;

    NSApp.mainMenu = mainMenu;
}

- (void)applicationWillTerminate:(NSNotification*)note
{
    [_windowController stopSync];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender
{
    return NO;
}

// First-responder pass-through: when no other responder handles
// Cmd-E (Insert Emoji), route it to the active window controller.
- (void)showEmojiPicker:(id)sender
{
    [_windowController showEmojiPicker:sender];
}

@end
