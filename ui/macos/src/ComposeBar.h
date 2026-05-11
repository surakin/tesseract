#pragma once
#import <AppKit/AppKit.h>

// Multi-line text input bar at the bottom of the chat view.
// Enter sends; Shift+Enter inserts a newline.
// Height grows with content up to maxHeight, then scrolls.
@interface ComposeBar : NSView

// Called on the main thread when the user submits a non-empty message.
@property (nonatomic, copy) void (^onSend)(NSString* body);

- (void)clear;
- (void)focusInput;

@end
