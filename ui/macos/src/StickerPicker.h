#pragma once
#import <AppKit/AppKit.h>

#include "tk/canvas.h"

#include <functional>
#include <string>

namespace tesseract { class Client; }

/// Floating sticker picker presented as a utility panel. Singleton via +sharedPanel.
@interface StickerPickerPanel : NSPanel

/// Called when the user picks a sticker; strings are UTF-8.
@property (nonatomic, copy) void (^onSelected)(NSString* url,
                                                NSString* body,
                                                NSString* infoJson);

/// Borrowed SDK client.
@property (nonatomic, assign) tesseract::Client* client;

+ (instancetype)sharedPanel;

/// Update the image provider. Call whenever the host's image cache changes.
- (void)setImageProvider:(std::function<const tk::Image*(const std::string&,
                                                          const std::string&)>)provider;

/// Repaint the grid — call after adding images to the host cache.
- (void)invalidateImageCache;

/// Re-pull packs from the SDK. Call from on_image_packs_updated.
- (void)refreshPacks;

/// Show the panel above anchorView.
- (void)popupAboveView:(NSView*)anchorView;

@end
