#import "StickerPicker.h"

#include "tk/host.h"
#include "tk/host_macos.h"
#include "tk/theme.h"
#include "views/StickerPicker.h"

#include <memory>
#include <string>
#include <functional>

namespace
{

constexpr CGFloat kPanelWidth = 360;
constexpr CGFloat kPanelHeight = 420;

} // namespace

// File scope (not a +sharedPanel local) so +existingPanel can hand the
// shell the already-created panel for re-theming without force-creating one.
static StickerPickerPanel* g_stickerPanel = nil;

@implementation StickerPickerPanel
{
    std::unique_ptr<tk::macos::Surface> _surface;
    tesseract::views::StickerPicker* _shared; // borrowed
    std::unique_ptr<tk::NativeTextField> _searchField;
}

+ (instancetype)existingPanel
{
    return g_stickerPanel;
}

- (BOOL)canBecomeKeyWindow
{
    return YES;
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
        NSRect frame = NSMakeRect(0, 0, kPanelWidth, kPanelHeight);
        g_stickerPanel = [[StickerPickerPanel alloc]
            initWithContentRect:frame
                      styleMask:mask
                        backing:NSBackingStoreBuffered
                          defer:NO];
        g_stickerPanel.title = @"Stickers";
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

    auto shared = std::make_unique<tesseract::views::StickerPicker>();
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

    _searchField = _surface->host().make_text_field();
    _searchField->set_placeholder("Search stickers");
    _searchField->set_on_changed(
        [weakSelf](const std::string& q)
        {
            StickerPickerPanel* s = weakSelf;
            if (!s)
            {
                return;
            }
            if (s->_shared)
            {
                s->_shared->set_search_query(q);
            }
            if (s->_surface)
            {
                s->_surface->relayout();
            }
        });
    _surface->set_on_layout(
        [weakSelf]
        {
            StickerPickerPanel* s = weakSelf;
            if (!s || !s->_shared || !s->_searchField)
            {
                return;
            }
            s->_searchField->set_rect(s->_shared->search_field_rect());
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
        _shared->refresh_packs();
        _shared->set_search_query("");
    }
    if (_searchField)
    {
        _searchField->set_text("");
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
    if (_searchField)
    {
        __weak StickerPickerPanel* weakSelf = self;
        auto* sf = _searchField.get();
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
    }
    if (_searchField)
    {
        _searchField->set_text("");
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
    if (_searchField)
    {
        __weak StickerPickerPanel* weakSelf = self;
        auto* sf = _searchField.get();
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
