#pragma once
#import <AppKit/AppKit.h>

#include "tk/canvas.h"
#include "tk/theme.h"

#include <functional>
#include <string>
#include <vector>

namespace tesseract
{
class Client;
}

/// Floating sticker picker presented as a utility panel. Singleton via +sharedPanel.
@interface StickerPickerPanel : NSPanel

/// Called when the user picks a sticker; strings are UTF-8.
@property(nonatomic, copy) void (^onSelected)
    (NSString* url, NSString* body, NSString* infoJson);

/// Borrowed SDK client.
@property(nonatomic, assign) tesseract::Client* client;

+ (instancetype)sharedPanel;

/// The already-created shared panel, or nil if it was never shown. Lets
/// the shell re-theme it without force-creating it.
+ (instancetype)existingPanel;

/// Re-skin the picker surface when the theme preference changes.
- (void)setTheme:(const tk::Theme&)t;

/// Update the image provider. Call whenever the host's image cache changes.
- (void)setImageProvider:
    (std::function<const tk::Image*(const std::string&, const std::string&)>)
        provider;

/// Repaint the grid — call after adding images to the host cache.
- (void)invalidateImageCache;

/// Re-pull packs from the SDK. Call from on_image_packs_updated.
- (void)refreshPacks;

/// Which room this panel is currently being shown for (this is a
/// singleton panel shared across the main window and every pop-out room
/// window, so whichever caller shows it last determines the room context)
/// — forwarded to the wrapped shared picker; call before refreshPacks so
/// the room's own pack sorts right after the personal pack.
- (void)setCurrentRoomId:(const std::string&)roomId;

/// Every Space (direct and ancestor) that the current room is in —
/// forwarded to the wrapped shared picker; call before refreshPacks so
/// those spaces' own packs sort right after the current room's pack.
- (void)setCurrentRoomParentSpaces:(const std::vector<std::string>&)spaceIds;

/// Show the panel above anchorView.
- (void)popupAboveView:(NSView*)anchorView;

/// Show the panel above and centered on localRect (surface-local coords) inside anchor.
- (void)popupAtRect:(tk::Rect)localRect inView:(NSView*)anchor;

@end
