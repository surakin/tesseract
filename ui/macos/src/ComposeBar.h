#pragma once
#import <AppKit/AppKit.h>

namespace tesseract { class Client; }

// Multi-line text input bar at the bottom of the chat view.
// Enter sends; Shift+Enter inserts a newline.
// Height grows with content up to maxHeight, then scrolls.
@interface ComposeBar : NSView

// Called on the main thread when the user submits a non-empty message.
@property (nonatomic, copy) void (^onSend)(NSString* body);

// Borrowed SDK client. The compose bar reads/writes
// `io.element.recent_emoji` account-data through it (top + bump). Must
// outlive the ComposeBar.
@property (nonatomic, assign) tesseract::Client* client;

- (void)clear;
- (void)focusInput;

// Insert `glyph` at the current cursor position in the text view; the
// existing selection (if any) is replaced. Used by the emoji picker.
- (void)insertEmoji:(NSString*)glyph;

@end
