#pragma once
#import <UIKit/UIKit.h>

#include <string>

namespace tesseract { class Client; }

/// Floating emoji picker presented via UIPopoverPresentationController.
/// Bottom tab strip (Frequently Used + Unicode categories), search field
/// at the top, and a scrollable grid of buttons in the middle. Insertion
/// is delegated to the parent through the `onSelect` block.
@interface EmojiPickerController : UIViewController

/// Borrowed SDK client; must outlive the picker. Used to read/write
/// `io.element.recent_emoji` account-data for the Frequents tab.
@property (nonatomic, assign) tesseract::Client* client;

/// Fired when the user picks an emoji; the NSString is the UTF-8 glyph.
@property (nonatomic, copy)   void (^onSelect)(NSString* glyph);

/// Called by the host before each presentation to refresh the
/// Frequently Used tab from `client->recent_emoji_top(...)`.
- (void)refreshFrequents;

@end
