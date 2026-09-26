#import "AppDelegate.h"
#import "MainWindowController.h"

#import <Carbon/Carbon.h> // kAEOpenApplication / keyAEPropData / keyAELaunchedAsLogInItem
#import <CoreSpotlight/CoreSpotlight.h>
#import "tk_locale.h"
#include "tesseract/launch_args.h"
#include "tesseract/paths.h"
#include "tk/single_instance.h"
#include <string>
#include <utility>

@implementation AppDelegate
{
    MainWindowController* _windowController;
    // Parsed command line, handed over by main() (see ui/shared/app/Launch.h).
    tesseract::LaunchPlan _launchPlan;
    // Best-effort "was I launched by the login-item mechanism" signal —
    // set in applicationWillFinishLaunching: (before the event queue could
    // have discarded the Apple Event), read in applicationDidFinishLaunching:
    // to decide whether to skip the initial window show.
    BOOL _launchedAsLoginItem;
}

- (void)setLaunchPlan:(tesseract::LaunchPlan)plan
{
    _launchPlan = std::move(plan);
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
// already-running instance to show its window and act on the duplicate's
// command line — see -handleActivateRequest:. NSRunningApplication's
// activateWithOptions: (below) can only bring the other process's app to the
// front; it can't reach into that process to un-hide a window that was
// orderOut:'d to the menu-bar tray, or open a room, so this fills that gap
// the same way WM_COPYDATA (Windows) / the ActivationListener socket
// (Qt6/GTK4) do for their platforms. The name carries the --profile suffix
// so a request only reaches the instance of the same profile.
static NSString* activateRequestNotificationName()
{
    return [@"io.gnomos.Tesseract.ActivateRequest"
        stringByAppendingString:[NSString stringWithUTF8String:
                                              tesseract::profile_suffix().c_str()]];
}

// userInfo keys of the activate-request notification. All optional.
static NSString* const kActivateUriKey = @"uri";
static NSString* const kActivateActionKey = @"action";
static NSString* const kActivateRoomKey = @"room_id";

- (void)handleActivateRequest:(NSNotification*)note
{
    // Same recipe as a Dock-icon reopen with no visible windows.
    [self applicationShouldHandleReopen:NSApp hasVisibleWindows:NO];

    NSDictionary* info = note.userInfo;
    NSString* uri = [info[kActivateUriKey] isKindOfClass:NSString.class]
                        ? info[kActivateUriKey] : nil;
    NSString* action = [info[kActivateActionKey] isKindOfClass:NSString.class]
                           ? info[kActivateActionKey] : nil;
    NSString* roomId = [info[kActivateRoomKey] isKindOfClass:NSString.class]
                           ? info[kActivateRoomKey] : nil;
    if (uri.length > 0)
        [_windowController openMatrixLink:uri];
    if (action.length > 0)
    {
        [_windowController
            dispatchLaunchAction:tesseract::launch_action_from_option_id(
                                     [action UTF8String])
                          roomId:roomId ?: @""];
    }
}

- (void)applicationWillFinishLaunching:(NSNotification*)note
{
    _launchedAsLoginItem = [self _wasLaunchedAsLoginItem];

    [NSDistributedNotificationCenter.defaultCenter
        addObserver:self
           selector:@selector(handleActivateRequest:)
               name:activateRequestNotificationName()
             object:nil];

#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    if (_launchPlan.args.screenshot_dir)
        return; // Screenshot builds may run beside an installed instance.
#endif

    // Raise the existing instance of this --profile and abort if we are a
    // duplicate. LSMultipleInstancesProhibited in Info.plist covers
    // Finder/Dock launches; this handles command-line and IDE launches where
    // LaunchServices is bypassed. A per-profile flock (not a bundle-id scan)
    // decides, so a named profile started from a terminal runs beside the
    // default one instead of being mistaken for a duplicate.
    if (!tk::acquire_single_instance_lock(_launchPlan.instance_lock_wait()).acquired)
    {
        // A duplicate hidden/autostart launch with nothing to forward has no
        // meaningful action against the running instance — terminate
        // quietly without raising it (which would defeat staying hidden).
        if (!_launchedAsLoginItem && _launchPlan.should_raise_existing_instance())
        {
            if (NSRunningApplication* other = [NSRunningApplication
                    runningApplicationWithProcessIdentifier:
                        tk::single_instance_owner_pid()])
            {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                [other activateWithOptions:NSApplicationActivateIgnoringOtherApps];
#pragma clang diagnostic pop
            }
            const tesseract::LaunchArgs& a = _launchPlan.args;
            NSMutableDictionary* info = [NSMutableDictionary dictionary];
            if (a.matrix_uri)
                info[kActivateUriKey] =
                    [NSString stringWithUTF8String:a.matrix_uri->c_str()];
            if (a.action != tesseract::LaunchAction::None)
                info[kActivateActionKey] = [NSString
                    stringWithUTF8String:std::string(
                                             tesseract::launch_action_option_id(
                                                 a.action))
                                             .c_str()];
            if (a.room_id)
                info[kActivateRoomKey] =
                    [NSString stringWithUTF8String:a.room_id->c_str()];
            [NSDistributedNotificationCenter.defaultCenter
                postNotificationName:activateRequestNotificationName()
                              object:nil
                            userInfo:info
                  deliverImmediately:YES];
        }
        [NSApp terminate:nil];
    }
}

- (void)applicationDidFinishLaunching:(NSNotification*)note
{
    // Settings, crash handler and locale were set up by prepare_launch() in
    // main() — before any views are constructed.
    // A copy: the dispatch_async block below captures it by value.
    const tesseract::LaunchArgs launch = _launchPlan.args;
    const BOOL startHidden = _launchedAsLoginItem || _launchPlan.start_hidden();

    _windowController = [[MainWindowController alloc] init];
    _windowController.startedHidden = startHidden;
    if (!startHidden)
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
        if (launch.screenshot_dir)
        {
            NSString* dir =
                [NSString stringWithUTF8String:launch.screenshot_dir->c_str()];
            [_windowController captureScreenshotsToDirectory:dir];
            return;
        }
#endif
        [_windowController beginLogin];
        // Command-line launch intents, same as a forwarded request. Both are
        // held by the shell until there is something to act on.
        if (launch.matrix_uri)
        {
            [_windowController
                openMatrixLink:[NSString
                                   stringWithUTF8String:launch.matrix_uri->c_str()]];
        }
        [_windowController
            dispatchLaunchAction:launch.action
                          roomId:[NSString stringWithUTF8String:
                                               launch.room_id.value_or(std::string{})
                                                   .c_str()]];
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
    [appMenu addItemWithTitle:TkTr("Settings\xe2\x80\xa6")
                       action:@selector(openSettingsMenuAction:)
                keyEquivalent:@","];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:TkTr("Hide Tesseract")
                       action:@selector(hide:)
                keyEquivalent:@"h"];
    [appMenu addItemWithTitle:TkTr("Quit Tesseract")
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    [mainMenu addItem:appItem];

    // ── File menu ─────────────────────────────────────────────────────
    NSMenuItem* fileItem = [[NSMenuItem alloc] init];
    NSMenu* fileMenu = [[NSMenu alloc] initWithTitle:TkTr("File")];
    [fileMenu addItemWithTitle:TkTr("Add Room\xe2\x80\xa6")
                        action:@selector(addRoomMenuAction:)
                 keyEquivalent:@"n"];
    fileItem.submenu = fileMenu;
    [mainMenu addItem:fileItem];

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

    // Edit ▸ Find submenu (macOS convention for search commands).
    NSMenuItem* findItem =
        [editMenu addItemWithTitle:TkTr("Find") action:nil keyEquivalent:@""];
    NSMenu* findMenu = [[NSMenu alloc] initWithTitle:TkTr("Find")];
    NSMenuItem* findInConvItem =
        [findMenu addItemWithTitle:TkTr("Find\xe2\x80\xa6")
                            action:@selector(findInConversationMenuAction:)
                     keyEquivalent:@"f"];
    findInConvItem.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    NSMenuItem* searchAllItem =
        [findMenu addItemWithTitle:TkTr("Search Your Messages\xe2\x80\xa6")
                            action:@selector(searchAllMessagesMenuAction:)
                     keyEquivalent:@"f"];
    searchAllItem.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    findItem.submenu = findMenu;

    [editMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* emojiItem =
        [editMenu addItemWithTitle:TkTr("Insert Emoji\xe2\x80\xa6")
                            action:@selector(showEmojiPicker:)
                     keyEquivalent:@"e"];
    emojiItem.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    editItem.submenu = editMenu;
    [mainMenu addItem:editItem];

    // ── Go menu ───────────────────────────────────────────────────────
    NSMenuItem* goItem = [[NSMenuItem alloc] init];
    NSMenu* goMenu = [[NSMenu alloc] initWithTitle:TkTr("Go")];
    [goMenu addItemWithTitle:TkTr("Go Back")
                      action:@selector(goBackMenuAction:)
               keyEquivalent:@"["];
    [goMenu addItemWithTitle:TkTr("Go Forward")
                      action:@selector(goForwardMenuAction:)
               keyEquivalent:@"]"];
    [goMenu addItem:[NSMenuItem separatorItem]];
    [goMenu addItemWithTitle:TkTr("Quick Switcher\xe2\x80\xa6")
                      action:@selector(openQuickSwitcherMenuAction:)
               keyEquivalent:@"k"];
    [goMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* cycleItem =
        [goMenu addItemWithTitle:TkTr("Cycle Recent Rooms")
                          action:@selector(cycleRecentRoomsMenuAction:)
                   keyEquivalent:@"\t"];
    cycleItem.keyEquivalentModifierMask = NSEventModifierFlagControl;
    NSMenuItem* cycleBackItem =
        [goMenu addItemWithTitle:TkTr("Cycle Recent Rooms Backward")
                          action:@selector(cycleRecentRoomsBackwardMenuAction:)
                   keyEquivalent:@"\t"];
    cycleBackItem.keyEquivalentModifierMask =
        NSEventModifierFlagControl | NSEventModifierFlagShift;
    goItem.submenu = goMenu;
    [mainMenu addItem:goItem];

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

// Menu-bar actions. AppDelegate is always in the responder chain (it is
// NSApp.delegate), so these reach the main window controller even while a
// pop-out room window is key — the controller's helpers bring the main
// window forward as needed.
- (void)openSettingsMenuAction:(id)sender
{
    [_windowController openSettingsMenuAction:sender];
}
- (void)addRoomMenuAction:(id)sender
{
    [_windowController addRoomMenuAction:sender];
}
- (void)findInConversationMenuAction:(id)sender
{
    [_windowController findInConversationMenuAction:sender];
}
- (void)searchAllMessagesMenuAction:(id)sender
{
    [_windowController searchAllMessagesMenuAction:sender];
}
- (void)goBackMenuAction:(id)sender
{
    [_windowController goBackMenuAction:sender];
}
- (void)goForwardMenuAction:(id)sender
{
    [_windowController goForwardMenuAction:sender];
}
- (void)openQuickSwitcherMenuAction:(id)sender
{
    [_windowController openQuickSwitcherMenuAction:sender];
}
- (void)cycleRecentRoomsMenuAction:(id)sender
{
    [_windowController cycleRecentRoomsMenuAction:sender];
}
- (void)cycleRecentRoomsBackwardMenuAction:(id)sender
{
    [_windowController cycleRecentRoomsBackwardMenuAction:sender];
}

- (BOOL)validateUserInterfaceItem:(id<NSValidatedUserInterfaceItem>)item
{
    SEL action = [item action];
    if (action == @selector(openSettingsMenuAction:) ||
        action == @selector(addRoomMenuAction:) ||
        action == @selector(findInConversationMenuAction:) ||
        action == @selector(searchAllMessagesMenuAction:) ||
        action == @selector(goBackMenuAction:) ||
        action == @selector(goForwardMenuAction:) ||
        action == @selector(openQuickSwitcherMenuAction:) ||
        action == @selector(cycleRecentRoomsMenuAction:) ||
        action == @selector(cycleRecentRoomsBackwardMenuAction:))
    {
        return [_windowController validateMenuAction:action];
    }
    return YES;
}

@end
