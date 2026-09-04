#pragma once
#import <AppKit/AppKit.h>

#include <tesseract/types.h>

#include <memory>
#include <string>
#include <vector>

/// Top-level AppKit window controller. Owns the `tesseract::Client`,
/// implements `IEventHandler`, and hosts the shared widget tree via two
/// `tk::macos::Surface` views (sidebar room list, message list) plus
/// native AppKit chrome (room header, compose bar).
@interface MainWindowController : NSWindowController

/// Set by AppDelegate right after -init when launched via the OS
/// login-item mechanism (--launched-as-login-item Apple Event) and the
/// window was therefore not shown. -beginLogin's async restore completion
/// reads this: stays hidden on a successful silent restore, force-shows
/// the window (and clears this back to NO) if there's no saved session.
@property (nonatomic) BOOL startedHidden;

/// Kick off the sign-in flow. Called from AppDelegate after the window
/// is on screen (or, when startedHidden, without showing it).
- (void)beginLogin;

#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
/// Seed the network-free fixture, save light/dark PNGs, then terminate.
- (void)captureScreenshotsToDirectory:(NSString*)directory;
#endif

/// Stop the background sync loop. Called from
/// `applicationWillTerminate:` so the SDK exits cleanly.
- (void)stopSync;

/// Show the emoji picker anchored to the compose bar. Wired from the
/// AppDelegate's "Insert Emoji" menu item.
- (void)showEmojiPicker:(id)sender;

/// Application-menu-bar actions. Each mirrors a keyboard shortcut already
/// handled by MainWindowController's local NSEvent monitor; the menu items
/// exist for discoverability and mouse access. AppDelegate forwards the
/// responder-chain selectors here (it is always reachable via NSApp.delegate,
/// even while a pop-out room window is key).
- (void)openSettingsMenuAction:(id)sender;
- (void)addRoomMenuAction:(id)sender;
- (void)findInConversationMenuAction:(id)sender;
- (void)searchAllMessagesMenuAction:(id)sender;
- (void)goBackMenuAction:(id)sender;
- (void)goForwardMenuAction:(id)sender;
- (void)openQuickSwitcherMenuAction:(id)sender;
- (void)cycleRecentRoomsMenuAction:(id)sender;
- (void)cycleRecentRoomsBackwardMenuAction:(id)sender;

/// Whether a given menu action selector should be enabled right now.
/// Consulted from AppDelegate's -validateUserInterfaceItem:.
- (BOOL)validateMenuAction:(SEL)action;

/// Navigate to the target described by a matrix.to or matrix: URI.
- (void)openMatrixLink:(NSString*)uri;

/// Navigate to the highest-priority unread room across all signed-in accounts.
/// No-op when there is nothing unread.
- (void)navigateToUnread;

/// Resolve a Spotlight CSSearchableItem identifier (a room id or mxid, see
/// MacSpotlightSearch) back to a room/contact and activate it. Called from
/// AppDelegate's -application:continueUserActivity:restorationHandler:.
- (void)activateSpotlightResult:(NSString*)identifier;

@end
