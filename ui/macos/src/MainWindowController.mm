#import "MainWindowController.h"
#import "LoginView.h"
#import "EmojiPicker.h"

#include <tesseract/client.h>
#include <tesseract/event_handler.h>
#include <tesseract/session_store.h>
#include <tesseract/visual.h>

#include "tk/canvas_cg.h"
#include "tk/host.h"
#include "tk/host_macos.h"
#include "tk/theme.h"
#include "views/ComposeBar.h"
#include "views/MessageListView.h"
#include "views/RecoveryBanner.h"
#include "views/RoomListView.h"

#include <ImageIO/ImageIO.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

@class MainWindowController;

// ─────────────────────────────────────────────────────────────────────────
//  EventBridge — IEventHandler that forwards to the controller on the
//  main thread. The SDK calls every method from worker threads; we route
//  through dispatch_async to land mutations on the AppKit run loop.
// ─────────────────────────────────────────────────────────────────────────

namespace {

class EventBridge final : public tesseract::IEventHandler {
public:
    explicit EventBridge(MainWindowController* controller)
        : controller_(controller) {}

    void on_message(tesseract::Event* ev) override;
    void on_rooms_updated(const std::vector<tesseract::RoomInfo>& rooms) override;
    void on_sync_error(const std::string& context,
                       const std::string& description,
                       bool soft_logout) override;
    void on_timeline_reset(const std::string& room_id) override;
    void on_session_saved(const std::string& session_json) override;
    void on_backup_progress(const tesseract::BackupProgress& progress) override;

private:
    MainWindowController* __weak controller_;
};

std::string trim(std::string s) {
    auto a = s.find_first_not_of(" \t\n\r");
    auto b = s.find_last_not_of (" \t\n\r");
    if (a == std::string::npos) return {};
    return s.substr(a, b - a + 1);
}

} // namespace

// Decoded-image cache entries — owned by the controller, referenced by
// borrowed pointer from the shared views' avatar/image providers.
using TkImagePtr = std::unique_ptr<tk::Image>;

// ─────────────────────────────────────────────────────────────────────────
//  Internal IBO that the C++ EventBridge calls back into.
// ─────────────────────────────────────────────────────────────────────────

@interface MainWindowController () <LoginViewDelegate>
- (void)pushEvent:(tesseract::Event*)ev;
- (void)updateRooms:(std::vector<tesseract::RoomInfo>)rooms;
- (void)handleSyncErrorContext:(NSString*)ctx
                    description:(NSString*)desc
                    softLogout:(BOOL)soft;
- (void)handleTimelineReset:(NSString*)roomId;
- (void)handleBackupProgress:(tesseract::BackupProgress)progress;

- (void)onRoomSelected:(std::string)roomId;
- (void)_onRecoveryVerify;
- (void)_onRecoveryDismiss;
- (void)_maybeShowRecoveryBanner;
- (void)showEmojiPickerAtRect:(tk::Rect)anchor;
@end

namespace {

void EventBridge::on_message(tesseract::Event* ev) {
    MainWindowController* c = controller_;
    if (!c || !ev) { delete ev; return; }
    // Hand ownership of the heap event to the main-thread block.
    dispatch_async(dispatch_get_main_queue(), ^{
        [c pushEvent:ev];
    });
}

void EventBridge::on_rooms_updated(
    const std::vector<tesseract::RoomInfo>& rooms)
{
    MainWindowController* c = controller_;
    if (!c) return;
    auto copy = rooms;
    dispatch_async(dispatch_get_main_queue(), ^{
        [c updateRooms:copy];
    });
}

void EventBridge::on_sync_error(const std::string& context,
                                  const std::string& description,
                                  bool soft_logout) {
    MainWindowController* c = controller_;
    if (!c) return;
    NSString* ctx  = [NSString stringWithUTF8String:context.c_str()]      ?: @"";
    NSString* desc = [NSString stringWithUTF8String:description.c_str()]  ?: @"";
    BOOL      soft = soft_logout;
    dispatch_async(dispatch_get_main_queue(), ^{
        [c handleSyncErrorContext:ctx description:desc softLogout:soft];
    });
}

void EventBridge::on_timeline_reset(const std::string& room_id) {
    MainWindowController* c = controller_;
    if (!c) return;
    NSString* rid = [NSString stringWithUTF8String:room_id.c_str()] ?: @"";
    dispatch_async(dispatch_get_main_queue(), ^{
        [c handleTimelineReset:rid];
    });
}

void EventBridge::on_session_saved(const std::string& session_json) {
    tesseract::SessionStore::save(session_json);
}

void EventBridge::on_backup_progress(const tesseract::BackupProgress& progress) {
    MainWindowController* c = controller_;
    if (!c) return;
    tesseract::BackupProgress copy = progress;
    dispatch_async(dispatch_get_main_queue(), ^{
        [c handleBackupProgress:copy];
    });
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────

@implementation MainWindowController {
    tesseract::Client                _client;
    std::unique_ptr<EventBridge>     _bridge;
    std::vector<tesseract::RoomInfo> _rooms;
    std::string                      _currentRoomId;
    std::string                      _myUserId;
    // When non-empty, the next emoji selection routes through
    // send_reaction for this event id (set by the "+" reaction chip).
    std::string                      _pendingReactionEventId;
    std::vector<std::string>         _spaceStack;

    // Shared widget tree.
    std::unique_ptr<tk::macos::Surface>             _roomSurface;
    std::unique_ptr<tk::macos::Surface>             _msgSurface;
    tesseract::views::RoomListView*                 _roomListView;     // borrowed
    tesseract::views::MessageListView*              _messageListView;  // borrowed
    std::unordered_map<std::string, TkImagePtr>     _tkAvatars;
    std::unordered_map<std::string, TkImagePtr>     _tkImages;

    // AppKit chrome.
    NSSplitView*   _splitView;
    NSView*        _sidebar;
    NSView*        _content;
    NSTextField*   _roomTitleLabel;
    LoginView*     _loginView;
    NSStackView*   _contentStack;

    // Compose bar — tk::macos::Surface hosting the shared ComposeBar
    // with a NativeTextArea overlay (NSTextView under the hood).
    std::unique_ptr<tk::macos::Surface>             _composeSurface;
    tesseract::views::ComposeBar*                    _composeShared;     // borrowed
    std::unique_ptr<tk::NativeTextArea>              _composeTextArea;
    NSLayoutConstraint*                               _composeHeightCon;

    // Recovery banner — shared widget on a tk::macos::Surface.
    std::unique_ptr<tk::macos::Surface>             _recoverySurface;
    tesseract::views::RecoveryBanner*               _recoveryShared;  // borrowed
    std::unique_ptr<tk::NativeTextField>            _recoveryKeyField;
    BOOL                                             _recoveryDismissed;
}

- (instancetype)init {
    NSRect frame = NSMakeRect(0, 0, 1100, 768);
    NSWindowStyleMask mask = NSWindowStyleMaskTitled
                              | NSWindowStyleMaskClosable
                              | NSWindowStyleMaskMiniaturizable
                              | NSWindowStyleMaskResizable;
    NSWindow* window = [[NSWindow alloc]
                          initWithContentRect:frame
                                     styleMask:mask
                                       backing:NSBackingStoreBuffered
                                         defer:NO];
    window.title           = @"Tesseract";
    window.minSize         = NSMakeSize(720, 480);
    window.titlebarAppearsTransparent = NO;
    window.releasedWhenClosed = NO;

    self = [super initWithWindow:window];
    if (!self) return nil;

    _bridge = std::make_unique<EventBridge>(self);
    [self _buildChrome];
    return self;
}

- (void)_buildChrome {
    NSView* content = self.window.contentView;
    content.wantsLayer = YES;

    // ── Sidebar ───────────────────────────────────────────────────────
    _sidebar = [[NSView alloc] initWithFrame:NSZeroRect];
    _sidebar.wantsLayer = YES;
    _sidebar.layer.backgroundColor =
        [NSColor colorWithCalibratedRed:0xF0/255.0
                                    green:0xF2/255.0
                                     blue:0xF5/255.0
                                    alpha:1.0].CGColor;

    _roomSurface = std::make_unique<tk::macos::Surface>(tk::Theme::light());
    auto room_view = std::make_unique<tesseract::views::RoomListView>();
    _roomListView  = room_view.get();
    _roomListView->set_avatar_provider(
        [self](const std::string& mxc) -> const tk::Image* {
            auto it = _tkAvatars.find(mxc);
            return it == _tkAvatars.end() ? nullptr : it->second.get();
        });
    _roomListView->on_room_selected =
        [self](const std::string& room_id) { [self onRoomSelected:room_id]; };
    _roomSurface->set_root(std::move(room_view));

    NSView* roomSurfaceView = (__bridge NSView*)_roomSurface->view_handle();
    roomSurfaceView.translatesAutoresizingMaskIntoConstraints = NO;
    [_sidebar addSubview:roomSurfaceView];
    [NSLayoutConstraint activateConstraints:@[
        [roomSurfaceView.topAnchor      constraintEqualToAnchor:_sidebar.topAnchor],
        [roomSurfaceView.leadingAnchor  constraintEqualToAnchor:_sidebar.leadingAnchor],
        [roomSurfaceView.trailingAnchor constraintEqualToAnchor:_sidebar.trailingAnchor],
        [roomSurfaceView.bottomAnchor   constraintEqualToAnchor:_sidebar.bottomAnchor],
    ]];

    // ── Content area ──────────────────────────────────────────────────
    _content = [[NSView alloc] initWithFrame:NSZeroRect];
    _content.wantsLayer = YES;
    _content.layer.backgroundColor = [NSColor whiteColor].CGColor;

    // Room header — a simple bordered strip with the room name.
    _roomTitleLabel = [NSTextField labelWithString:@""];
    _roomTitleLabel.font = [NSFont systemFontOfSize:14 weight:NSFontWeightSemibold];
    _roomTitleLabel.textColor = [NSColor labelColor];
    _roomTitleLabel.translatesAutoresizingMaskIntoConstraints = NO;
    NSView* header = [[NSView alloc] init];
    header.wantsLayer = YES;
    header.layer.backgroundColor = [NSColor windowBackgroundColor].CGColor;
    [header addSubview:_roomTitleLabel];
    [NSLayoutConstraint activateConstraints:@[
        [_roomTitleLabel.leadingAnchor  constraintEqualToAnchor:header.leadingAnchor constant:16],
        [_roomTitleLabel.centerYAnchor  constraintEqualToAnchor:header.centerYAnchor],
    ]];
    [header.heightAnchor constraintEqualToConstant:48].active = YES;

    // Message list surface.
    _msgSurface = std::make_unique<tk::macos::Surface>(tk::Theme::light());
    auto msg_view = std::make_unique<tesseract::views::MessageListView>();
    _messageListView = msg_view.get();
    _messageListView->set_avatar_provider(
        [self](const std::string& mxc) -> const tk::Image* {
            auto it = _tkAvatars.find(mxc);
            return it == _tkAvatars.end() ? nullptr : it->second.get();
        });
    _messageListView->set_image_provider(
        [self](const std::string& mxc) -> const tk::Image* {
            auto it = _tkImages.find(mxc);
            return it == _tkImages.end() ? nullptr : it->second.get();
        });
    {
        __weak MainWindowController* weakSelf = self;
        _messageListView->on_reaction_toggled =
            [weakSelf](const std::string& event_id, const std::string& key) {
                MainWindowController* s = weakSelf;
                if (!s) return;
                if (s->_currentRoomId.empty()) return;
                s->_client.send_reaction(s->_currentRoomId, event_id, key);
            };
        _messageListView->on_add_reaction_requested =
            [weakSelf](const std::string& event_id, tk::Rect anchor) {
                MainWindowController* s = weakSelf;
                if (!s) return;
                if (s->_currentRoomId.empty()) return;
                s->_pendingReactionEventId = event_id;
                // anchor is in MessageListView-local coords; the view is
                // the root of _msgSurface, whose backing NSView is
                // flipped so the rect maps directly to view-local.
                [s showEmojiPickerAtRect:anchor];
            };
    }
    _msgSurface->set_root(std::move(msg_view));
    NSView* msgSurfaceView = (__bridge NSView*)_msgSurface->view_handle();
    msgSurfaceView.translatesAutoresizingMaskIntoConstraints = NO;

    // Recovery banner — shared widget hosted in a tk::macos::Surface.
    _recoverySurface = std::make_unique<tk::macos::Surface>(tk::Theme::light());
    {
        auto banner = std::make_unique<tesseract::views::RecoveryBanner>();
        _recoveryShared = banner.get();
        __weak MainWindowController* weakSelf = self;
        _recoveryShared->on_verify = [weakSelf](const std::string& /*key*/) {
            MainWindowController* s = weakSelf;
            if (s) [s _onRecoveryVerify];
        };
        _recoveryShared->on_dismiss = [weakSelf] {
            MainWindowController* s = weakSelf;
            if (s) [s _onRecoveryDismiss];
        };
        _recoverySurface->set_root(std::move(banner));

        _recoveryKeyField = _recoverySurface->host().make_text_field();
        _recoveryKeyField->set_placeholder("Recovery key or passphrase");
        _recoveryKeyField->set_password(true);
        _recoveryKeyField->set_on_changed([weakSelf](const std::string& k) {
            MainWindowController* s = weakSelf;
            if (s && s->_recoveryShared) s->_recoveryShared->set_current_key(k);
        });
        _recoveryKeyField->set_on_submit([weakSelf] {
            MainWindowController* s = weakSelf;
            if (s) [s _onRecoveryVerify];
        });
        _recoverySurface->set_on_layout([weakSelf] {
            MainWindowController* s = weakSelf;
            if (!s || !s->_recoveryShared || !s->_recoveryKeyField) return;
            s->_recoveryKeyField->set_visible(
                s->_recoveryShared->recovery_key_field_visible());
            s->_recoveryKeyField->set_rect(
                s->_recoveryShared->recovery_key_field_rect());
        });
    }
    NSView* recoveryView = (__bridge NSView*)_recoverySurface->view_handle();
    recoveryView.translatesAutoresizingMaskIntoConstraints = NO;
    recoveryView.hidden = YES;

    // Compose bar — shared widget on a tk::macos::Surface. The text input
    // is a NativeTextArea overlay on the bar's text_area_rect; emoji and
    // send buttons paint into the toolkit.
    _composeSurface = std::make_unique<tk::macos::Surface>(tk::Theme::light());
    {
        auto bar = std::make_unique<tesseract::views::ComposeBar>();
        _composeShared = bar.get();
        __weak MainWindowController* weakSelf = self;
        _composeShared->on_send = [weakSelf](const std::string&) {
            MainWindowController* s = weakSelf;
            if (s) [s _onComposeSend];
        };
        _composeShared->on_emoji = [weakSelf] {
            MainWindowController* s = weakSelf;
            if (s) [s showEmojiPicker:nil];
        };
        _composeSurface->set_root(std::move(bar));

        _composeTextArea = _composeSurface->host().make_text_area();
        _composeTextArea->set_placeholder("Message…");
        _composeTextArea->set_on_changed([weakSelf](const std::string& s) {
            MainWindowController* c = weakSelf;
            if (c && c->_composeShared) c->_composeShared->set_current_text(s);
        });
        _composeTextArea->set_on_submit([weakSelf] {
            MainWindowController* c = weakSelf;
            if (c) [c _onComposeSend];
        });
        _composeTextArea->set_on_height_changed([weakSelf](float h) {
            MainWindowController* c = weakSelf;
            if (!c || !c->_composeShared) return;
            c->_composeShared->set_text_area_natural_height(h);
            if (c->_composeHeightCon)
                c->_composeHeightCon.constant = c->_composeShared->natural_height();
        });
        _composeSurface->set_on_layout([weakSelf] {
            MainWindowController* c = weakSelf;
            if (!c || !c->_composeShared || !c->_composeTextArea) return;
            c->_composeTextArea->set_rect(c->_composeShared->text_area_rect());
        });
    }
    NSView* composeView = (__bridge NSView*)_composeSurface->view_handle();
    composeView.translatesAutoresizingMaskIntoConstraints = NO;

    _contentStack = [NSStackView stackViewWithViews:@[header, recoveryView,
                                                       msgSurfaceView, composeView]];
    _contentStack.orientation     = NSUserInterfaceLayoutOrientationVertical;
    _contentStack.alignment       = NSLayoutAttributeLeading;
    _contentStack.distribution    = NSStackViewDistributionFill;
    _contentStack.spacing         = 0;
    _contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    [_content addSubview:_contentStack];
    [NSLayoutConstraint activateConstraints:@[
        [_contentStack.topAnchor      constraintEqualToAnchor:_content.topAnchor],
        [_contentStack.leadingAnchor  constraintEqualToAnchor:_content.leadingAnchor],
        [_contentStack.trailingAnchor constraintEqualToAnchor:_content.trailingAnchor],
        [_contentStack.bottomAnchor   constraintEqualToAnchor:_content.bottomAnchor],

        [header.leadingAnchor   constraintEqualToAnchor:_contentStack.leadingAnchor],
        [header.trailingAnchor  constraintEqualToAnchor:_contentStack.trailingAnchor],
        [recoveryView.leadingAnchor  constraintEqualToAnchor:_contentStack.leadingAnchor],
        [recoveryView.trailingAnchor constraintEqualToAnchor:_contentStack.trailingAnchor],
        [recoveryView.heightAnchor   constraintEqualToConstant:48],
        [msgSurfaceView.leadingAnchor  constraintEqualToAnchor:_contentStack.leadingAnchor],
        [msgSurfaceView.trailingAnchor constraintEqualToAnchor:_contentStack.trailingAnchor],
        [composeView.leadingAnchor  constraintEqualToAnchor:_contentStack.leadingAnchor],
        [composeView.trailingAnchor constraintEqualToAnchor:_contentStack.trailingAnchor],
    ]];
    _composeHeightCon = [composeView.heightAnchor
        constraintEqualToConstant:tesseract::views::ComposeBar::kMinHeight];
    _composeHeightCon.active = YES;

    // ── Split view (sidebar | content) ────────────────────────────────
    _splitView = [[NSSplitView alloc] initWithFrame:NSZeroRect];
    _splitView.dividerStyle = NSSplitViewDividerStyleThin;
    _splitView.vertical     = YES;
    _splitView.translatesAutoresizingMaskIntoConstraints = NO;
    [_splitView addArrangedSubview:_sidebar];
    [_splitView addArrangedSubview:_content];
    [_splitView setHoldingPriority:NSLayoutPriorityDefaultHigh
                  forSubviewAtIndex:0];

    [content addSubview:_splitView];
    [NSLayoutConstraint activateConstraints:@[
        [_splitView.topAnchor      constraintEqualToAnchor:content.topAnchor],
        [_splitView.leadingAnchor  constraintEqualToAnchor:content.leadingAnchor],
        [_splitView.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_splitView.bottomAnchor   constraintEqualToAnchor:content.bottomAnchor],
        [_sidebar.widthAnchor      constraintEqualToConstant:
            tesseract::visual::kSidebarWidth],
    ]];

    // ── Login overlay ─────────────────────────────────────────────────
    _loginView = [[LoginView alloc] initWithClient:&_client];
    _loginView.delegate = self;
    _loginView.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:_loginView];
    [NSLayoutConstraint activateConstraints:@[
        [_loginView.topAnchor      constraintEqualToAnchor:content.topAnchor],
        [_loginView.leadingAnchor  constraintEqualToAnchor:content.leadingAnchor],
        [_loginView.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_loginView.bottomAnchor   constraintEqualToAnchor:content.bottomAnchor],
    ]];
    _splitView.hidden = YES;
}

- (void)dealloc {
    [self stopSync];
}

- (void)stopSync {
    _client.stop_sync();
}

- (void)showEmojiPicker:(id)sender {
    if (!_composeSurface) return;
    EmojiPickerPanel* panel = [EmojiPickerPanel sharedPanel];
    panel.client = &_client;
    __weak MainWindowController* weakSelf = self;
    panel.onSelect = ^(NSString* glyph) {
        MainWindowController* s = weakSelf;
        if (!s || glyph.length == 0) return;
        // Reaction mode — "+" chip set _pendingReactionEventId.
        if (!s->_pendingReactionEventId.empty()) {
            std::string ev = std::move(s->_pendingReactionEventId);
            s->_pendingReactionEventId.clear();
            if (!s->_currentRoomId.empty()) {
                s->_client.send_reaction(s->_currentRoomId, ev,
                                          std::string(glyph.UTF8String ?: ""));
            }
            [panel close];
            return;
        }
        if (!s->_composeTextArea) return;
        std::string cur = s->_composeTextArea->text();
        cur += glyph.UTF8String ?: "";
        s->_composeTextArea->set_text(cur);
        if (s->_composeShared) s->_composeShared->set_current_text(cur);
        s->_composeTextArea->set_focused(true);
    };
    NSView* anchor = (__bridge NSView*)_composeSurface->view_handle();
    [panel popupAboveView:anchor];
}

- (void)showEmojiPickerAtRect:(tk::Rect)anchor {
    if (!_msgSurface) return;
    EmojiPickerPanel* panel = [EmojiPickerPanel sharedPanel];
    panel.client = &_client;
    __weak MainWindowController* weakSelf = self;
    panel.onSelect = ^(NSString* glyph) {
        MainWindowController* s = weakSelf;
        if (!s || glyph.length == 0) return;
        if (!s->_pendingReactionEventId.empty()) {
            std::string ev = std::move(s->_pendingReactionEventId);
            s->_pendingReactionEventId.clear();
            if (!s->_currentRoomId.empty()) {
                s->_client.send_reaction(s->_currentRoomId, ev,
                                          std::string(glyph.UTF8String ?: ""));
            }
            [panel close];
            return;
        }
        if (!s->_composeTextArea) return;
        std::string cur = s->_composeTextArea->text();
        cur += glyph.UTF8String ?: "";
        s->_composeTextArea->set_text(cur);
        if (s->_composeShared) s->_composeShared->set_current_text(cur);
        s->_composeTextArea->set_focused(true);
    };
    NSView* anchorView = (__bridge NSView*)_msgSurface->view_handle();
    [panel popupAtRect:anchor inView:anchorView];
}

- (void)_onComposeSend {
    if (_currentRoomId.empty() || !_composeTextArea) return;
    std::string body = trim(_composeTextArea->text());
    if (body.empty()) return;
    auto res = _client.send_message(_currentRoomId, body);
    if (res) {
        _composeTextArea->set_text("");
        if (_composeShared) _composeShared->set_current_text({});
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Login flow
// ─────────────────────────────────────────────────────────────────────────

- (void)beginLogin {
    if (auto saved = tesseract::SessionStore::load()) {
        if (_client.restore_session(*saved)) {
            [self _afterAuthSucceeded];
            return;
        }
    }
    _splitView.hidden = YES;
    _loginView.hidden = NO;
}

- (void)loginViewDidSucceed:(LoginView*)view {
    [self _afterAuthSucceeded];
}

- (void)_afterAuthSucceeded {
    _myUserId = _client.get_user_id();
    _client.start_sync(_bridge.get());
    _splitView.hidden = NO;
    _loginView.hidden = YES;
}

// ─────────────────────────────────────────────────────────────────────────
//  Avatar / media decoding into tk::Image
// ─────────────────────────────────────────────────────────────────────────

- (void)_decodeAndCache:(const std::vector<uint8_t>&)bytes
                  forKey:(const std::string&)key
              destMap:(std::unordered_map<std::string, TkImagePtr>&)dest
                 cap:(int)cap {
    if (bytes.empty() || dest.count(key)) return;
    CFDataRef data = CFDataCreate(kCFAllocatorDefault,
                                    bytes.data(),
                                    static_cast<CFIndex>(bytes.size()));
    if (!data) return;
    CGImageSourceRef src = CGImageSourceCreateWithData(data, nullptr);
    CFRelease(data);
    if (!src) return;
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!img) return;

    // For avatars we keep the cap at the visual::kRoom/MsgAvatarSize;
    // for inline media we use the inline-image / sticker caps. The
    // shared views also clip + fit on paint, so over-large bytes here
    // are merely wasteful, not wrong.
    (void)cap;
    auto wrapper = tk::cg::make_image(img);
    CGImageRelease(img);
    dest.emplace(key, std::move(wrapper));
}

- (void)_ensureRoomAvatar:(const tesseract::RoomInfo&)r {
    if (r.avatar_url.empty() || _tkAvatars.count(r.avatar_url)) return;
    auto bytes = _client.fetch_avatar_bytes(r.id);
    [self _decodeAndCache:bytes
                    forKey:r.avatar_url
                  destMap:_tkAvatars
                       cap:tesseract::visual::kRoomAvatarSize];
}

- (void)_ensureUserAvatar:(const std::string&)mxc {
    if (mxc.empty() || _tkAvatars.count(mxc)) return;
    auto bytes = _client.fetch_media_bytes(mxc);
    [self _decodeAndCache:bytes
                    forKey:mxc
                  destMap:_tkAvatars
                       cap:tesseract::visual::kMsgAvatarSize];
}

- (void)_ensureMediaImage:(const std::string&)url cap:(int)cap {
    if (url.empty() || _tkImages.count(url)) return;
    auto bytes = _client.fetch_media_bytes(url);
    [self _decodeAndCache:bytes
                    forKey:url
                  destMap:_tkImages
                       cap:cap];
}

// ─────────────────────────────────────────────────────────────────────────
//  Event-bridge callbacks (main thread)
// ─────────────────────────────────────────────────────────────────────────

- (void)pushEvent:(tesseract::Event*)ev {
    if (!ev) return;
    std::unique_ptr<tesseract::Event> guard(ev);
    if (ev->room_id != _currentRoomId) return;
    if (ev->type == tesseract::EventType::Unhandled) return;
    [self _appendMessage:*ev];
}

- (void)updateRooms:(std::vector<tesseract::RoomInfo>)rooms {
    _rooms = std::move(rooms);
    [self _refreshRoomList];
    if (!_currentRoomId.empty()) {
        for (const auto& r : _rooms) {
            if (r.id == _currentRoomId) {
                [self _setRoomHeader:r];
                break;
            }
        }
    }
}

- (void)handleSyncErrorContext:(NSString*)ctx
                    description:(NSString*)desc
                    softLogout:(BOOL)soft {
    if ([ctx isEqualToString:@"sync_auth_error"]) {
        if (soft) {
            if (auto saved = tesseract::SessionStore::load()) {
                if (_client.restore_session(*saved)) {
                    _client.start_sync(_bridge.get());
                    return;
                }
            }
        }
        tesseract::SessionStore::clear();
        _client.stop_sync();
        _splitView.hidden = YES;
        _loginView.hidden = NO;
        [_loginView setStatusMessage:@"Session expired; please log in again."];
    }
}

- (void)handleTimelineReset:(NSString*)roomId {
    if (roomId.UTF8String && std::string(roomId.UTF8String) == _currentRoomId) {
        _messageListView->set_messages({});
        _msgSurface->relayout();
    }
}

- (void)handleBackupProgress:(tesseract::BackupProgress)progress {
    [self _maybeShowRecoveryBanner];

    if (_recoverySurface && _recoveryShared
        && !((__bridge NSView*)_recoverySurface->view_handle()).hidden
        && _recoveryShared->state()
            == tesseract::views::RecoveryBanner::State::Importing
        && progress.state == tesseract::BackupState::Downloading
        && progress.imported_keys > 0)
    {
        _recoveryShared->set_import_progress(progress.imported_keys);
        _recoverySurface->relayout();
    }
    if (progress.state == tesseract::BackupState::Enabled
        && !_client.needs_recovery()
        && _recoverySurface)
    {
        ((__bridge NSView*)_recoverySurface->view_handle()).hidden = YES;
    }
}

- (void)_maybeShowRecoveryBanner {
    if (_recoveryDismissed)         return;
    if (!_client.needs_recovery())  return;
    if (!_recoverySurface)          return;
    NSView* view = (__bridge NSView*)_recoverySurface->view_handle();
    if (view.hidden) {
        if (_recoveryShared) {
            _recoveryShared->set_state(
                tesseract::views::RecoveryBanner::State::Form);
            _recoveryShared->set_current_key("");
        }
        if (_recoveryKeyField) {
            _recoveryKeyField->set_text("");
            _recoveryKeyField->set_enabled(true);
        }
        view.hidden = NO;
        _recoverySurface->relayout();
    }
}

- (void)_onRecoveryVerify {
    std::string key;
    if (_recoveryKeyField) key = _recoveryKeyField->text();
    auto a = key.find_first_not_of(" \t\r\n");
    auto b = key.find_last_not_of (" \t\r\n");
    if (a == std::string::npos) {
        if (_recoveryShared) {
            _recoveryShared->set_state(
                tesseract::views::RecoveryBanner::State::Failed);
            _recoveryShared->set_failure_message(
                "Please enter a recovery key or passphrase.");
            if (_recoverySurface) _recoverySurface->relayout();
        }
        return;
    }
    key = key.substr(a, b - a + 1);

    if (_recoveryShared)
        _recoveryShared->set_state(
            tesseract::views::RecoveryBanner::State::Verifying);
    if (_recoveryKeyField) _recoveryKeyField->set_enabled(false);
    if (_recoverySurface)  _recoverySurface->relayout();

    __weak MainWindowController* weakSelf = self;
    std::thread([weakSelf, key]() {
        MainWindowController* strongSelf = weakSelf;
        if (!strongSelf) return;
        auto res = strongSelf->_client.recover(key);
        bool        ok  = res.ok;
        std::string msg = res.message;
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController* s = weakSelf;
            if (!s) return;
            if (ok) {
                if (s->_recoveryShared)
                    s->_recoveryShared->set_state(
                        tesseract::views::RecoveryBanner::State::Importing);
            } else {
                if (s->_recoveryShared) {
                    s->_recoveryShared->set_state(
                        tesseract::views::RecoveryBanner::State::Failed);
                    s->_recoveryShared->set_failure_message(msg);
                }
                if (s->_recoveryKeyField) {
                    s->_recoveryKeyField->set_enabled(true);
                    s->_recoveryKeyField->set_focused(true);
                }
            }
            if (s->_recoverySurface) s->_recoverySurface->relayout();
        });
    }).detach();
}

- (void)_onRecoveryDismiss {
    _recoveryDismissed = YES;
    if (_recoverySurface) {
        ((__bridge NSView*)_recoverySurface->view_handle()).hidden = YES;
    }
}

- (void)_refreshRoomList {
    std::vector<tesseract::RoomInfo> filtered;
    if (_spaceStack.empty()) {
        filtered.reserve(_rooms.size());
        for (const auto& r : _rooms) if (!r.is_space) filtered.push_back(r);
        for (const auto& r : _rooms) if ( r.is_space) filtered.push_back(r);
    } else {
        auto child_ids = _client.space_children(_spaceStack.back());
        for (const auto& r : _rooms) {
            if (std::find(child_ids.begin(), child_ids.end(), r.id)
                != child_ids.end()) {
                filtered.push_back(r);
            }
        }
    }
    for (const auto& r : filtered) [self _ensureRoomAvatar:r];

    _roomListView->set_rooms(filtered);
    if (!_currentRoomId.empty()) {
        _roomListView->set_selected_room(_currentRoomId);
    }
    _roomSurface->relayout();
}

- (void)_setRoomHeader:(const tesseract::RoomInfo&)info {
    _roomTitleLabel.stringValue =
        [NSString stringWithUTF8String:info.name.c_str()] ?: @"";
}

// ─────────────────────────────────────────────────────────────────────────
//  Room + message handling
// ─────────────────────────────────────────────────────────────────────────

- (void)onRoomSelected:(std::string)roomId {
    if (roomId.empty()) return;
    for (const auto& r : _rooms) {
        if (r.id == roomId && r.is_space) {
            _spaceStack.push_back(roomId);
            [self _refreshRoomList];
            return;
        }
    }
    if (!_currentRoomId.empty() && _currentRoomId != roomId) {
        _client.unsubscribe_room(_currentRoomId);
    }
    _currentRoomId = roomId;
    for (const auto& r : _rooms) {
        if (r.id == _currentRoomId) { [self _setRoomHeader:r]; break; }
    }

    auto res = _client.subscribe_room(_currentRoomId);
    if (!res) return;
    _client.paginate_back(_currentRoomId, 50);
    _client.start_background_backfill();
}

- (tesseract::views::MessageRowData)_toRowData:(const tesseract::Event&)ev {
    using Kind = tesseract::views::MessageRowData::Kind;
    tesseract::views::MessageRowData row;
    row.event_id          = ev.event_id;
    row.sender            = ev.sender;
    row.sender_name       = ev.sender_name;
    row.sender_avatar_url = ev.sender_avatar_url;
    row.body              = ev.body;
    row.timestamp_ms      = ev.timestamp;
    row.is_own            = (ev.sender == _myUserId);
    row.reactions         = ev.reactions;

    switch (ev.type) {
        case tesseract::EventType::Text:    row.kind = Kind::Text;    break;
        case tesseract::EventType::Image: {
            row.kind = Kind::Image;
            const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
            row.media_url            = img.image_url;
            row.media_w              = static_cast<int>(img.width);
            row.media_h              = static_cast<int>(img.height);
            row.has_filename_caption = !img.filename.empty();
            break;
        }
        case tesseract::EventType::Sticker: {
            row.kind = Kind::Sticker;
            const auto& s = static_cast<const tesseract::StickerEvent&>(ev);
            row.media_url = s.image_url;
            row.media_w   = static_cast<int>(s.width);
            row.media_h   = static_cast<int>(s.height);
            break;
        }
        case tesseract::EventType::File: {
            row.kind = Kind::File;
            const auto& f = static_cast<const tesseract::FileEvent&>(ev);
            row.file_name = f.file_name;
            row.file_size = f.file_size;
            row.media_url = f.file_url;
            break;
        }
        case tesseract::EventType::Redacted:  row.kind = Kind::Redacted;  break;
        case tesseract::EventType::Unhandled: row.kind = Kind::Unhandled; break;
    }
    return row;
}

- (void)_appendMessage:(const tesseract::Event&)ev {
    [self _ensureUserAvatar:ev.sender_avatar_url];

    if (ev.type == tesseract::EventType::Image) {
        const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
        [self _ensureMediaImage:img.image_url
                            cap:tesseract::visual::kMaxInlineImageWidth];
    } else if (ev.type == tesseract::EventType::Sticker) {
        const auto& s = static_cast<const tesseract::StickerEvent&>(ev);
        [self _ensureMediaImage:s.image_url
                            cap:tesseract::visual::kStickerSize];
    }
    for (const auto& r : ev.reactions) {
        if (!r.source_json.empty()) {
            [self _ensureMediaImage:r.source_json cap:20];
        }
    }

    auto row = [self _toRowData:ev];

    // Live re-emit (reactions, edit, sender-profile resolution): replace
    // the existing row with the same event_id rather than appending.
    auto& msgs = const_cast<std::vector<tesseract::views::MessageRowData>&>(
        _messageListView->messages());
    auto it = std::find_if(msgs.begin(), msgs.end(),
        [&](const tesseract::views::MessageRowData& m) {
            return m.event_id == row.event_id;
        });
    if (it != msgs.end()) {
        *it = std::move(row);
        _messageListView->invalidate_data();
        _msgSurface->relayout();
        return;
    }
    _messageListView->append_message(std::move(row));
    _msgSurface->relayout();
}

@end
