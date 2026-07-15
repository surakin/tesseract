#import "StickerPicker.h"
#import "tk_locale.h"

#include "tk/host.h"
#include "tk/host_macos.h"
#include "tk/theme.h"
#include "views/StickerPicker.h"

#include <memory>
#include <string>
#include <functional>

namespace
{

constexpr CGFloat kStickerPanelWidth = 360;
constexpr CGFloat kStickerPanelHeight = 420;

} // namespace

// File scope (not a +sharedPanel local) so +existingPanel can hand the
// shell the already-created panel for re-theming without force-creating one.
static StickerPickerPanel* g_stickerPanel = nil;

@implementation StickerPickerPanel
{
    std::unique_ptr<tk::macos::Surface> _surface;
    tesseract::views::StickerPicker* _shared; // borrowed
}

+ (instancetype)existingPanel
{
    return g_stickerPanel;
}

- (BOOL)canBecomeKeyWindow
{
    return YES;
}

- (void)resignKeyWindow
{
    BOOL wasVisible = self.isVisible;
    [super resignKeyWindow];
    if (wasVisible)
    {
        [self orderOut:nil];
        if (self.onDismiss)
        {
            self.onDismiss();
        }
    }
}

- (void)setTheme:(const tk::Theme&)t
{
    if (_surface)
    {
        _surface->set_theme(t);
    }
}

+ (instancetype)sharedPanel
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSWindowStyleMask mask =
            NSWindowStyleMaskNonactivatingPanel;
        NSRect frame = NSMakeRect(0, 0, kStickerPanelWidth, kStickerPanelHeight);
        g_stickerPanel = [[StickerPickerPanel alloc]
            initWithContentRect:frame
                      styleMask:mask
                        backing:NSBackingStoreBuffered
                          defer:NO];
        g_stickerPanel.title = TkTr("Stickers");
        g_stickerPanel.floatingPanel = YES;
        g_stickerPanel.becomesKeyOnlyIfNeeded = NO;
        g_stickerPanel.hidesOnDeactivate = YES;
        g_stickerPanel.releasedWhenClosed = NO;
        [g_stickerPanel _setUpContent];
    });
    return g_stickerPanel;
}

- (void)_setUpContent
{
    _surface = std::make_unique<tk::macos::Surface>(tk::Theme::light());

    auto shared =
        std::make_unique<tesseract::views::StickerPicker>(&_surface->host());
    _shared = shared.get();

    __weak StickerPickerPanel* weakSelf = self;
    _shared->on_selected = [weakSelf](const tesseract::ImagePackImage& img)
    {
        StickerPickerPanel* s = weakSelf;
        if (!s || !s.onSelected)
        {
            return;
        }
        const std::string& body = img.body.empty() ? img.shortcode : img.body;
        NSString* url = [NSString stringWithUTF8String:img.url.c_str()];
        NSString* bodyStr = [NSString stringWithUTF8String:body.c_str()];
        NSString* infoJson =
            [NSString stringWithUTF8String:img.info_json.c_str()];
        if (url)
        {
            s.onSelected(url, bodyStr ?: @"", infoJson ?: @"{}");
        }
    };
    _surface->set_root(std::move(shared));

    NSView* surfaceView = (__bridge NSView*)_surface->view_handle();
    surfaceView.translatesAutoresizingMaskIntoConstraints = NO;
    [self.contentView addSubview:surfaceView];
    [NSLayoutConstraint activateConstraints:@[
        [surfaceView.topAnchor
            constraintEqualToAnchor:self.contentView.topAnchor],
        [surfaceView.leadingAnchor
            constraintEqualToAnchor:self.contentView.leadingAnchor],
        [surfaceView.trailingAnchor
            constraintEqualToAnchor:self.contentView.trailingAnchor],
        [surfaceView.bottomAnchor
            constraintEqualToAnchor:self.contentView.bottomAnchor],
    ]];

}

- (void)setClient:(tesseract::Client*)client
{
    _client = client;
    if (_shared)
    {
        _shared->set_client(client);
    }
}

- (void)setImageProvider:
    (std::function<const tk::Image*(const std::string&, const std::string&)>)
        provider
{
    if (_shared)
    {
        _shared->set_image_provider(std::move(provider));
    }
}

- (void)invalidateImageCache
{
    if (_shared)
    {
        _shared->invalidate_image_cache();
    }
    if (_surface)
    {
        _surface->relayout();
    }
}

- (void)refreshPacks
{
    if (_shared)
    {
        _shared->refresh_packs();
    }
    if (_surface)
    {
        _surface->relayout();
    }
}

- (void)setCurrentRoomId:(const std::string&)roomId
{
    if (_shared)
    {
        _shared->set_current_room_id(roomId);
    }
}

- (void)setCurrentRoomParentSpaces:(const std::vector<std::string>&)spaceIds
{
    if (_shared)
    {
        _shared->set_current_room_parent_spaces(spaceIds);
    }
}

- (void)popupAboveView:(NSView*)anchor
{
    if (self.isVisible)
    {
        [self orderOut:nil];
        // Toggle-close (re-clicking the button that opened it): this is a
        // dismissal without a selection too, but doesn't go through
        // resignKeyWindow since the panel is closing itself while still
        // key. Fire onDismiss explicitly so callers see every close.
        if (self.onDismiss)
        {
            self.onDismiss();
        }
        return;
    }
    if (!anchor || !anchor.window)
    {
        return;
    }

    if (_shared)
    {
        _shared->refresh_packs();
        _shared->set_search_query("");
        if (auto* sf = _shared->search_field())
        {
            sf->set_text("");
        }
    }

    NSRect anchorRectInWindow = [anchor convertRect:anchor.bounds toView:nil];
    NSRect anchorRectScreen =
        [anchor.window convertRectToScreen:anchorRectInWindow];
    NSScreen* screen = anchor.window.screen ?: NSScreen.mainScreen;
    NSRect visible = screen.visibleFrame;

    NSRect frame = self.frame;
    frame.origin.x = anchorRectScreen.origin.x + anchorRectScreen.size.width -
                     frame.size.width;
    frame.origin.y =
        anchorRectScreen.origin.y + anchorRectScreen.size.height + 4;
    if (frame.origin.x < visible.origin.x)
    {
        frame.origin.x = visible.origin.x + 4;
    }
    if (frame.origin.x + frame.size.width > NSMaxX(visible))
    {
        frame.origin.x = NSMaxX(visible) - frame.size.width - 4;
    }
    if (frame.origin.y + frame.size.height > NSMaxY(visible))
    {
        frame.origin.y = anchorRectScreen.origin.y - frame.size.height - 4;
    }

    [self setFrame:frame display:NO];
    [self orderFront:nil];
    if (_surface)
    {
        _surface->relayout();
    }
    if (_shared && _shared->search_field())
    {
        __weak StickerPickerPanel* weakSelf = self;
        auto* sf = _shared->search_field();
        dispatch_async(dispatch_get_main_queue(), ^{
            StickerPickerPanel* s = weakSelf;
            if (s && s.isVisible)
            {
                [s makeKeyWindow];
                sf->set_focused(true);
            }
        });
    }
}

- (void)popupAtRect:(tk::Rect)localRect inView:(NSView*)anchor
{
    if (self.isVisible)
    {
        [self orderOut:nil];
        // Toggle-close (re-clicking the button that opened it): this is a
        // dismissal without a selection too, but doesn't go through
        // resignKeyWindow since the panel is closing itself while still
        // key. Fire onDismiss explicitly so callers see every close.
        if (self.onDismiss)
        {
            self.onDismiss();
        }
        return;
    }
    if (!anchor || !anchor.window)
    {
        return;
    }

    if (_shared)
    {
        _shared->refresh_packs();
        _shared->set_search_query("");
        if (auto* sf = _shared->search_field())
        {
            sf->set_text("");
        }
    }

    // tk::Rect is in the surface's flipped (top-left origin) widget
    // coordinates; the backing NSView has isFlipped == YES so it maps
    // directly into the anchor view's local rect.
    NSRect localR =
        NSMakeRect(localRect.x, localRect.y, localRect.w, localRect.h);
    NSRect inWindow = [anchor convertRect:localR toView:nil];
    NSRect screenRect = [anchor.window convertRectToScreen:inWindow];
    NSScreen* ns = anchor.window.screen ?: NSScreen.mainScreen;
    NSRect visible = ns.visibleFrame;

    NSRect frame = self.frame;
    // Center horizontally over the button; prefer above.
    frame.origin.x = screenRect.origin.x + screenRect.size.width / 2.0 -
                     frame.size.width / 2.0;
    frame.origin.y = NSMaxY(screenRect) + 4;
    if (frame.origin.y + frame.size.height > NSMaxY(visible))
    {
        frame.origin.y = screenRect.origin.y - frame.size.height - 4;
    }
    if (frame.origin.x < visible.origin.x)
    {
        frame.origin.x = visible.origin.x + 4;
    }
    if (frame.origin.x + frame.size.width > NSMaxX(visible))
    {
        frame.origin.x = NSMaxX(visible) - frame.size.width - 4;
    }

    [self setFrame:frame display:NO];
    [self orderFront:nil];
    if (_surface)
    {
        _surface->relayout();
    }
    if (_shared && _shared->search_field())
    {
        __weak StickerPickerPanel* weakSelf = self;
        auto* sf = _shared->search_field();
        dispatch_async(dispatch_get_main_queue(), ^{
            StickerPickerPanel* s = weakSelf;
            if (s && s.isVisible)
            {
                [s makeKeyWindow];
                sf->set_focused(true);
            }
        });
    }
}

@end
