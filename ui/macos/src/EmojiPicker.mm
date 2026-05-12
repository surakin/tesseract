#import "EmojiPicker.h"

#include "tk/host.h"
#include "tk/host_macos.h"
#include "tk/theme.h"
#include "views/EmojiPicker.h"

#include <memory>
#include <string>

namespace {

constexpr CGFloat kPanelWidth  = 320;
constexpr CGFloat kPanelHeight = 360;

} // namespace

@implementation EmojiPickerPanel {
    std::unique_ptr<tk::macos::Surface>             _surface;
    tesseract::views::EmojiPicker*                  _shared;        // borrowed
    std::unique_ptr<tk::NativeTextField>            _searchField;
}

+ (instancetype)sharedPanel {
    static EmojiPickerPanel* panel;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSWindowStyleMask mask = NSWindowStyleMaskTitled
                                  | NSWindowStyleMaskClosable
                                  | NSWindowStyleMaskUtilityWindow
                                  | NSWindowStyleMaskHUDWindow
                                  | NSWindowStyleMaskNonactivatingPanel;
        NSRect frame = NSMakeRect(0, 0, kPanelWidth, kPanelHeight);
        panel = [[EmojiPickerPanel alloc]
                   initWithContentRect:frame
                              styleMask:mask
                                backing:NSBackingStoreBuffered
                                  defer:NO];
        panel.title                  = @"Emoji";
        panel.floatingPanel          = YES;
        panel.becomesKeyOnlyIfNeeded = NO;
        panel.hidesOnDeactivate      = YES;
        panel.releasedWhenClosed     = NO;
        [panel _setUpContent];
    });
    return panel;
}

- (void)_setUpContent {
    _surface = std::make_unique<tk::macos::Surface>(tk::Theme::light());

    auto shared = std::make_unique<tesseract::views::EmojiPicker>();
    _shared = shared.get();
    __weak EmojiPickerPanel* weakSelf = self;
    _shared->on_selected = [weakSelf](const std::string& glyph) {
        EmojiPickerPanel* s = weakSelf;
        if (!s) return;
        if (s.onSelect) {
            NSString* g = [NSString stringWithUTF8String:glyph.c_str()];
            if (g) s.onSelect(g);
        }
    };
    _surface->set_root(std::move(shared));

    NSView* surfaceView = (__bridge NSView*)_surface->view_handle();
    surfaceView.translatesAutoresizingMaskIntoConstraints = NO;
    [self.contentView addSubview:surfaceView];
    [NSLayoutConstraint activateConstraints:@[
        [surfaceView.topAnchor      constraintEqualToAnchor:self.contentView.topAnchor],
        [surfaceView.leadingAnchor  constraintEqualToAnchor:self.contentView.leadingAnchor],
        [surfaceView.trailingAnchor constraintEqualToAnchor:self.contentView.trailingAnchor],
        [surfaceView.bottomAnchor   constraintEqualToAnchor:self.contentView.bottomAnchor],
    ]];

    _searchField = _surface->host().make_text_field();
    _searchField->set_placeholder("Search emoji");
    _searchField->set_on_changed([weakSelf](const std::string& q) {
        EmojiPickerPanel* s = weakSelf;
        if (s) [s _onSearchChanged:q];
    });
    _surface->set_on_layout([weakSelf] {
        EmojiPickerPanel* s = weakSelf;
        if (s) [s _positionOverlay];
    });
}

- (void)setClient:(tesseract::Client*)client {
    _client = client;
    if (_shared) _shared->set_client(client);
}

- (void)_onSearchChanged:(const std::string&)q {
    if (_shared) _shared->set_search_query(q);
    if (_surface) _surface->relayout();
}

- (void)_positionOverlay {
    if (!_shared || !_searchField) return;
    _searchField->set_rect(_shared->search_field_rect());
}

- (void)popupAboveView:(NSView*)anchor {
    if (!anchor || !anchor.window) return;

    if (_shared) {
        _shared->refresh_frequents();
        _shared->set_search_query("");
    }
    if (_searchField) _searchField->set_text("");

    // Position above the anchor in screen coords.
    NSRect anchorRectInWindow = [anchor convertRect:anchor.bounds toView:nil];
    NSRect anchorRectScreen   = [anchor.window convertRectToScreen:anchorRectInWindow];
    NSScreen* screen = anchor.window.screen ?: NSScreen.mainScreen;
    NSRect visible   = screen.visibleFrame;

    NSRect frame = self.frame;
    frame.origin.x = anchorRectScreen.origin.x
                      + anchorRectScreen.size.width - frame.size.width;
    frame.origin.y = anchorRectScreen.origin.y
                      + anchorRectScreen.size.height + 4;
    if (frame.origin.x < visible.origin.x) frame.origin.x = visible.origin.x + 4;
    if (frame.origin.x + frame.size.width > NSMaxX(visible))
        frame.origin.x = NSMaxX(visible) - frame.size.width - 4;
    if (frame.origin.y + frame.size.height > NSMaxY(visible))
        frame.origin.y = anchorRectScreen.origin.y - frame.size.height - 4;

    [self setFrame:frame display:NO];
    [self orderFront:nil];
    if (_searchField) _searchField->set_focused(true);
    if (_surface) _surface->relayout();
}

- (void)popupAtRect:(tk::Rect)localRect inView:(NSView*)anchor {
    if (!anchor || !anchor.window) return;

    if (_shared) {
        _shared->refresh_frequents();
        _shared->set_search_query("");
    }
    if (_searchField) _searchField->set_text("");

    // tk::Rect lives in the surface's flipped (top-left origin) widget
    // coordinates. The shared message-list view is the root of a
    // tk::macos::Surface whose backing NSView has isFlipped == YES, so
    // the rect maps directly into the anchor view's local rect.
    NSRect anchorRectLocal = NSMakeRect(localRect.x, localRect.y,
                                         localRect.w, localRect.h);
    NSRect anchorRectInWindow = [anchor convertRect:anchorRectLocal toView:nil];
    NSRect anchorRectScreen   = [anchor.window convertRectToScreen:anchorRectInWindow];

    NSScreen* screen = anchor.window.screen ?: NSScreen.mainScreen;
    NSRect visible   = screen.visibleFrame;

    NSRect frame = self.frame;
    // Prefer popping above the rect, left-aligned with it. In NSWindow
    // (non-flipped) coordinates, "above" means a larger y.
    frame.origin.x = anchorRectScreen.origin.x;
    frame.origin.y = NSMaxY(anchorRectScreen) + 4;
    if (frame.origin.y + frame.size.height > NSMaxY(visible))
        frame.origin.y = anchorRectScreen.origin.y - frame.size.height - 4;
    if (frame.origin.x < visible.origin.x) frame.origin.x = visible.origin.x + 4;
    if (frame.origin.x + frame.size.width > NSMaxX(visible))
        frame.origin.x = NSMaxX(visible) - frame.size.width - 4;
    if (frame.origin.y < visible.origin.y) frame.origin.y = visible.origin.y + 4;

    [self setFrame:frame display:NO];
    [self orderFront:nil];
    if (_searchField) _searchField->set_focused(true);
    if (_surface) _surface->relayout();
}

@end
