#import "EmojiPicker.h"
#import "tk_locale.h"

#include "tk/host.h"
#include "tk/host_macos.h"
#include "tk/theme.h"
#include "views/EmojiPicker.h"

#include <tesseract/image_pack.h>

#include <memory>
#include <string>

namespace
{

constexpr CGFloat kEmojiPanelWidth = 320;
constexpr CGFloat kEmojiPanelHeight = 360;

} // namespace

// File scope (not a +sharedPanel local) so +existingPanel can hand the
// shell the already-created panel for re-theming without force-creating one.
static EmojiPickerPanel* g_emojiPanel = nil;

@implementation EmojiPickerPanel
{
    std::unique_ptr<tk::macos::Surface> _surface;
    tesseract::views::EmojiPicker* _shared; // borrowed
    std::unique_ptr<tk::NativeTextField> _searchField;
}

+ (instancetype)existingPanel
{
    return g_emojiPanel;
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
        NSRect frame = NSMakeRect(0, 0, kEmojiPanelWidth, kEmojiPanelHeight);
        g_emojiPanel =
            [[EmojiPickerPanel alloc] initWithContentRect:frame
                                                styleMask:mask
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
        g_emojiPanel.title = TkTr("Emoji");
        g_emojiPanel.floatingPanel = YES;
        g_emojiPanel.becomesKeyOnlyIfNeeded = NO;
        g_emojiPanel.hidesOnDeactivate = YES;
        g_emojiPanel.releasedWhenClosed = NO;
        [g_emojiPanel _setUpContent];
    });
    return g_emojiPanel;
}

- (void)_setUpContent
{
    _surface = std::make_unique<tk::macos::Surface>(tk::Theme::light());

    auto shared = std::make_unique<tesseract::views::EmojiPicker>();
    _shared = shared.get();
    __weak EmojiPickerPanel* weakSelf = self;
    _shared->on_selected = [weakSelf](const std::string& glyph)
    {
        EmojiPickerPanel* s = weakSelf;
        if (!s)
        {
            return;
        }
        if (s.onSelect)
        {
            NSString* g = [NSString stringWithUTF8String:glyph.c_str()];
            if (g)
            {
                s.onSelect(g);
            }
        }
    };
    _shared->on_emoticon_selected =
        [weakSelf](const tesseract::ImagePackImage& img)
    {
        EmojiPickerPanel* s = weakSelf;
        if (s && s.onEmoticonSelect)
        {
            s.onEmoticonSelect(img);
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

    _searchField = _surface->host().make_text_field();
    _searchField->set_placeholder("Search emoji");
    _searchField->set_on_changed(
        [weakSelf](const std::string& q)
        {
            EmojiPickerPanel* s = weakSelf;
            if (s)
            {
                [s _onSearchChanged:q];
            }
        });
    _surface->set_on_layout(
        [weakSelf]
        {
            EmojiPickerPanel* s = weakSelf;
            if (s)
            {
                [s _positionOverlay];
            }
        });
}

- (void)setClient:(tesseract::Client*)client
{
    _client = client;
    if (_shared)
    {
        _shared->set_client(client);
    }
}

- (void)setCurrentRoomId:(const std::string&)roomId
{
    if (_shared)
    {
        _shared->set_current_room_id(roomId);
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

- (void)_onSearchChanged:(const std::string&)q
{
    if (_shared)
    {
        _shared->set_search_query(q);
    }
    if (_surface)
    {
        _surface->relayout();
    }
}

- (void)_positionOverlay
{
    if (!_shared || !_searchField)
    {
        return;
    }
    _searchField->set_rect(_shared->search_field_rect());
}

- (void)popupAboveView:(NSView*)anchor
{
    if (self.isVisible)
    {
        [self orderOut:nil];
        return;
    }
    if (!anchor || !anchor.window)
    {
        return;
    }

    if (_shared)
    {
        _shared->refresh_frequents();
        _shared->set_search_query("");
    }
    if (_searchField)
    {
        _searchField->set_text("");
    }

    // Position above the anchor in screen coords.
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
    if (_searchField)
    {
        __weak EmojiPickerPanel* weakSelf = self;
        auto* sf = _searchField.get();
        dispatch_async(dispatch_get_main_queue(), ^{
            EmojiPickerPanel* s = weakSelf;
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
        return;
    }
    if (!anchor || !anchor.window)
    {
        return;
    }

    if (_shared)
    {
        _shared->refresh_frequents();
        _shared->set_search_query("");
    }
    if (_searchField)
    {
        _searchField->set_text("");
    }

    // tk::Rect lives in the surface's flipped (top-left origin) widget
    // coordinates. The shared message-list view is the root of a
    // tk::macos::Surface whose backing NSView has isFlipped == YES, so
    // the rect maps directly into the anchor view's local rect.
    NSRect anchorRectLocal =
        NSMakeRect(localRect.x, localRect.y, localRect.w, localRect.h);
    NSRect anchorRectInWindow = [anchor convertRect:anchorRectLocal toView:nil];
    NSRect anchorRectScreen =
        [anchor.window convertRectToScreen:anchorRectInWindow];

    NSScreen* screen = anchor.window.screen ?: NSScreen.mainScreen;
    NSRect visible = screen.visibleFrame;

    NSRect frame = self.frame;
    // Prefer popping above the rect, centered on it. In NSWindow
    // (non-flipped) coordinates, "above" means a larger y.
    frame.origin.x = anchorRectScreen.origin.x +
                     anchorRectScreen.size.width / 2.0 - frame.size.width / 2.0;
    frame.origin.y = NSMaxY(anchorRectScreen) + 4;
    if (frame.origin.y + frame.size.height > NSMaxY(visible))
    {
        frame.origin.y = anchorRectScreen.origin.y - frame.size.height - 4;
    }
    if (frame.origin.x < visible.origin.x)
    {
        frame.origin.x = visible.origin.x + 4;
    }
    if (frame.origin.x + frame.size.width > NSMaxX(visible))
    {
        frame.origin.x = NSMaxX(visible) - frame.size.width - 4;
    }
    if (frame.origin.y < visible.origin.y)
    {
        frame.origin.y = visible.origin.y + 4;
    }

    [self setFrame:frame display:NO];
    [self orderFront:nil];
    if (_surface)
    {
        _surface->relayout();
    }
    if (_searchField)
    {
        __weak EmojiPickerPanel* weakSelf = self;
        auto* sf = _searchField.get();
        dispatch_async(dispatch_get_main_queue(), ^{
            EmojiPickerPanel* s = weakSelf;
            if (s && s.isVisible)
            {
                [s makeKeyWindow];
                sf->set_focused(true);
            }
        });
    }
}

@end
