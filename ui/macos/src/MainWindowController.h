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

/// Kick off the sign-in flow. Called from AppDelegate after the window
/// is on screen.
- (void)beginLogin;

/// Stop the background sync loop. Called from
/// `applicationWillTerminate:` so the SDK exits cleanly.
- (void)stopSync;

/// Show the emoji picker anchored to the compose bar. Wired from the
/// AppDelegate's "Insert Emoji" menu item.
- (void)showEmojiPicker:(id)sender;

/// Navigate to the target described by a matrix.to or matrix: URI.
- (void)openMatrixLink:(NSString*)uri;

/// Navigate to the highest-priority unread room across all signed-in accounts.
/// No-op when there is nothing unread.
- (void)navigateToUnread;

@end
