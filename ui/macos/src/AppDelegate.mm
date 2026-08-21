#import "AppDelegate.h"
#import "MainWindowController.h"

#import <Carbon/Carbon.h> // kAEOpenApplication / keyAEPropData / keyAELaunchedAsLogInItem
#import <CoreSpotlight/CoreSpotlight.h>
#import "tk_locale.h"
#include "tesseract/crash_handler.h"
#include "tesseract/paths.h"
#include "tesseract/settings.h"
#include <optional>
#include <vector>
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
#include "tesseract/launch_args.h"
#endif

@implementation AppDelegate
{
    MainWindowController* _windowController;
    // Best-effort "was I launched by the login-item mechanism" signal —
    // set in applicationWillFinishLaunching: (before the event queue could
    // have discarded the Apple Event), read in applicationDidFinishLaunching:
    // to decide whether to skip the initial window show.
    BOOL _launchedAsLoginItem;
}

// Detects the kAEOpenApplication Apple Event's keyAEPropData ==
// keyAELaunchedAsLogInItem parameter — a legacy (Carbon-era) signal, but
// still the mechanism macOS uses to flag a login-item launch regardless of
// whether registration went through the modern SMAppService API or the
// classic Login Items list. Not guaranteed reliable across every macOS
// version/launch path — best-effort by design (see MacAutostart).
- (BOOL)_wasLaunchedAsLoginItem
{
    NSAppleEventDescriptor* event =
        NSAppleEventManager.sharedAppleEventManager.currentAppleEvent;
    if (event.eventClass != kCoreEventClass || event.eventID != kAEOpenApplication)
    {
        return NO;
    }
    NSAppleEventDescriptor* propData =
        [event paramDescriptorForKeyword:keyAEPropData];
    return propData != nil &&
           propData.enumCodeValue == keyAELaunchedAsLogInItem;
}

// Distributed notification a duplicate-launch process posts to ask the
// already-running instance to show its window — see -handleActivateRequest:.
// NSRunningApplication.activateWithOptions: (below) can only bring the other
// process's app to the front; it can't reach into that process to un-hide a
// window that was orderOut:'d to the menu-bar tray, so this fills that gap
// the same way WM_COPYDATA (Windows) / the ActivationListener socket
// (Qt6/GTK4) do for their platforms.
static NSString* const kTesseractActivateRequestNotification =
    @"io.gnomos.Tesseract.ActivateRequest";

- (void)handleActivateRequest:(NSNotification*)note
{
    // Same recipe as a Dock-icon reopen with no visible windows.
    [self applicationShouldHandleReopen:NSApp hasVisibleWindows:NO];
}

- (void)applicationWillFinishLaunching:(NSNotification*)note
{
    _launchedAsLoginItem = [self _wasLaunchedAsLoginItem];

    [NSDistributedNotificationCenter.defaultCenter
        addObserver:self
           selector:@selector(handleActivateRequest:)
               name:kTesseractActivateRequestNotification
             object:nil];

#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    {
        NSArray<NSString*>* args = NSProcessInfo.processInfo.arguments;
        std::vector<std::string> cppArgs;
        for (NSUInteger i = 1; i < args.count; ++i)
            cppArgs.emplace_back([args[i] UTF8String]);
        if (tesseract::parse_launch_args(cppArgs).screenshot_dir)
            return; // Screenshot builds may run beside an installed instance.
    }
#endif

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
            // A duplicate autostart launch has no meaningful action against
            // an already-running instance — terminate quietly without
            // raising it (which would defeat staying hidden).
            if (!_launchedAsLoginItem)
            {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                [other activateWithOptions:NSApplicationActivateIgnoringOtherApps];
#pragma clang diagnostic pop
                // activateWithOptions: only brings the other process's app
                // to the front — it can't un-hide a window that instance
                // has orderOut:'d to the tray. Ask it directly.
                [NSDistributedNotificationCenter.defaultCenter
                    postNotificationName:kTesseractActivateRequestNotification
                                  object:nil
                                userInfo:nil
                      deliverImmediately:YES];
            }
            [NSApp terminate:nil];
            return;
        }
    }
}

- (void)applicationDidFinishLaunching:(NSNotification*)note
{
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    std::optional<std::string> screenshotDir;
    {
        NSArray<NSString*>* args = NSProcessInfo.processInfo.arguments;
        std::vector<std::string> cppArgs;
        for (NSUInteger i = 1; i < args.count; ++i)
            cppArgs.emplace_back([args[i] UTF8String]);
        screenshotDir = tesseract::parse_launch_args(cppArgs).screenshot_dir;
    }
#endif
    // Load persisted settings before set_locale so the saved language
    // preference overrides the OS default when the user has set one.
    tesseract::Settings::instance().load_from_disk(tesseract::config_dir());

    tesseract::install_crash_handler(tesseract::Settings::instance().crash_reporting_enabled);

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
    _windowController.startedHidden = _launchedAsLoginItem;
    if (!_launchedAsLoginItem)
    {
        [_windowController showWindow:self];
        [_windowController.window makeKeyAndOrderFront:self];
        [NSApp activateIgnoringOtherApps:YES];
    }
    // Else: stays hidden until -beginLogin's async restore completes —
    // hidden (tray-only) on a successful silent restore, or force-shown by
    // MainWindowController if there's no saved session to restore.

    [self _installMenuBar];

    // Start the login flow after the window is on screen so the
    // browser-redirect prompt doesn't open behind a still-loading shell.
    dispatch_async(dispatch_get_main_queue(), ^{
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
        if (screenshotDir)
        {
            NSString* dir = [NSString stringWithUTF8String:screenshotDir->c_str()];
            [_windowController captureScreenshotsToDirectory:dir];
            return;
        }
#endif
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

- (void)application:(NSApplication*)app openURLs:(NSArray<NSURL*>*)urls
{
    for (NSURL* url in urls)
        [_windowController openMatrixLink:[url absoluteString]];
}

// Fired when the user activates a room/contact from Spotlight (see
// MacSpotlightSearch, which indexes them as CSSearchableItems).
- (BOOL)application:(NSApplication*)application
    continueUserActivity:(NSUserActivity*)userActivity
      restorationHandler:
          (void (^)(NSArray<id<NSUserActivityRestoring>>*))restorationHandler
{
    (void)application;
    (void)restorationHandler;
    if (![userActivity.activityType isEqualToString:CSSearchableItemActionType])
        return NO;
    NSString* identifier =
        userActivity.userInfo[CSSearchableItemActivityIdentifier];
    if (identifier.length == 0)
        return NO;
    [_windowController activateSpotlightResult:identifier];
    return YES;
}

- (void)applicationWillTerminate:(NSNotification*)note
{
    [_windowController stopSync];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender
{
    return NO;
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender
                     hasVisibleWindows:(BOOL)hasVisibleWindows
{
    [_windowController.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [_windowController navigateToUnread];
    return YES;
}

// First-responder pass-through: when no other responder handles
// Cmd-E (Insert Emoji), route it to the active window controller.
- (void)showEmojiPicker:(id)sender
{
    [_windowController showEmojiPicker:sender];
}

@end
