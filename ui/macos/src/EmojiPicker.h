#pragma once
#import <AppKit/AppKit.h>

namespace tesseract { class Client; }

/// Floating emoji picker presented as a utility panel. The panel hosts a
/// tk::macos::Surface that paints the shared
/// `tesseract::views::EmojiPicker`, with an NSTextField overlay for the
/// search row. The panel is a process-wide singleton accessed via
/// `+sharedPanel`; per-window state (client pointer, on-select callback)
/// is set before each show.
@interface EmojiPickerPanel : NSPanel

/// Fired when the user picks an emoji; the NSString is the UTF-8 glyph.
@property (nonatomic, copy) void (^onSelect)(NSString* glyph);

/// Borrowed SDK client; must outlive the panel's lifetime. The shared
/// picker reads recent_emoji_top + writes recent_emoji_bump.
@property (nonatomic, assign) tesseract::Client* client;

+ (instancetype)sharedPanel;

/// Position the panel so its bottom edge sits just above `anchorView`,
/// then show it. The anchor's window becomes the panel's parent so the
/// panel hides automatically when the window loses key.
- (void)popupAboveView:(NSView*)anchorView;

@end
