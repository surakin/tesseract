#import "MainWindowController.h"
#import "LoginView.h"
#import "EmojiPicker.h"
#import "StickerPicker.h"
#import "MacOSTrayIcon.h"

#include <tesseract/client.h>
#include <tesseract/event_handler.h>
#include <tesseract/session_store.h>
#include <tesseract/prefs.h>
#include <tesseract/settings.h>
#include <tesseract/visual.h>

#include "tk/canvas_cg.h"
#include "tk/host.h"
#include "tk/host_macos.h"
#include "tk/theme.h"
#include "views/ComposeBar.h"
#include "views/ImageViewerOverlay.h"
#include "views/VideoViewerOverlay.h"
#include "views/MessageListView.h"
#include "views/RecoveryBanner.h"
#include "views/RoomListView.h"

#include <ImageIO/ImageIO.h>
#import <AVFoundation/AVFoundation.h>
#import <UserNotifications/UserNotifications.h>

// Animated WebP properties landed in macOS 11 SDK; define them ourselves
// when building against an older SDK so we can still attempt WebP delay
// extraction at runtime on newer OS versions.
#ifndef kCGImagePropertyWebPDictionary
#define kCGImagePropertyWebPDictionary CFSTR("{WebP}")
#endif
#ifndef kCGImagePropertyWebPDelayTime
#define kCGImagePropertyWebPDelayTime  CFSTR("DelayTime")
#endif

#include <tesseract/account_session.h>

#include "views/AccountPicker.h"
#include "views/UserInfo.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

@class MainWindowController;

// ─────────────────────────────────────────────────────────────────────────
//  EventBridge — IEventHandler that forwards to the controller on the
//  main thread. The SDK calls every method from worker threads; we route
//  through dispatch_async to land mutations on the AppKit run loop.
// ─────────────────────────────────────────────────────────────────────────

namespace {

struct AnimEntry {
    std::vector<std::unique_ptr<tk::Image>> frames;
    std::vector<int>                         delays_ms;
    std::size_t                              current         = 0;
    std::int64_t                             next_advance_ms = 0;
};

class EventBridge final : public tesseract::IEventHandler {
public:
    explicit EventBridge(MainWindowController* controller)
        : controller_(controller) {}

    void on_timeline_reset(const std::string& room_id,
                            std::vector<std::unique_ptr<tesseract::Event>> snapshot) override;
    void on_message_inserted(const std::string& room_id,
                              std::size_t index,
                              std::unique_ptr<tesseract::Event> event) override;
    void on_message_updated(const std::string& room_id,
                             std::size_t index,
                             std::unique_ptr<tesseract::Event> event) override;
    void on_message_removed(const std::string& room_id,
                             std::size_t index) override;
    void on_rooms_updated(const std::vector<tesseract::RoomInfo>& rooms) override;
    void on_sync_error(const std::string& context,
                       const std::string& description,
                       bool soft_logout) override;
    void on_session_saved(const std::string& session_json) override;
    void on_backup_progress(const tesseract::BackupProgress& progress) override;
    void on_image_packs_updated() override;
    void on_account_prefs_updated(const std::string& json) override;
    void on_notification(const std::string& room_id,
                         const std::string& room_name,
                         const std::string& sender,
                         const std::string& body,
                         bool is_mention) override;

public:
    std::string user_id_;
    void set_user_id(const std::string& uid) { user_id_ = uid; }

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

@interface MainWindowController () <LoginViewDelegate, UNUserNotificationCenterDelegate, NSWindowDelegate>
- (void)handleTimelineReset:(NSString*)roomId
                    snapshot:(std::vector<tesseract::Event*>)snapshot;
- (void)handleMessageInserted:(NSString*)roomId
                         index:(std::size_t)index
                         event:(tesseract::Event*)event;
- (void)handleMessageUpdated:(NSString*)roomId
                        index:(std::size_t)index
                        event:(tesseract::Event*)event;
- (void)handleMessageRemoved:(NSString*)roomId
                        index:(std::size_t)index;
- (void)handlePaginateResultForRoom:(std::string)roomId
                      reached_start:(BOOL)reached;
- (void)handleSubscribeResultForRoom:(std::string)roomId reached:(BOOL)reached;
- (void)requestMoreHistoryForRoom:(std::string)roomId;
- (void)updateRoomsForUserId:(std::string)userId rooms:(std::vector<tesseract::RoomInfo>)rooms;
- (void)handleSyncErrorContext:(NSString*)ctx
                    description:(NSString*)desc
                    softLogout:(BOOL)soft;
- (void)_switchActiveAccount:(int)idx;
- (void)_beginAddAccount;
- (void)_logoutActiveAccount;
- (void)loginViewDidCancel:(LoginView*)view;
- (void)_openAccountPicker;
- (void)_onUserStripLeftClick:(NSGestureRecognizer*)gr;
- (void)handleBackupProgress:(tesseract::BackupProgress)progress;

- (void)onRoomSelected:(std::string)roomId;
- (void)_onRecoveryVerify;
- (void)_onRecoveryDismiss;
- (void)_maybeShowRecoveryBanner;
- (void)showEmojiPickerAtRect:(tk::Rect)anchor;
- (void)_sendComposedImage:(std::vector<std::uint8_t>)bytes
                       mime:(std::string)mime
                   filename:(std::string)filename
                    caption:(std::string)caption
               replyEventId:(std::string)reply_event_id;
- (void)_sendComposedFile:(std::vector<std::uint8_t>)bytes
                      mime:(std::string)mime
                  filename:(std::string)filename
                   caption:(std::string)caption
             replyEventId:(std::string)reply_event_id;

// Notifications.
- (void)handleNotification:(std::string)roomId
                  roomName:(std::string)roomName
                    sender:(std::string)sender
                      body:(std::string)body
                    userId:(std::string)userId
                 isMention:(BOOL)isMention;
- (void)_navigateToRoom:(std::string)roomId;

// Sticker picker + animated stickers.
- (void)handleImagePacksUpdated;
- (void)handleAccountPrefsUpdated:(NSString*)json;
- (void)_showStickerPicker;
- (void)_showStickerContextMenuAt:(NSPoint)screenPt;
- (void)_onStickerSave:(id)sender;
- (void)_startAnimTickIfNeeded;
- (void)_animTick:(NSTimer*)timer;
- (void)_ensureStickerImageAsync:(std::string)url;
- (void)_decodeMediaBytes:(const std::vector<uint8_t>&)bytes
                   forKey:(const std::string&)key;
/// Spawn `fn` on a detached worker thread.  No-ops when shutdown is in
/// progress, and the worker itself rechecks the flag before calling
/// `_client.fetch_*`.  Bumps `_workersInFlight` so `stopSync` can wait
/// (bounded) for in-flight workers to drain before the FFI runtime is
/// torn down.  Required: without this, a worker mid-FFI racing against
/// `~ClientFfi` is a data race on `&mut self` in Rust that surfaces as
/// `panic_in_cleanup` through cxx.
- (void)runAsync:(std::function<void()>)fn;
@end

namespace {

void EventBridge::on_timeline_reset(
    const std::string& room_id,
    std::vector<std::unique_ptr<tesseract::Event>> snapshot)
{
    MainWindowController* c = controller_;
    if (!c) return;
    NSString* rid = [NSString stringWithUTF8String:room_id.c_str()] ?: @"";
    // Release ownership of every Event into the block. The main-thread
    // method takes ownership of the raw pointers.
    std::vector<tesseract::Event*> raw;
    raw.reserve(snapshot.size());
    for (auto& p : snapshot) raw.push_back(p.release());
    dispatch_async(dispatch_get_main_queue(), ^{
        [c handleTimelineReset:rid snapshot:raw];
    });
}

void EventBridge::on_message_inserted(
    const std::string& room_id,
    std::size_t index,
    std::unique_ptr<tesseract::Event> event)
{
    MainWindowController* c = controller_;
    if (!c || !event) return;
    NSString*         rid = [NSString stringWithUTF8String:room_id.c_str()] ?: @"";
    tesseract::Event* raw = event.release();
    dispatch_async(dispatch_get_main_queue(), ^{
        [c handleMessageInserted:rid index:index event:raw];
    });
}

void EventBridge::on_message_updated(
    const std::string& room_id,
    std::size_t index,
    std::unique_ptr<tesseract::Event> event)
{
    MainWindowController* c = controller_;
    if (!c || !event) return;
    NSString*         rid = [NSString stringWithUTF8String:room_id.c_str()] ?: @"";
    tesseract::Event* raw = event.release();
    dispatch_async(dispatch_get_main_queue(), ^{
        [c handleMessageUpdated:rid index:index event:raw];
    });
}

void EventBridge::on_message_removed(
    const std::string& room_id,
    std::size_t index)
{
    MainWindowController* c = controller_;
    if (!c) return;
    NSString* rid = [NSString stringWithUTF8String:room_id.c_str()] ?: @"";
    dispatch_async(dispatch_get_main_queue(), ^{
        [c handleMessageRemoved:rid index:index];
    });
}

void EventBridge::on_rooms_updated(
    const std::vector<tesseract::RoomInfo>& rooms)
{
    MainWindowController* c = controller_;
    if (!c) return;
    auto copy = rooms;
    std::string uid = user_id_;
    dispatch_async(dispatch_get_main_queue(), ^{
        [c updateRoomsForUserId:uid rooms:copy];
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

void EventBridge::on_session_saved(const std::string& session_json) {
    if (!user_id_.empty()) {
        tesseract::SessionStore::save_account(user_id_, session_json);
    }
}

void EventBridge::on_backup_progress(const tesseract::BackupProgress& progress) {
    MainWindowController* c = controller_;
    if (!c) return;
    tesseract::BackupProgress copy = progress;
    dispatch_async(dispatch_get_main_queue(), ^{
        [c handleBackupProgress:copy];
    });
}

void EventBridge::on_image_packs_updated() {
    MainWindowController* c = controller_;
    if (!c) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        [c handleImagePacksUpdated];
    });
}

void EventBridge::on_account_prefs_updated(const std::string& json) {
    MainWindowController* c = controller_;
    if (!c) return;
    NSString* ns = [NSString stringWithUTF8String:json.c_str()];
    dispatch_async(dispatch_get_main_queue(), ^{
        [c handleAccountPrefsUpdated:ns];
    });
}

void EventBridge::on_notification(const std::string& room_id,
                                   const std::string& room_name,
                                   const std::string& sender,
                                   const std::string& body,
                                   bool is_mention) {
    MainWindowController* c = controller_;
    if (!c) return;
    std::string rid  = room_id;
    std::string rnam = room_name;
    std::string sndr = sender;
    std::string bd   = body;
    std::string uid  = user_id_;
    BOOL mention     = is_mention;
    dispatch_async(dispatch_get_main_queue(), ^{
        [c handleNotification:rid roomName:rnam sender:sndr body:bd
                     userId:uid isMention:mention];
    });
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────

@implementation MainWindowController {
    std::vector<std::unique_ptr<tesseract::AccountSession>>     _accounts;
    int                                                          _activeAccountIndex;
    tesseract::Client*                                           _client;   // non-owning alias
    EventBridge*                                                 _bridge;   // non-owning alias
    std::unordered_map<std::string, std::vector<tesseract::RoomInfo>> _perAccountRooms;
    std::unique_ptr<tesseract::Client>                           _pendingLoginClient;
    std::filesystem::path                                        _pendingLoginTempDir;
    BOOL                                                         _pendingLoginIsAddAccount;
    int                                                          _addAccountReturnIdx;

    std::vector<tesseract::RoomInfo> _rooms;
    std::string                      _currentRoomId;
    std::string                      _pendingRestoreRoom;
    std::string                      _myUserId;

    // Per-room back-pagination state (mirrors Qt/GTK). `in_flight` gates
    // concurrent paginate_back calls; `reached_start` latches the trigger
    // off when the timeline reports no more history.
    struct PaginationState { bool in_flight = false; bool reached_start = false; };
    std::unordered_map<std::string, PaginationState> _pagination;
    // When non-empty, the next emoji selection routes through
    // send_reaction for this event id (set by the "+" reaction chip).
    std::string                      _pendingReactionEventId;
    std::vector<std::string>         _spaceStack;

    // Shared widget tree.
    std::unique_ptr<tk::macos::Surface>             _roomSurface;
    std::unique_ptr<tk::macos::Surface>             _msgSurface;
    tesseract::views::RoomListView*                 _roomListView;     // borrowed
    std::unique_ptr<tk::NativeTextField>            _roomSearchField;
    std::string                                     _pendingSearchText;
    tesseract::views::MessageListView*              _messageListView;  // borrowed
    std::unordered_map<std::string, TkImagePtr>     _tkAvatars;
    std::unordered_map<std::string, TkImagePtr>     _tkImages;
    // Voice-message source tokens already prefetched (or in flight). The
    // SDK media cache owns the bytes; this set just dedupes worker spawns.
    std::unordered_set<std::string>                 _voicePrefetched;

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

    // Animated sticker cache (GIF / APNG / animated WebP).
    std::unordered_map<std::string, AnimEntry>       _tkAnimImages;
    NSTimer*                                         _animTimer;

    // System-tray icon (menu-bar status item). Created after login; nil
    // until then. When non-nil and `is_available()`, closing the window
    // hides it instead of terminating the app.
    std::unique_ptr<MacOSTrayIcon>                    _tray;

    // Sticker picker async-fetch guard.
    std::set<std::string>                            _stickerFetchesInFlight;

    // Room/user-avatar + inline-media async-fetch guard. The synchronous
    // Rust FFI does a `tokio::block_on` per call; running it on the main
    // queue would freeze the AppKit run loop on accounts with many rooms
    // (one round-trip per avatar). The worker spawns are deduped here.
    std::set<std::string>                            _mediaFetchesInFlight;

    // Replied-to event IDs for which we have already called
    // fetch_reply_details this subscription session. Cleared on room switch.
    std::set<std::string>                            _replyDetailsRequested;

    // Image/sticker lightbox overlay.
    std::unique_ptr<tk::macos::Surface>              _imgViewerSurface;
    tesseract::views::ImageViewerOverlay*            _imgViewer;  // borrowed
    NSView*                                          _imgViewerView;
    id                                               _escapeMonitor;

    // Video lightbox overlay.
    std::unique_ptr<tk::macos::Surface>              _vidViewerSurface;
    tesseract::views::VideoViewerOverlay*            _vidViewer;  // borrowed
    NSView*                                          _vidViewerView;
    NSMutableSet*                                    _videoThumbInFlight;

    // Right-click context menu sticker state.
    std::string                                      _ctxStickerEventId;
    std::string                                      _ctxStickerMxcUrl;
    std::string                                      _ctxStickerBody;

    // Space navigation chrome (top of sidebar, shown when drilling into a space).
    NSView*              _spaceNavBar;
    NSButton*            _spaceBackButton;
    NSTextField*         _spaceNameLabel;
    NSLayoutConstraint*  _spaceNavHeightCon;

    // User identity strip (bottom of sidebar, shown after login).
    NSView*              _userStrip;
    NSImageView*         _userAvatarView;
    NSTextField*         _userNameLabel;
    NSTextField*         _userIdLabel;
    NSLayoutConstraint*  _userStripHeightCon;
    std::string          _myDisplayName;
    std::string          _myAvatarUrl;

    // Account picker popover (left-click on user strip).
    NSPopover*                                        _accountPickerPopover;
    std::unique_ptr<tk::macos::Surface>               _accountPickerSurface;
    tesseract::views::AccountPicker*                  _accountPickerShared;  // borrowed

    // Background-worker coordination — see `runAsync:` for context.
    std::atomic<bool>           _shuttingDown;
    std::mutex                  _workersMu;
    std::condition_variable     _workersCv;
    int                         _workersInFlight;
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

    _videoThumbInFlight = [NSMutableSet set];
    _activeAccountIndex = -1;
    _client = nullptr;
    _bridge = nullptr;
    _addAccountReturnIdx = -1;
    _pendingLoginIsAddAccount = NO;
    _accountPickerShared = nullptr;
    window.delegate = self;
    [self _buildChrome];
    return self;
}

// Intercept the red traffic-light / Cmd-W. If the tray icon is up, hide the
// window instead of closing it; the user can bring it back via the menu-bar
// item. Returns NO to swallow the close.
- (BOOL)windowShouldClose:(NSWindow*)sender {
    if (_tray && _tray->is_available()) {
        [sender orderOut:nil];
        return NO;
    }
    return YES;
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

    // ── Space nav bar (top of sidebar; hidden until drilled into a space) ──
    _spaceNavBar = [[NSView alloc] initWithFrame:NSZeroRect];
    _spaceNavBar.wantsLayer = YES;
    _spaceNavBar.layer.backgroundColor =
        [NSColor colorWithCalibratedRed:0xE8/255.0
                                    green:0xEA/255.0
                                     blue:0xEE/255.0
                                    alpha:1.0].CGColor;
    _spaceBackButton = [NSButton buttonWithTitle:@"←"
                                          target:self
                                          action:@selector(_onSpaceBack)];
    _spaceBackButton.bezelStyle = NSBezelStyleRounded;
    _spaceBackButton.font = [NSFont systemFontOfSize:14];
    _spaceBackButton.translatesAutoresizingMaskIntoConstraints = NO;
    [_spaceNavBar addSubview:_spaceBackButton];
    _spaceNameLabel = [NSTextField labelWithString:@""];
    _spaceNameLabel.font = [NSFont systemFontOfSize:11 weight:NSFontWeightBold];
    _spaceNameLabel.textColor = [NSColor labelColor];
    _spaceNameLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    _spaceNameLabel.translatesAutoresizingMaskIntoConstraints = NO;
    [_spaceNavBar addSubview:_spaceNameLabel];
    [NSLayoutConstraint activateConstraints:@[
        [_spaceBackButton.leadingAnchor  constraintEqualToAnchor:_spaceNavBar.leadingAnchor constant:4],
        [_spaceBackButton.centerYAnchor  constraintEqualToAnchor:_spaceNavBar.centerYAnchor],
        [_spaceBackButton.widthAnchor    constraintEqualToConstant:32],
        [_spaceNameLabel.leadingAnchor   constraintEqualToAnchor:_spaceBackButton.trailingAnchor constant:4],
        [_spaceNameLabel.trailingAnchor  constraintEqualToAnchor:_spaceNavBar.trailingAnchor constant:-4],
        [_spaceNameLabel.centerYAnchor   constraintEqualToAnchor:_spaceNavBar.centerYAnchor],
    ]];
    _spaceNavBar.translatesAutoresizingMaskIntoConstraints = NO;
    _spaceNavHeightCon = [_spaceNavBar.heightAnchor constraintEqualToConstant:0];

    // ── Room list surface ─────────────────────────────────────────────
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

    // Search field — host-overlaid NativeTextField (an NSTextField under
    // the hood) shown only when the list overflows the viewport; the
    // RoomListView itself decides visibility in its arrange() pass.
    _roomSearchField = _roomSurface->host().make_text_field();
    _roomSearchField->set_placeholder("Search rooms");
    _roomSearchField->set_visible(false);
    _roomSearchField->set_on_changed([self](const std::string& q) {
        self->_pendingSearchText = q;
        [NSObject cancelPreviousPerformRequestsWithTarget:self
                  selector:@selector(_applySearchFilter)
                  object:nil];
        [self performSelector:@selector(_applySearchFilter)
                   withObject:nil
                   afterDelay:0.5];
    });
    _roomSurface->set_on_layout([self] {
        if (!_roomListView || !_roomSearchField) return;
        bool visible = _roomListView->search_field_visible();
        _roomSearchField->set_visible(visible);
        if (visible) {
            _roomSearchField->set_rect(
                _roomListView->search_field_rect());
        }
    });

    NSView* roomSurfaceView = (__bridge NSView*)_roomSurface->view_handle();
    roomSurfaceView.translatesAutoresizingMaskIntoConstraints = NO;

    // ── User identity strip (bottom of sidebar; hidden until login) ───
    _userStrip = [[NSView alloc] initWithFrame:NSZeroRect];
    _userStrip.wantsLayer = YES;
    _userStrip.layer.backgroundColor =
        [NSColor colorWithCalibratedRed:0xE8/255.0
                                    green:0xEA/255.0
                                     blue:0xEE/255.0
                                    alpha:1.0].CGColor;
    NSBox* stripSeparator = [[NSBox alloc] init];
    stripSeparator.boxType = NSBoxSeparator;
    stripSeparator.translatesAutoresizingMaskIntoConstraints = NO;
    [_userStrip addSubview:stripSeparator];
    _userAvatarView = [[NSImageView alloc] initWithFrame:NSZeroRect];
    _userAvatarView.imageScaling = NSImageScaleProportionallyUpOrDown;
    _userAvatarView.wantsLayer = YES;
    _userAvatarView.layer.cornerRadius = 16;
    _userAvatarView.layer.masksToBounds = YES;
    _userAvatarView.translatesAutoresizingMaskIntoConstraints = NO;
    [_userStrip addSubview:_userAvatarView];
    _userNameLabel = [NSTextField labelWithString:@""];
    _userNameLabel.font = [NSFont systemFontOfSize:12 weight:NSFontWeightBold];
    _userNameLabel.textColor = [NSColor labelColor];
    _userNameLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    _userNameLabel.translatesAutoresizingMaskIntoConstraints = NO;
    [_userStrip addSubview:_userNameLabel];
    _userIdLabel = [NSTextField labelWithString:@""];
    _userIdLabel.font = [NSFont systemFontOfSize:10];
    _userIdLabel.textColor = [[NSColor labelColor] colorWithAlphaComponent:0.55];
    _userIdLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    _userIdLabel.translatesAutoresizingMaskIntoConstraints = NO;
    [_userStrip addSubview:_userIdLabel];
    [NSLayoutConstraint activateConstraints:@[
        [stripSeparator.topAnchor      constraintEqualToAnchor:_userStrip.topAnchor],
        [stripSeparator.leadingAnchor  constraintEqualToAnchor:_userStrip.leadingAnchor],
        [stripSeparator.trailingAnchor constraintEqualToAnchor:_userStrip.trailingAnchor],
        [_userAvatarView.leadingAnchor  constraintEqualToAnchor:_userStrip.leadingAnchor constant:8],
        [_userAvatarView.centerYAnchor  constraintEqualToAnchor:_userStrip.centerYAnchor],
        [_userAvatarView.widthAnchor    constraintEqualToConstant:32],
        [_userAvatarView.heightAnchor   constraintEqualToConstant:32],
        [_userNameLabel.leadingAnchor   constraintEqualToAnchor:_userAvatarView.trailingAnchor constant:8],
        [_userNameLabel.trailingAnchor  constraintEqualToAnchor:_userStrip.trailingAnchor constant:-8],
        [_userNameLabel.topAnchor       constraintEqualToAnchor:_userStrip.topAnchor constant:10],
        [_userIdLabel.leadingAnchor     constraintEqualToAnchor:_userAvatarView.trailingAnchor constant:8],
        [_userIdLabel.trailingAnchor    constraintEqualToAnchor:_userStrip.trailingAnchor constant:-8],
        [_userIdLabel.topAnchor         constraintEqualToAnchor:_userNameLabel.bottomAnchor constant:1],
    ]];
    _userStrip.translatesAutoresizingMaskIntoConstraints = NO;
    _userStripHeightCon = [_userStrip.heightAnchor constraintEqualToConstant:0];

    // Right-click: account menu. Left-click: account picker (≥2 accounts).
    NSClickGestureRecognizer* stripClick =
        [[NSClickGestureRecognizer alloc] initWithTarget:self
                                                   action:@selector(_onUserStripRightClick:)];
    stripClick.buttonMask = 0x2;  // right mouse button
    [_userStrip addGestureRecognizer:stripClick];
    NSClickGestureRecognizer* stripLeftClick =
        [[NSClickGestureRecognizer alloc] initWithTarget:self
                                                   action:@selector(_onUserStripLeftClick:)];
    stripLeftClick.buttonMask = 0x1;  // left mouse button
    [_userStrip addGestureRecognizer:stripLeftClick];

    // ── Assemble sidebar with the three zones ─────────────────────────
    [_sidebar addSubview:_spaceNavBar];
    [_sidebar addSubview:roomSurfaceView];
    [_sidebar addSubview:_userStrip];
    [NSLayoutConstraint activateConstraints:@[
        // Space nav bar: pinned to top, full width
        [_spaceNavBar.topAnchor      constraintEqualToAnchor:_sidebar.topAnchor],
        [_spaceNavBar.leadingAnchor  constraintEqualToAnchor:_sidebar.leadingAnchor],
        [_spaceNavBar.trailingAnchor constraintEqualToAnchor:_sidebar.trailingAnchor],
        _spaceNavHeightCon,
        // Room surface: fills the middle
        [roomSurfaceView.topAnchor      constraintEqualToAnchor:_spaceNavBar.bottomAnchor],
        [roomSurfaceView.leadingAnchor  constraintEqualToAnchor:_sidebar.leadingAnchor],
        [roomSurfaceView.trailingAnchor constraintEqualToAnchor:_sidebar.trailingAnchor],
        [roomSurfaceView.bottomAnchor   constraintEqualToAnchor:_userStrip.topAnchor],
        // User strip: pinned to bottom, full width
        [_userStrip.bottomAnchor   constraintEqualToAnchor:_sidebar.bottomAnchor],
        [_userStrip.leadingAnchor  constraintEqualToAnchor:_sidebar.leadingAnchor],
        [_userStrip.trailingAnchor constraintEqualToAnchor:_sidebar.trailingAnchor],
        _userStripHeightCon,
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
            auto ait = _tkAnimImages.find(mxc);
            if (ait != _tkAnimImages.end() && !ait->second.frames.empty())
                return ait->second.frames[ait->second.current].get();
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
                s->_client->send_reaction(s->_currentRoomId, event_id, key);
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
        _messageListView->on_reply_requested =
            [weakSelf](const std::string& event_id,
                       const std::string& sender_name,
                       const std::string& body_preview) {
                MainWindowController* s = weakSelf;
                if (!s || !s->_composeShared) return;
                s->_composeShared->set_reply_to(event_id, sender_name,
                                                body_preview);
                if (s->_composeTextArea) s->_composeTextArea->set_focused(true);
            };
        _messageListView->on_edit_requested =
            [weakSelf](const std::string& event_id,
                       const std::string& current_body) {
                MainWindowController* s = weakSelf;
                if (!s || !s->_composeShared) return;
                s->_composeShared->set_editing(event_id);
                if (s->_composeTextArea) {
                    s->_composeTextArea->set_text(current_body);
                    s->_composeShared->set_current_text(current_body);
                    s->_composeTextArea->set_focused(true);
                }
            };
        _messageListView->on_near_top = [weakSelf]{
            MainWindowController* s = weakSelf;
            if (!s) return;
            if (s->_currentRoomId.empty()) return;
            [s requestMoreHistoryForRoom:s->_currentRoomId];
        };
        _messageListView->on_image_clicked =
            [weakSelf](const tesseract::views::MessageListView::ImageHit& hit) {
                MainWindowController* s = weakSelf;
                if (!s || !s->_imgViewer || !s->_imgViewerView) return;
                s->_imgViewer->open(hit.media_url, hit.body,
                                    hit.natural_w, hit.natural_h);
                [s->_imgViewerView setHidden:NO];
                [s->_imgViewerView.window makeFirstResponder:s->_imgViewerView];
            };
        _messageListView->on_video_clicked =
            [weakSelf](const tesseract::views::MessageListView::VideoHit& hit) {
                MainWindowController* s = weakSelf;
                if (!s || !s->_vidViewer || !s->_vidViewerView) return;
                s->_vidViewer->open(hit.source_json, hit.thumbnail_url,
                                    hit.mime_type, hit.duration_ms,
                                    hit.natural_w, hit.natural_h);
                [s->_vidViewerView setHidden:NO];
                [s->_vidViewerView.window makeFirstResponder:s->_vidViewerView];
                // Async byte fetch.
                tesseract::Client* clientPtr = s->_client;
                auto bytes_holder = std::make_shared<std::vector<uint8_t>>();
                std::string src = hit.source_json;
                [s runAsync:[clientPtr, weakSelf, src, bytes_holder]() {
                    *bytes_holder = clientPtr->fetch_source_bytes(src);
                    dispatch_async(dispatch_get_main_queue(), ^{
                        MainWindowController* s2 = weakSelf;
                        if (!s2 || !s2->_vidViewer) return;
                        s2->_vidViewer->load_bytes(bytes_holder->data(),
                                                    bytes_holder->size());
                    });
                }];
            };
    }
    // Voice (MSC3245) playback — AVAudioPlayer-backed tk::AudioPlayer.
    if (auto player = _msgSurface->host().make_audio_player()) {
        _messageListView->set_audio_player(std::move(player));
    }
    _messageListView->set_voice_bytes_provider(
        [self](const std::string& source_json) -> std::vector<std::uint8_t> {
            return _client->fetch_source_bytes(source_json);
        });
    {
        __weak MainWindowController* weakSelf = self;
        _messageListView->set_repaint_requester([weakSelf]() {
            MainWindowController* s = weakSelf;
            if (!s || !s->_msgSurface) return;
            NSView* v = (__bridge NSView*)s->_msgSurface->view_handle();
            [v setNeedsDisplay:YES];
        });
    }
    _msgSurface->set_root(std::move(msg_view));
    {
        __weak MainWindowController* ws = self;
        _msgSurface->set_on_right_click([ws](tk::Point p) {
            MainWindowController* s = ws;
            if (!s || !s->_messageListView) return;
            auto hit = s->_messageListView->sticker_hit_at(p);
            if (!hit) return;
            if (s->_client->user_pack_has_sticker(hit->mxc_url)) return;
            s->_ctxStickerEventId = hit->event_id;
            s->_ctxStickerMxcUrl  = hit->mxc_url;
            s->_ctxStickerBody    = hit->body;
            NSView* view = (__bridge NSView*)s->_msgSurface->view_handle();
            NSPoint local = NSMakePoint(p.x, p.y);
            NSPoint screen = [view.window convertPointToScreen:
                               [view convertPoint:local toView:nil]];
            [s _showStickerContextMenuAt:screen];
        });
    }
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
        _composeShared->on_send = [weakSelf](const std::string& body) {
            MainWindowController* s = weakSelf;
            if (!s || s->_currentRoomId.empty()) return;
            std::string trimmed = trim(body);
            if (trimmed.empty()) return;
            auto res = s->_client->send_message(s->_currentRoomId, trimmed);
            if (res) {
                if (s->_composeTextArea) s->_composeTextArea->set_text("");
                if (s->_composeShared)   s->_composeShared->set_current_text({});
            }
        };
        _composeShared->on_send_image =
            [weakSelf](std::vector<std::uint8_t> bytes,
                        std::string mime,
                        std::string filename,
                        std::string caption,
                        std::uint32_t /*src_w*/, std::uint32_t /*src_h*/,
                        std::string reply_event_id) {
                MainWindowController* s = weakSelf;
                if (!s) return;
                [s _sendComposedImage:std::move(bytes)
                                   mime:std::move(mime)
                                filename:std::move(filename)
                                  caption:std::move(caption)
                           replyEventId:std::move(reply_event_id)];
            };
        _composeShared->on_size_changed = [weakSelf] {
            MainWindowController* c = weakSelf;
            if (!c || !c->_composeShared || !c->_composeHeightCon) return;
            c->_composeHeightCon.constant = c->_composeShared->natural_height();
        };
        _composeShared->on_emoji = [weakSelf] {
            MainWindowController* s = weakSelf;
            if (s) [s showEmojiPicker:nil];
        };
        _composeShared->on_sticker = [weakSelf] {
            MainWindowController* s = weakSelf;
            if (s) [s _showStickerPicker];
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
        _composeTextArea->set_on_image_paste(
            [weakSelf](std::vector<std::uint8_t> bytes, std::string mime) {
                MainWindowController* c = weakSelf;
                if (c && c->_composeShared)
                    c->_composeShared->set_pending_image(std::move(bytes),
                                                          std::move(mime));
            });

        // Drag-and-drop: any file dropped on the message list or
        // composer parks in the compose bar — images use the preview
        // band, everything else uses the file chip.
        auto on_file_drop = [weakSelf](std::vector<std::uint8_t> bytes,
                                        std::string mime,
                                        std::string filename) {
            MainWindowController* c = weakSelf;
            if (!c || !c->_composeShared) return;
            const auto limit = c->_client->media_upload_limit();
            if (limit > 0 && bytes.size() > limit) return;
            if (mime.rfind("image/", 0) == 0) {
                c->_composeShared->set_pending_image(std::move(bytes),
                                                     std::move(mime),
                                                     std::move(filename));
            } else {
                c->_composeShared->set_pending_file(std::move(bytes),
                                                    std::move(mime),
                                                    std::move(filename));
            }
        };
        _composeSurface->set_on_file_drop(on_file_drop);
        if (_msgSurface) _msgSurface->set_on_file_drop(on_file_drop);

        _composeShared->on_send_file =
            [weakSelf](std::vector<std::uint8_t> bytes,
                        std::string mime,
                        std::string filename,
                        std::string caption,
                        std::string reply_event_id) {
                MainWindowController* s = weakSelf;
                if (!s) return;
                [s _sendComposedFile:std::move(bytes)
                                  mime:std::move(mime)
                              filename:std::move(filename)
                                caption:std::move(caption)
                          replyEventId:std::move(reply_event_id)];
            };
        _composeShared->on_send_reply =
            [weakSelf](const std::string& reply_event_id,
                       const std::string& body) {
                MainWindowController* s = weakSelf;
                if (!s || body.empty() || s->_currentRoomId.empty()) return;
                s->_client->send_reply(s->_currentRoomId, reply_event_id, body);
                if (s->_composeTextArea) s->_composeTextArea->set_text("");
                if (s->_composeShared)   s->_composeShared->set_current_text({});
            };
        _composeShared->on_send_edit =
            [weakSelf](const std::string& event_id,
                       const std::string& new_body) {
                MainWindowController* s = weakSelf;
                if (!s || new_body.empty() || s->_currentRoomId.empty()) return;
                s->_client->send_edit(s->_currentRoomId, event_id, new_body);
                if (s->_composeTextArea) s->_composeTextArea->set_text("");
                if (s->_composeShared)   s->_composeShared->set_current_text({});
            };
        _composeShared->on_edit_cancelled = [weakSelf] {
            MainWindowController* s = weakSelf;
            if (!s) return;
            if (s->_composeTextArea) s->_composeTextArea->set_text("");
            if (s->_composeShared)   s->_composeShared->set_current_text({});
        };

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
    _loginView = [[LoginView alloc] init];
    _loginView.delegate = self;
    _loginView.translatesAutoresizingMaskIntoConstraints = NO;
    _loginView.hidden = YES;
    [content addSubview:_loginView];
    [NSLayoutConstraint activateConstraints:@[
        [_loginView.topAnchor      constraintEqualToAnchor:content.topAnchor],
        [_loginView.leadingAnchor  constraintEqualToAnchor:content.leadingAnchor],
        [_loginView.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_loginView.bottomAnchor   constraintEqualToAnchor:content.bottomAnchor],
    ]];
    _splitView.hidden = YES;

    // ── Image / sticker lightbox overlay ─────────────────────────────
    {
        __weak MainWindowController* weakSelf = self;
        _imgViewerSurface = std::make_unique<tk::macos::Surface>(tk::Theme::light());
        auto img_view = std::make_unique<tesseract::views::ImageViewerOverlay>();
        _imgViewer = img_view.get();
        _imgViewer->set_image_provider(
            [weakSelf](const std::string& url) -> const tk::Image* {
                MainWindowController* s = weakSelf;
                if (!s) return nullptr;
                auto ait = s->_tkAnimImages.find(url);
                if (ait != s->_tkAnimImages.end() && !ait->second.frames.empty())
                    return ait->second.frames[ait->second.current].get();
                auto it = s->_tkImages.find(url);
                return it == s->_tkImages.end() ? nullptr : it->second.get();
            });
        _imgViewer->on_close = [weakSelf] {
            MainWindowController* s = weakSelf;
            if (s) [s->_imgViewerView setHidden:YES];
        };
        _imgViewerSurface->set_root(std::move(img_view));

        _imgViewerView = (__bridge NSView*)_imgViewerSurface->view_handle();
        _imgViewerView.translatesAutoresizingMaskIntoConstraints = NO;
        _imgViewerView.hidden = YES;
        [content addSubview:_imgViewerView];
        [NSLayoutConstraint activateConstraints:@[
            [_imgViewerView.topAnchor      constraintEqualToAnchor:content.topAnchor],
            [_imgViewerView.leadingAnchor  constraintEqualToAnchor:content.leadingAnchor],
            [_imgViewerView.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
            [_imgViewerView.bottomAnchor   constraintEqualToAnchor:content.bottomAnchor],
        ]];

        // Escape key monitor: close the overlay when it's open.
        _escapeMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                               handler:^(NSEvent* event) {
            MainWindowController* s = weakSelf;
            if (s && event.keyCode == 53) {  // Escape
                if (s->_vidViewer && s->_vidViewer->is_open()) {
                    s->_vidViewer->close();
                    return (NSEvent*)nil;
                }
                if (s->_imgViewer && s->_imgViewer->is_open()) {
                    s->_imgViewer->close();
                    return (NSEvent*)nil;
                }
            }
            return event;
        }];
    }

    // ── Video lightbox overlay ────────────────────────────────────────
    {
        __weak MainWindowController* weakSelf = self;
        _vidViewerSurface = std::make_unique<tk::macos::Surface>(tk::Theme::light());
        auto vid_view = std::make_unique<tesseract::views::VideoViewerOverlay>();
        _vidViewer = vid_view.get();
        _vidViewer->set_image_provider(
            [weakSelf](const std::string& url) -> const tk::Image* {
                MainWindowController* s = weakSelf;
                if (!s) return nullptr;
                auto it = s->_tkImages.find(url);
                return it == s->_tkImages.end() ? nullptr : it->second.get();
            });
        _vidViewer->set_video_player(_msgSurface->host().make_video_player());
        _vidViewer->set_repaint_requester([weakSelf] {
            MainWindowController* s = weakSelf;
            if (s && s->_vidViewerSurface) s->_vidViewerSurface->relayout();
        });
        _vidViewer->on_close = [weakSelf] {
            MainWindowController* s = weakSelf;
            if (s) [s->_vidViewerView setHidden:YES];
        };
        _vidViewerSurface->set_root(std::move(vid_view));

        _vidViewerView = (__bridge NSView*)_vidViewerSurface->view_handle();
        _vidViewerView.translatesAutoresizingMaskIntoConstraints = NO;
        _vidViewerView.hidden = YES;
        [content addSubview:_vidViewerView];
        [NSLayoutConstraint activateConstraints:@[
            [_vidViewerView.topAnchor      constraintEqualToAnchor:content.topAnchor],
            [_vidViewerView.leadingAnchor  constraintEqualToAnchor:content.leadingAnchor],
            [_vidViewerView.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
            [_vidViewerView.bottomAnchor   constraintEqualToAnchor:content.bottomAnchor],
        ]];
    }
}

- (void)dealloc {
    [self stopSync];
    if (_escapeMonitor) {
        [NSEvent removeMonitor:_escapeMonitor];
        _escapeMonitor = nil;
    }
}

- (void)stopSync {
    // Drain background workers BEFORE tearing the client down.  Each
    // worker calls `_client.fetch_*` (which takes `&mut self` on the
    // Rust side); racing one against `~ClientFfi` is a data race that
    // surfaces as `panic_in_cleanup` through cxx's `prevent_unwind`.
    _shuttingDown.store(true, std::memory_order_release);
    {
        std::unique_lock<std::mutex> lk(_workersMu);
        _workersCv.wait_for(lk, std::chrono::seconds(5),
                             [self]{ return self->_workersInFlight == 0; });
    }
    for (auto& acc : _accounts) {
        if (acc->sync_started) acc->client->stop_sync();
    }
}

- (void)runAsync:(std::function<void()>)fn {
    if (_shuttingDown.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lk(_workersMu);
        ++_workersInFlight;
    }
    __weak MainWindowController* weakSelf = self;
    std::thread([weakSelf, fn = std::move(fn)]() mutable {
        MainWindowController* strong = weakSelf;
        if (strong && !strong->_shuttingDown.load(std::memory_order_acquire)) {
            fn();
        }
        if (strong) {
            std::lock_guard<std::mutex> lk(strong->_workersMu);
            if (--strong->_workersInFlight == 0) strong->_workersCv.notify_all();
        }
    }).detach();
}

- (void)showEmojiPicker:(id)sender {
    if (!_composeSurface) return;
    EmojiPickerPanel* panel = [EmojiPickerPanel sharedPanel];
    panel.client = _client;
    __weak MainWindowController* weakSelf = self;
    panel.onSelect = ^(NSString* glyph) {
        MainWindowController* s = weakSelf;
        if (!s || glyph.length == 0) return;
        // Reaction mode — "+" chip set _pendingReactionEventId.
        if (!s->_pendingReactionEventId.empty()) {
            std::string ev = std::move(s->_pendingReactionEventId);
            s->_pendingReactionEventId.clear();
            if (!s->_currentRoomId.empty()) {
                s->_client->send_reaction(s->_currentRoomId, ev,
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
    panel.client = _client;
    __weak MainWindowController* weakSelf = self;
    panel.onSelect = ^(NSString* glyph) {
        MainWindowController* s = weakSelf;
        if (!s || glyph.length == 0) return;
        if (!s->_pendingReactionEventId.empty()) {
            std::string ev = std::move(s->_pendingReactionEventId);
            s->_pendingReactionEventId.clear();
            if (!s->_currentRoomId.empty()) {
                s->_client->send_reaction(s->_currentRoomId, ev,
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
    if (_composeShared) _composeShared->trigger_send();
}

- (void)_sendComposedImage:(std::vector<std::uint8_t>)bytes
                       mime:(std::string)mime
                   filename:(std::string)filename
                    caption:(std::string)caption
               replyEventId:(std::string)reply_event_id {
    if (_currentRoomId.empty() || !_composeSurface) return;
    const bool compress =
        tesseract::Settings::instance().image_quality
        == tesseract::Settings::ImageQuality::Compressed;
    auto enc = _composeSurface->host().encode_for_send(
        bytes.data(), bytes.size(), compress);
    if (enc.bytes.empty()) return;
    std::string out_name = filename;
    if (enc.mime == "image/jpeg") {
        auto dot = out_name.find_last_of('.');
        if (dot != std::string::npos) out_name = out_name.substr(0, dot);
        out_name += ".jpg";
    }
    auto res = _client->send_image(_currentRoomId, enc.bytes, enc.mime,
                                    out_name, caption,
                                    enc.width, enc.height,
                                    reply_event_id);
    if (res) {
        if (_composeTextArea) _composeTextArea->set_text("");
        if (_composeShared)   _composeShared->set_current_text({});
    }
}

- (void)_sendComposedFile:(std::vector<std::uint8_t>)bytes
                      mime:(std::string)mime
                  filename:(std::string)filename
                   caption:(std::string)caption
             replyEventId:(std::string)reply_event_id {
    if (_currentRoomId.empty()) return;
    auto res = _client->send_file(_currentRoomId, bytes, mime,
                                  filename, caption, reply_event_id);
    if (res) {
        if (_composeTextArea) _composeTextArea->set_text("");
        if (_composeShared)   _composeShared->set_current_text({});
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Login flow
// ─────────────────────────────────────────────────────────────────────────

- (void)beginLogin {
    tesseract::SessionStore::migrate_legacy_layout();

    auto index = tesseract::SessionStore::load_index();
    for (const auto& uid : index.user_ids) {
        auto session_json = tesseract::SessionStore::load_account(uid);
        if (!session_json) continue;

        auto session = std::make_unique<tesseract::AccountSession>();
        session->client = std::make_unique<tesseract::Client>();
        session->client->set_data_dir(
            tesseract::SessionStore::sdk_store_dir(uid).string());
        if (!session->client->restore_session(*session_json)) continue;

        auto* bridge_ptr = new EventBridge(self);
        bridge_ptr->set_user_id(uid);
        session->bridge.reset(bridge_ptr);

        session->user_id      = session->client->get_user_id();
        session->display_name = session->client->get_display_name();
        session->avatar_url   = session->client->get_avatar_url();
        session->last_room    = tesseract::Prefs::parse(
            session->client->load_prefs_json()).last_room;
        session->sync_started = true;
        session->client->start_sync(session->bridge.get());

        session->avatar_disk_cache = std::make_unique<tesseract::AvatarDiskCache>(
            tesseract::SessionStore::account_dir(uid) / "avatars");

        _accounts.push_back(std::move(session));
    }

    if (_accounts.empty()) {
        _pendingLoginClient = std::make_unique<tesseract::Client>();
        auto ms = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        _pendingLoginTempDir = tesseract::SessionStore::account_dir("pending-" + ms);
        _pendingLoginClient->set_data_dir(
            (_pendingLoginTempDir / "matrix-store").string());
        [_loginView setClient:_pendingLoginClient.get()];
        [_loginView setMode:tesseract::views::LoginView::Mode::Initial];
        [_loginView reset];
        _splitView.hidden = YES;
        _loginView.hidden = NO;
        return;
    }

    int firstActive = 0;
    for (int i = 0; i < (int)_accounts.size(); ++i) {
        if (_accounts[i]->user_id == index.active_user_id) {
            firstActive = i;
            break;
        }
    }
    [self _switchActiveAccount:firstActive];
}

- (void)loginViewDidSucceed:(LoginView*)view {
    if (!_pendingLoginClient) return;

    std::string sessionJson = _pendingLoginClient->export_session();
    std::string newUserId   = _pendingLoginClient->get_user_id();

    auto finalDir = tesseract::SessionStore::account_dir(newUserId);
    std::error_code ec;
    std::filesystem::create_directories(finalDir.parent_path(), ec);
    std::filesystem::rename(_pendingLoginTempDir, finalDir, ec);
    if (ec) finalDir = _pendingLoginTempDir;  // EXDEV fallback: keep as-is
    _pendingLoginClient.reset();  // close SQLite handles before reopen
    _pendingLoginTempDir = {};

    auto session = std::make_unique<tesseract::AccountSession>();
    session->client = std::make_unique<tesseract::Client>();
    session->client->set_data_dir(
        tesseract::SessionStore::sdk_store_dir(newUserId).string());
    if (!session->client->restore_session(sessionJson)) return;

    auto* bridge_ptr = new EventBridge(self);
    bridge_ptr->set_user_id(newUserId);
    session->bridge.reset(bridge_ptr);

    session->user_id      = newUserId;
    session->display_name = session->client->get_display_name();
    session->avatar_url   = session->client->get_avatar_url();
    session->last_room    = tesseract::Prefs::parse(
        session->client->load_prefs_json()).last_room;
    session->sync_started = true;
    session->client->start_sync(session->bridge.get());

    session->avatar_disk_cache = std::make_unique<tesseract::AvatarDiskCache>(
        tesseract::SessionStore::account_dir(newUserId) / "avatars");

    tesseract::SessionStore::save_account(newUserId, sessionJson);
    auto idxData = tesseract::SessionStore::load_index();
    auto& ids = idxData.user_ids;
    if (std::find(ids.begin(), ids.end(), newUserId) == ids.end())
        ids.push_back(newUserId);
    idxData.active_user_id = newUserId;
    tesseract::SessionStore::save_index(idxData);

    int newIdx = (int)_accounts.size();
    _accounts.push_back(std::move(session));
    _pendingLoginIsAddAccount = NO;
    _addAccountReturnIdx = -1;

    [self _switchActiveAccount:newIdx];
}

- (void)loginViewDidCancel:(LoginView*)view {
    _pendingLoginClient.reset();
    if (_pendingLoginTempDir != std::filesystem::path()) {
        std::error_code ec;
        std::filesystem::remove_all(_pendingLoginTempDir, ec);
        _pendingLoginTempDir = {};
    }
    _pendingLoginIsAddAccount = NO;
    int returnIdx = _addAccountReturnIdx;
    _addAccountReturnIdx = -1;
    if (returnIdx >= 0 && returnIdx < (int)_accounts.size())
        [self _switchActiveAccount:returnIdx];
}

- (void)_switchActiveAccount:(int)idx {
    _activeAccountIndex = idx;
    auto* session = _accounts[idx].get();
    _client = session->client.get();
    _bridge = static_cast<EventBridge*>(session->bridge.get());

    _myUserId      = session->user_id;
    _myDisplayName = session->display_name;
    _myAvatarUrl   = session->avatar_url;
    _pendingRestoreRoom = session->last_room;

    auto idxData = tesseract::SessionStore::load_index();
    idxData.active_user_id = _myUserId;
    tesseract::SessionStore::save_index(idxData);

    _currentRoomId.clear();
    _spaceStack.clear();

    auto it = _perAccountRooms.find(_myUserId);
    _rooms = (it != _perAccountRooms.end())
        ? it->second : std::vector<tesseract::RoomInfo>{};
    [self _refreshRoomList];
    _messageListView->set_messages({});
    _msgSurface->relayout();

    _splitView.hidden = NO;
    _loginView.hidden = YES;

    [self _populateUserStrip];
    [self _maybeShowRecoveryBanner];

    if (!_pendingRestoreRoom.empty()) {
        for (const auto& r : _rooms) {
            if (r.id == _pendingRestoreRoom && !r.is_space) {
                std::string target = std::move(_pendingRestoreRoom);
                _pendingRestoreRoom.clear();
                [self onRoomSelected:target];
                break;
            }
        }
    }

    if (!_tray) {
        __weak MainWindowController* weakSelf = self;
        _tray = std::make_unique<MacOSTrayIcon>(
            [weakSelf]{
                dispatch_async(dispatch_get_main_queue(), ^{
                    MainWindowController* strong = weakSelf;
                    if (!strong) return;
                    [strong.window makeKeyAndOrderFront:nil];
                    [NSApp activateIgnoringOtherApps:YES];
                });
            },
            []{
                dispatch_async(dispatch_get_main_queue(), ^{
                    [NSApp terminate:nil];
                });
            });
    }

    UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];
    center.delegate = self;
    UNAuthorizationOptions opts = UNAuthorizationOptionAlert
                                | UNAuthorizationOptionSound
                                | UNAuthorizationOptionBadge;
    [center requestAuthorizationWithOptions:opts
                          completionHandler:^(BOOL granted, NSError* err) {
        (void)granted; (void)err;
    }];
}

- (void)_beginAddAccount {
    _pendingLoginIsAddAccount = YES;
    _addAccountReturnIdx = _activeAccountIndex;
    _pendingLoginClient = std::make_unique<tesseract::Client>();
    auto ms = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    _pendingLoginTempDir = tesseract::SessionStore::account_dir("pending-" + ms);
    _pendingLoginClient->set_data_dir(
        (_pendingLoginTempDir / "matrix-store").string());
    [_loginView setClient:_pendingLoginClient.get()];
    [_loginView setMode:tesseract::views::LoginView::Mode::AddAccount];
    [_loginView reset];
    _splitView.hidden = YES;
    _loginView.hidden = NO;
}

- (void)_populateUserStrip {
    NSString* shown = _myDisplayName.empty()
        ? [NSString stringWithUTF8String:_myUserId.c_str()]
        : [NSString stringWithUTF8String:_myDisplayName.c_str()];
    _userNameLabel.stringValue = shown ?: @"";
    _userIdLabel.stringValue = [NSString stringWithUTF8String:_myUserId.c_str()] ?: @"";
    _userStrip.hidden = NO;
    _userStripHeightCon.constant = 48;

    if (!_myAvatarUrl.empty()) {
        std::string url = _myAvatarUrl;
        __weak MainWindowController* weakSelf = self;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            MainWindowController* s = weakSelf;
            if (!s) return;
            auto bytes = s->_client->fetch_media_bytes(url);
            dispatch_async(dispatch_get_main_queue(), ^{
                MainWindowController* s2 = weakSelf;
                if (!s2 || bytes.empty()) return;
                [s2 _setUserAvatarBytes:bytes];
            });
        });
    } else {
        [self _setUserAvatarInitials:shown];
    }
}

- (void)_setUserAvatarBytes:(const std::vector<uint8_t>&)bytes {
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
    NSImage* ns = [[NSImage alloc] initWithCGImage:img size:NSMakeSize(32, 32)];
    CGImageRelease(img);
    _userAvatarView.image = ns;
}

- (void)_setUserAvatarInitials:(NSString*)name {
    NSSize sz = NSMakeSize(32, 32);
    NSImage* img = [[NSImage alloc] initWithSize:sz];
    [img lockFocus];
    NSRect r = NSMakeRect(0, 0, sz.width, sz.height);
    [[NSColor colorWithCalibratedRed:0x6C/255.0
                               green:0x8E/255.0
                                blue:0xBF/255.0 alpha:1.0] setFill];
    [[NSBezierPath bezierPathWithOvalInRect:r] fill];
    NSString* letter = name.length > 0 ? [name substringToIndex:1].uppercaseString : @"?";
    NSDictionary* attrs = @{
        NSFontAttributeName:            [NSFont systemFontOfSize:14 weight:NSFontWeightMedium],
        NSForegroundColorAttributeName: [NSColor whiteColor],
    };
    NSSize ts = [letter sizeWithAttributes:attrs];
    [letter drawAtPoint:NSMakePoint((sz.width - ts.width) * 0.5f,
                                    (sz.height - ts.height) * 0.5f)
         withAttributes:attrs];
    [img unlockFocus];
    _userAvatarView.image = img;
}

- (void)_applySearchFilter {
    if (_roomListView) _roomListView->set_search_text(_pendingSearchText);
}

- (void)_onSpaceBack {
    if (!_spaceStack.empty()) _spaceStack.pop_back();
    [self _refreshRoomList];
}

- (void)_onUserStripRightClick:(NSGestureRecognizer*)gr {
    if (gr.state != NSGestureRecognizerStateEnded) return;
    NSString* logoutTitle = [NSString stringWithFormat:@"Log Out %@",
        _myDisplayName.empty()
            ? [NSString stringWithUTF8String:_myUserId.c_str()]
            : [NSString stringWithUTF8String:_myDisplayName.c_str()]];
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
    [menu addItemWithTitle:@"Add Account…"
                   action:@selector(_beginAddAccount)
            keyEquivalent:@""];
    [menu addItemWithTitle:logoutTitle
                   action:@selector(_logoutActiveAccount)
            keyEquivalent:@""];
    [NSMenu popUpContextMenu:menu withEvent:NSApp.currentEvent forView:_userStrip];
}

- (void)_onUserStripLeftClick:(NSGestureRecognizer*)gr {
    if (gr.state != NSGestureRecognizerStateEnded) return;
    if (_accounts.size() < 2) return;
    [self _openAccountPicker];
}

- (void)_openAccountPicker {
    if (!_accountPickerPopover) {
        _accountPickerPopover = [[NSPopover alloc] init];
        _accountPickerPopover.behavior = NSPopoverBehaviorTransient;
        NSViewController* vc = [[NSViewController alloc] init];
        NSView* container = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 220, 48)];
        vc.view = container;
        _accountPickerPopover.contentViewController = vc;

        _accountPickerSurface = std::make_unique<tk::macos::Surface>(tk::Theme::light());
        auto picker = std::make_unique<tesseract::views::AccountPicker>();
        _accountPickerShared = picker.get();
        __weak MainWindowController* weakSelf = self;
        _accountPickerShared->on_select = [weakSelf](const std::string& uid) {
            MainWindowController* s = weakSelf;
            if (!s) return;
            for (int i = 0; i < (int)s->_accounts.size(); ++i) {
                if (s->_accounts[i]->user_id == uid) {
                    [s->_accountPickerPopover close];
                    [s _switchActiveAccount:i];
                    break;
                }
            }
        };
        _accountPickerShared->set_image_provider(
            [weakSelf](const std::string& mxc) -> const tk::Image* {
                MainWindowController* s = weakSelf;
                if (!s) return nullptr;
                auto it = s->_tkAvatars.find(mxc);
                return it == s->_tkAvatars.end() ? nullptr : it->second.get();
            });
        _accountPickerSurface->set_root(std::move(picker));

        NSView* surfaceView = (__bridge NSView*)_accountPickerSurface->view_handle();
        surfaceView.translatesAutoresizingMaskIntoConstraints = NO;
        [container addSubview:surfaceView];
        [NSLayoutConstraint activateConstraints:@[
            [surfaceView.topAnchor      constraintEqualToAnchor:container.topAnchor],
            [surfaceView.leadingAnchor  constraintEqualToAnchor:container.leadingAnchor],
            [surfaceView.trailingAnchor constraintEqualToAnchor:container.trailingAnchor],
            [surfaceView.bottomAnchor   constraintEqualToAnchor:container.bottomAnchor],
        ]];
    }

    std::vector<tesseract::views::AccountEntry> entries;
    for (int i = 0; i < (int)_accounts.size(); ++i) {
        auto& acc = *_accounts[i];
        tesseract::views::AccountEntry e;
        e.user_id      = acc.user_id;
        e.display_name = acc.display_name;
        e.avatar_url   = acc.avatar_url;
        e.active       = (i == _activeAccountIndex);
        entries.push_back(std::move(e));
        if (!acc.avatar_url.empty()) [self _ensureUserAvatar:acc.avatar_url];
    }
    _accountPickerShared->set_entries(std::move(entries));

    CGFloat rowH = 48.0f;
    NSSize sz = NSMakeSize(220, rowH * (CGFloat)_accounts.size());
    _accountPickerPopover.contentSize = sz;
    _accountPickerSurface->relayout();

    [_accountPickerPopover showRelativeToRect:_userStrip.bounds
                                       ofView:_userStrip
                                preferredEdge:NSRectEdgeMaxY];
}

- (void)_logoutActiveAccount {
    if (_activeAccountIndex < 0 || _activeAccountIndex >= (int)_accounts.size()) return;
    auto* session = _accounts[_activeAccountIndex].get();
    std::string uid = session->user_id;

    if (!_currentRoomId.empty()) {
        _client->unsubscribe_room(_currentRoomId);
        _currentRoomId.clear();
    }
    session->client->logout();
    session->client->stop_sync();
    session->sync_started = false;

    tesseract::SessionStore::clear_account(uid);
    _perAccountRooms.erase(uid);
    _accounts.erase(_accounts.begin() + _activeAccountIndex);

    auto idxData = tesseract::SessionStore::load_index();
    auto& ids = idxData.user_ids;
    ids.erase(std::remove(ids.begin(), ids.end(), uid), ids.end());

    if (_accounts.empty()) {
        _activeAccountIndex = -1;
        _client = nullptr;
        _bridge = nullptr;
        idxData.active_user_id.clear();
        tesseract::SessionStore::save_index(idxData);

        _rooms.clear();
        _spaceStack.clear();
        [self _refreshRoomList];
        _messageListView->set_messages({});
        _msgSurface->relayout();
        _userStrip.hidden = YES;
        _userStripHeightCon.constant = 0;

        _pendingLoginClient = std::make_unique<tesseract::Client>();
        auto ms = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        _pendingLoginTempDir = tesseract::SessionStore::account_dir("pending-" + ms);
        _pendingLoginClient->set_data_dir(
            (_pendingLoginTempDir / "matrix-store").string());
        [_loginView setClient:_pendingLoginClient.get()];
        [_loginView setMode:tesseract::views::LoginView::Mode::Initial];
        [_loginView reset];
        _splitView.hidden = YES;
        _loginView.hidden = NO;
    } else {
        int newIdx = std::min(_activeAccountIndex, (int)_accounts.size() - 1);
        idxData.active_user_id = _accounts[newIdx]->user_id;
        tesseract::SessionStore::save_index(idxData);
        [self _switchActiveAccount:newIdx];
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Notifications (UNUserNotificationCenter)
// ─────────────────────────────────────────────────────────────────────────

- (void)handleNotification:(std::string)roomId
                  roomName:(std::string)roomName
                    sender:(std::string)sender
                      body:(std::string)body
                    userId:(std::string)userId
                 isMention:(BOOL)isMention {
    (void)isMention;
    // Suppress if the window is focused, this account is active, and room is open.
    if (self.window.isKeyWindow
            && _activeAccountIndex >= 0
            && (int)_accounts.size() > _activeAccountIndex
            && _accounts[_activeAccountIndex]->user_id == userId
            && _currentRoomId == roomId) return;

    UNMutableNotificationContent* content =
        [[UNMutableNotificationContent alloc] init];
    content.title = [NSString stringWithUTF8String:sender.c_str()] ?: @"";
    if (roomName != sender) {
        content.subtitle = [NSString stringWithUTF8String:roomName.c_str()] ?: @"";
    }
    std::string preview = body.size() > 120
        ? body.substr(0, 120) + "\xe2\x80\xa6"
        : body;
    content.body             = [NSString stringWithUTF8String:preview.c_str()] ?: @"";
    content.sound            = [UNNotificationSound defaultSound];
    content.threadIdentifier = [NSString stringWithUTF8String:roomId.c_str()] ?: @"";
    NSString* nsRoomId = content.threadIdentifier;
    NSString* nsUserId = [NSString stringWithUTF8String:userId.c_str()] ?: @"";
    content.userInfo = @{ @"room_id": nsRoomId, @"user_id": nsUserId };

    UNNotificationRequest* req =
        [UNNotificationRequest requestWithIdentifier:
            [NSUUID UUID].UUIDString
                                             content:content
                                             trigger:nil];
    [[UNUserNotificationCenter currentNotificationCenter]
        addNotificationRequest:req withCompletionHandler:nil];
}

// Suppress banner when the app is in the foreground and the relevant room
// is already open.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler {
    (void)center;
    NSDictionary* info = notification.request.content.userInfo;
    NSString* rid = info[@"room_id"];
    NSString* uid = info[@"user_id"];
    BOOL activeAccount = uid && _activeAccountIndex >= 0
        && (int)_accounts.size() > _activeAccountIndex
        && _accounts[_activeAccountIndex]->user_id == std::string(uid.UTF8String ?: "");
    if (self.window.isKeyWindow && activeAccount
            && rid && _currentRoomId == std::string(rid.UTF8String ?: "")) {
        completionHandler(UNNotificationPresentationOptionNone);
    } else {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
        completionHandler(UNNotificationPresentationOptionBanner
                        | UNNotificationPresentationOptionSound);
#else
        completionHandler(UNNotificationPresentationOptionAlert
                        | UNNotificationPresentationOptionSound);
#endif
    }
}

// Navigate to the room when the user taps/clicks the notification.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
didReceiveNotificationResponse:(UNNotificationResponse*)response
         withCompletionHandler:(void (^)(void))completionHandler {
    (void)center;
    NSDictionary* info = response.notification.request.content.userInfo;
    NSString* rid = info[@"room_id"];
    NSString* uid = info[@"user_id"];
    // Switch to the account that owns this notification before navigating.
    if (uid) {
        std::string target_uid(uid.UTF8String ?: "");
        for (int i = 0; i < (int)_accounts.size(); ++i) {
            if (_accounts[i]->user_id == target_uid) {
                [self _switchActiveAccount:i];
                break;
            }
        }
    }
    if (rid) {
        [self _navigateToRoom:std::string(rid.UTF8String ?: "")];
    }
    completionHandler();
}

- (void)_navigateToRoom:(std::string)roomId {
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [self onRoomSelected:roomId];
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

// These three helpers used to call the synchronous Rust FFI on the
// main queue. `fetch_avatar_bytes` / `fetch_media_bytes` do a
// `tokio::block_on` inside; on first sync of an account with many rooms
// the room-list paint froze the AppKit run loop for minutes (one
// network round-trip per room avatar, serialised on the main queue).
// Decode + cache now happens after a worker thread lands the bytes
// back via `dispatch_async(main_queue)`; the call sites return
// immediately and the views paint initials placeholders until the
// bytes arrive.
- (void)_ensureRoomAvatar:(const tesseract::RoomInfo&)r {
    if (r.avatar_url.empty() || _tkAvatars.count(r.avatar_url)) return;

    // L1 check: synchronous disk read before touching the network/SDK.
    if (_activeAccountIndex >= 0) {
        auto* cache = _accounts[_activeAccountIndex]->avatar_disk_cache.get();
        if (cache) {
            auto bytes = cache->get(r.avatar_url);
            if (!bytes.empty()) {
                [self _decodeAndCache:bytes
                                forKey:r.avatar_url
                              destMap:_tkAvatars
                                   cap:tesseract::visual::kRoomAvatarSize];
                if (_tkAvatars.count(r.avatar_url)) return;
                // Decode failed: remove the corrupt entry and fall through to fetch.
                cache->remove(r.avatar_url);
            }
        }
    }

    if (!_mediaFetchesInFlight.insert(r.avatar_url).second) return;

    tesseract::Client* clientPtr = _client;
    __weak MainWindowController* weakSelf = self;
    std::string roomId = r.id;
    std::string mxc    = r.avatar_url;
    auto bytes_holder = std::make_shared<std::vector<uint8_t>>();

    [self runAsync:[clientPtr, weakSelf, roomId, mxc, bytes_holder]() {
        *bytes_holder = clientPtr->fetch_avatar_bytes(roomId);
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController* s = weakSelf;
            if (!s) return;
            s->_mediaFetchesInFlight.erase(mxc);
            if (bytes_holder->empty() || s->_tkAvatars.count(mxc)) return;
            // L1 write: persist bytes for future launches.
            if (s->_activeAccountIndex >= 0) {
                if (auto* c = s->_accounts[s->_activeAccountIndex]
                                  ->avatar_disk_cache.get())
                    c->put(mxc, *bytes_holder);
            }
            [s _decodeAndCache:*bytes_holder
                          forKey:mxc
                        destMap:s->_tkAvatars
                             cap:tesseract::visual::kRoomAvatarSize];
            if (s->_roomSurface) s->_roomSurface->relayout();
        });
    }];
}

- (void)_ensureUserAvatar:(const std::string&)mxc {
    if (mxc.empty() || _tkAvatars.count(mxc)) return;

    // L1 check: synchronous disk read before touching the network/SDK.
    if (_activeAccountIndex >= 0) {
        auto* cache = _accounts[_activeAccountIndex]->avatar_disk_cache.get();
        if (cache) {
            auto bytes = cache->get(mxc);
            if (!bytes.empty()) {
                [self _decodeAndCache:bytes
                                forKey:mxc
                              destMap:_tkAvatars
                                   cap:tesseract::visual::kMsgAvatarSize];
                if (_tkAvatars.count(mxc)) return;
                // Decode failed: remove the corrupt entry and fall through to fetch.
                cache->remove(mxc);
            }
        }
    }

    if (!_mediaFetchesInFlight.insert(mxc).second) return;

    tesseract::Client* clientPtr = _client;
    __weak MainWindowController* weakSelf = self;
    std::string key = mxc;
    auto bytes_holder = std::make_shared<std::vector<uint8_t>>();

    [self runAsync:[clientPtr, weakSelf, key, bytes_holder]() {
        *bytes_holder = clientPtr->fetch_media_bytes(key);
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController* s = weakSelf;
            if (!s) return;
            s->_mediaFetchesInFlight.erase(key);
            if (bytes_holder->empty() || s->_tkAvatars.count(key)) return;
            // L1 write: persist bytes for future launches.
            if (s->_activeAccountIndex >= 0) {
                if (auto* c = s->_accounts[s->_activeAccountIndex]
                                  ->avatar_disk_cache.get())
                    c->put(key, *bytes_holder);
            }
            [s _decodeAndCache:*bytes_holder
                          forKey:key
                        destMap:s->_tkAvatars
                             cap:tesseract::visual::kMsgAvatarSize];
            if (s->_msgSurface) s->_msgSurface->relayout();
        });
    }];
}

- (void)_ensureMediaImage:(const std::string&)url cap:(int)cap {
    if (url.empty() || _tkImages.count(url) || _tkAnimImages.count(url)) return;
    if (!_mediaFetchesInFlight.insert(url).second) return;

    tesseract::Client* clientPtr = _client;
    __weak MainWindowController* weakSelf = self;
    std::string key = url;
    auto bytes_holder = std::make_shared<std::vector<uint8_t>>();

    [self runAsync:[clientPtr, weakSelf, key, bytes_holder]() {
        // `key` may be plain mxc (plain images/stickers) or a JSON
        // MediaSource (encrypted images/stickers + reaction sources).
        // `fetch_source_bytes` handles both shapes; `fetch_media_bytes`
        // only handles plain mxc and would return empty for encrypted.
        *bytes_holder = clientPtr->fetch_source_bytes(key);
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController* s = weakSelf;
            if (!s) return;
            s->_mediaFetchesInFlight.erase(key);
            if (bytes_holder->empty()
                || s->_tkImages.count(key)
                || s->_tkAnimImages.count(key)) return;
            [s _decodeMediaBytes:*bytes_holder forKey:key];
            if (s->_msgSurface) s->_msgSurface->relayout();
        });
    }];
}

- (void)_ensureReplyDetails:(const std::string&)eventId {
    if (eventId.empty() || _currentRoomId.empty()) return;
    if (!_replyDetailsRequested.insert(eventId).second) return;
    _client->fetch_reply_details(_currentRoomId, eventId);
}

// ─────────────────────────────────────────────────────────────────────────
//  Event-bridge callbacks (main thread)
// ─────────────────────────────────────────────────────────────────────────

- (void)handleMessageInserted:(NSString*)roomId
                         index:(std::size_t)index
                         event:(tesseract::Event*)event
{
    std::unique_ptr<tesseract::Event> guard(event);
    if (!event) return;
    if (std::string(roomId.UTF8String ?: "") != _currentRoomId) return;
    if (event->type == tesseract::EventType::Unhandled) return;
    [self _ensureRowMedia:*event];
    [self _ensureReplyDetails:event->in_reply_to_id];
    _messageListView->insert_message(index, [self _toRowData:*event]);
    _msgSurface->relayout();
}

- (void)handleMessageUpdated:(NSString*)roomId
                        index:(std::size_t)index
                        event:(tesseract::Event*)event
{
    std::unique_ptr<tesseract::Event> guard(event);
    if (!event) return;
    if (std::string(roomId.UTF8String ?: "") != _currentRoomId) return;
    if (event->type == tesseract::EventType::Unhandled) return;
    [self _ensureRowMedia:*event];
    [self _ensureReplyDetails:event->in_reply_to_id];
    _messageListView->update_message(index, [self _toRowData:*event]);
    _msgSurface->relayout();
}

- (void)handleMessageRemoved:(NSString*)roomId
                        index:(std::size_t)index
{
    if (std::string(roomId.UTF8String ?: "") != _currentRoomId) return;
    _messageListView->remove_message(index);
    _msgSurface->relayout();
}

- (void)updateRoomsForUserId:(std::string)userId rooms:(std::vector<tesseract::RoomInfo>)rooms {
    _perAccountRooms[userId] = rooms;
    if (_activeAccountIndex < 0 || _activeAccountIndex >= (int)_accounts.size()) return;
    if (_accounts[_activeAccountIndex]->user_id != userId) return;
    _rooms = std::move(rooms);
    [self _refreshRoomList];
    if (!_currentRoomId.empty()) {
        for (const auto& r : _rooms) {
            if (r.id == _currentRoomId) {
                [self _setRoomHeader:r];
                break;
            }
        }
    } else if (!_pendingRestoreRoom.empty()) {
        for (const auto& r : _rooms) {
            if (r.id == _pendingRestoreRoom && !r.is_space) {
                std::string target = std::move(_pendingRestoreRoom);
                _pendingRestoreRoom.clear();
                [self onRoomSelected:target];
                break;
            }
        }
    }
}

- (void)handleSyncErrorContext:(NSString*)ctx
                    description:(NSString*)desc
                    softLogout:(BOOL)soft {
    if ([ctx isEqualToString:@"sync_auth_error"]) {
        if (soft && _client && _activeAccountIndex >= 0) {
            std::string uid = _accounts[_activeAccountIndex]->user_id;
            if (auto saved = tesseract::SessionStore::load_account(uid)) {
                if (_client->restore_session(*saved)) {
                    _client->start_sync(_bridge);
                    return;
                }
            }
        }
        [self _logoutActiveAccount];
        [_loginView setStatusMessage:@"Session expired; please log in again."];
    }
}

- (void)handleTimelineReset:(NSString*)roomId
                    snapshot:(std::vector<tesseract::Event*>)snapshot
{
    const bool current =
        roomId.UTF8String && std::string(roomId.UTF8String) == _currentRoomId;
    if (current) {
        std::vector<tesseract::views::MessageRowData> rows;
        rows.reserve(snapshot.size());
        for (auto* ev : snapshot) {
            if (!ev) continue;
            [self _ensureRowMedia:*ev];
            [self _ensureReplyDetails:ev->in_reply_to_id];
            rows.push_back([self _toRowData:*ev]);
        }
        _messageListView->set_messages(std::move(rows));
        _msgSurface->relayout();
    }
    for (auto* ev : snapshot) delete ev;
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
        && !_client->needs_recovery()
        && _recoverySurface)
    {
        ((__bridge NSView*)_recoverySurface->view_handle()).hidden = YES;
    }
}

- (void)_maybeShowRecoveryBanner {
    if (_recoveryDismissed)         return;
    if (!_client->needs_recovery())  return;
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
    [self runAsync:[weakSelf, key]() {
        MainWindowController* strongSelf = weakSelf;
        if (!strongSelf) return;
        auto res = strongSelf->_client->recover(key);
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
    }];
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
        std::unordered_set<std::string> in_space;
        for (const auto& r : _rooms) {
            if (!r.is_space) continue;
            for (const auto& id : _client->space_children(r.id))
                in_space.insert(id);
        }
        filtered.reserve(_rooms.size());
        for (const auto& r : _rooms)
            if (!r.is_space && (!in_space.count(r.id) || r.is_favorite)) filtered.push_back(r);
        for (const auto& r : _rooms)
            if ( r.is_space) filtered.push_back(r);
        _spaceNavBar.hidden = YES;
        _spaceNavHeightCon.constant = 0;
    } else {
        auto child_ids = _client->space_children(_spaceStack.back());
        for (const auto& r : _rooms) {
            if (std::find(child_ids.begin(), child_ids.end(), r.id)
                != child_ids.end()) {
                filtered.push_back(r);
            }
        }
        for (const auto& r : _rooms) {
            if (r.id == _spaceStack.back()) {
                _spaceNameLabel.stringValue =
                    [NSString stringWithUTF8String:r.name.c_str()] ?: @"";
                break;
            }
        }
        _spaceNavBar.hidden = NO;
        _spaceNavHeightCon.constant = 36;
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
        _client->unsubscribe_room(_currentRoomId);
    }
    _currentRoomId = roomId;
    _replyDetailsRequested.clear();
    {
        auto prefs = tesseract::Prefs::parse(_client->load_prefs_json());
        prefs.last_room = roomId;
        _client->save_prefs_json(tesseract::Prefs::serialize(prefs));
    }
    if (_composeShared) {
        _composeShared->clear_reply();
        _composeShared->clear_editing();
    }
    for (const auto& r : _rooms) {
        if (r.id == _currentRoomId) { [self _setRoomHeader:r]; break; }
    }

    // subscribe_room + paginate_back both block inside the Rust runtime;
    // run them on a background queue so the main thread stays responsive.
    std::string subRoom = _currentRoomId;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto res = self->_client->subscribe_room(subRoom);
        BOOL reached = NO;
        if (res) {
            auto pr = self->_client->paginate_back_with_status(subRoom, 50);
            reached = pr.ok && pr.reached_start;
            self->_client->start_background_backfill();
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            [self handleSubscribeResultForRoom:subRoom reached:reached];
        });
    });
}

- (void)requestMoreHistoryForRoom:(std::string)roomId {
    if (roomId.empty()) return;
    auto& state = _pagination[roomId];
    if (state.in_flight || state.reached_start) return;
    state.in_flight = true;

    // Run the blocking paginate call on a background queue; marshal the
    // result back to the main thread.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto pr = self->_client->paginate_back_with_status(roomId, 50);
        BOOL reached = pr.ok && pr.reached_start;
        dispatch_async(dispatch_get_main_queue(), ^{
            [self handlePaginateResultForRoom:roomId reached_start:reached];
        });
    });
}

- (void)handlePaginateResultForRoom:(std::string)roomId
                      reached_start:(BOOL)reached {
    auto it = _pagination.find(roomId);
    if (it == _pagination.end()) return;
    it->second.in_flight     = false;
    it->second.reached_start = reached;
    if (roomId == _currentRoomId && _messageListView)
        _messageListView->reset_near_top_latch();
}

- (void)handleSubscribeResultForRoom:(std::string)roomId reached:(BOOL)reached {
    if (roomId != _currentRoomId) return;
    auto& state = _pagination[roomId];
    state.in_flight     = false;
    state.reached_start = reached;
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
    row.read_receipts     = ev.read_receipts;

    row.in_reply_to_id          = ev.in_reply_to_id;
    row.in_reply_to_sender_name = ev.in_reply_to_sender_name;
    row.in_reply_to_body        = ev.in_reply_to_body;
    row.is_edited               = ev.is_edited;

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
        case tesseract::EventType::Voice: {
            row.kind = Kind::Voice;
            const auto& v = static_cast<const tesseract::VoiceEvent&>(ev);
            row.audio_source = v.audio_source;
            row.audio_mime   = v.mime_type;
            row.duration_ms  = v.duration_ms;
            row.waveform     = v.waveform;
            break;
        }
        case tesseract::EventType::Video: {
            row.kind = Kind::Video;
            const auto& vid = static_cast<const tesseract::VideoEvent&>(ev);
            row.media_url         = vid.video_url;
            row.video_thumb_url   = vid.thumbnail_url.empty()
                                    ? ("thumb::" + ev.event_id)
                                    : vid.thumbnail_url;
            row.media_w           = static_cast<int>(vid.width);
            row.media_h           = static_cast<int>(vid.height);
            row.duration_ms       = vid.duration_ms;
            row.has_filename_caption = !vid.filename.empty();
            break;
        }
        case tesseract::EventType::Redacted:  row.kind = Kind::Redacted;  break;
        case tesseract::EventType::Unhandled: row.kind = Kind::Unhandled; break;
    }
    return row;
}

- (void)_ensureRowMedia:(const tesseract::Event&)ev {
    [self _ensureUserAvatar:ev.sender_avatar_url];
    for (const auto& rr : ev.read_receipts) {
        [self _ensureUserAvatar:rr.avatar_url];
    }

    if (ev.type == tesseract::EventType::Image) {
        const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
        [self _ensureMediaImage:img.image_url
                            cap:tesseract::visual::kMaxInlineImageWidth];
    } else if (ev.type == tesseract::EventType::Sticker) {
        const auto& s = static_cast<const tesseract::StickerEvent&>(ev);
        [self _ensureMediaImage:s.image_url
                            cap:tesseract::visual::kStickerSize];
    } else if (ev.type == tesseract::EventType::Voice) {
        const auto& v = static_cast<const tesseract::VoiceEvent&>(ev);
        if (!v.audio_source.empty() &&
            _voicePrefetched.insert(v.audio_source).second) {
            // Pull the clip into the SDK media cache on a background
            // queue so the first play tap is instant. Bytes are discarded;
            // the next synchronous fetch reads them out of cache.
            std::string src = v.audio_source;
            tesseract::Client* clientPtr = _client;
            [self runAsync:[clientPtr, src]() {
                (void)clientPtr->fetch_source_bytes(src);
            }];
        }
    } else if (ev.type == tesseract::EventType::Video) {
        const auto& vid = static_cast<const tesseract::VideoEvent&>(ev);
        if (!vid.thumbnail_url.empty())
            [self _ensureMediaImage:vid.thumbnail_url
                                cap:tesseract::visual::kMaxInlineImageWidth];
        // Client-side first-frame via AVAssetImageGenerator when no server thumbnail.
        if (vid.thumbnail_url.empty() && !vid.video_url.empty()) {
            NSString* eidStr = [NSString stringWithUTF8String:ev.event_id.c_str()];
            if (![_videoThumbInFlight containsObject:eidStr]) {
                [_videoThumbInFlight addObject:eidStr];
                tesseract::Client* clientPtr = _client;
                __weak MainWindowController* weakSelf = self;
                std::string src = vid.video_url;
                std::string eid = ev.event_id;
                auto bytes_holder = std::make_shared<std::vector<uint8_t>>();
                [self runAsync:[clientPtr, weakSelf, src, eid, bytes_holder]() {
                    *bytes_holder = clientPtr->fetch_source_bytes(src);
                    if (bytes_holder->empty()) return;
                    // Write bytes to a temp file for AVURLAsset.
                    NSString* tmpDir = NSTemporaryDirectory();
                    NSString* eidNS = [NSString stringWithUTF8String:eid.c_str()];
                    NSString* tmpPath = [tmpDir stringByAppendingPathComponent:
                                         [NSString stringWithFormat:@"vtmp_%@.mp4", eidNS]];
                    NSData* data = [NSData dataWithBytes:bytes_holder->data()
                                                  length:bytes_holder->size()];
                    [data writeToFile:tmpPath atomically:YES];
                    NSURL* url = [NSURL fileURLWithPath:tmpPath];
                    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
                    AVAssetImageGenerator* gen =
                        [[AVAssetImageGenerator alloc] initWithAsset:asset];
                    gen.appliesPreferredTrackTransform = YES;
                    CMTime t = CMTimeMake(0, 1);
                    NSError* err = nil;
                    CGImageRef frame = [gen copyCGImageAtTime:t
                                                  actualTime:nil
                                                       error:&err];
                    [[NSFileManager defaultManager] removeItemAtPath:tmpPath error:nil];
                    if (!frame) return;
                    auto img_holder =
                        std::make_shared<std::unique_ptr<tk::Image>>(
                            tk::cg::make_image(frame));
                    CGImageRelease(frame);
                    std::string key = "thumb::" + eid;
                    dispatch_async(dispatch_get_main_queue(), ^{
                        MainWindowController* s = weakSelf;
                        if (!s || s->_tkImages.count(key)) return;
                        s->_tkImages.emplace(key, std::move(*img_holder));
                        if (s->_msgSurface) s->_msgSurface->relayout();
                    });
                }];
            }
        }
    }
    for (const auto& r : ev.reactions) {
        if (!r.source_json.empty()) {
            [self _ensureMediaImage:r.source_json cap:20];
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Animated sticker support
// ─────────────────────────────────────────────────────────────────────────

- (void)_decodeMediaBytes:(const std::vector<uint8_t>&)bytes
                   forKey:(const std::string&)key {
    if (bytes.empty() || _tkImages.count(key) || _tkAnimImages.count(key)) return;
    CFDataRef data = CFDataCreate(kCFAllocatorDefault,
                                   bytes.data(),
                                   static_cast<CFIndex>(bytes.size()));
    if (!data) return;
    CGImageSourceRef src = CGImageSourceCreateWithData(data, nullptr);
    CFRelease(data);
    if (!src) return;

    std::size_t count = CGImageSourceGetCount(src);
    if (count > 1) {
        AnimEntry entry;
        entry.frames.reserve(count);
        entry.delays_ms.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            CGImageRef frame = CGImageSourceCreateImageAtIndex(src, i, nullptr);
            if (!frame) continue;
            entry.frames.push_back(tk::cg::make_image(frame));
            CGImageRelease(frame);
            int delay_ms = 100;
            CFDictionaryRef props =
                CGImageSourceCopyPropertiesAtIndex(src, i, nullptr);
            if (props) {
                auto try_delay = [&](CFStringRef dictKey,
                                     CFStringRef unclampedKey,
                                     CFStringRef clampedKey) {
                    auto* d = (CFDictionaryRef)CFDictionaryGetValue(props, dictKey);
                    if (!d) return;
                    auto* v = (CFNumberRef)CFDictionaryGetValue(d, unclampedKey);
                    if (!v) v = (CFNumberRef)CFDictionaryGetValue(d, clampedKey);
                    if (!v) return;
                    double secs = 0;
                    CFNumberGetValue(v, kCFNumberDoubleType, &secs);
                    if (secs > 0) delay_ms = static_cast<int>(secs * 1000.0);
                };
                try_delay(kCGImagePropertyGIFDictionary,
                          kCGImagePropertyGIFUnclampedDelayTime,
                          kCGImagePropertyGIFDelayTime);
                try_delay(kCGImagePropertyPNGDictionary,
                          kCGImagePropertyAPNGUnclampedDelayTime,
                          kCGImagePropertyAPNGDelayTime);
                if (@available(macOS 11.0, *)) {
                    try_delay(kCGImagePropertyWebPDictionary,
                              kCGImagePropertyWebPDelayTime,
                              kCGImagePropertyWebPDelayTime);
                }
                CFRelease(props);
            }
            entry.delays_ms.push_back(std::max(delay_ms, 20));
        }
        if (!entry.frames.empty()) {
            CFRelease(src);
            entry.current = 0;
            entry.next_advance_ms =
                static_cast<std::int64_t>(
                    [[NSDate date] timeIntervalSince1970] * 1000.0)
                + entry.delays_ms[0];
            _tkAnimImages.emplace(key, std::move(entry));
            [self _startAnimTickIfNeeded];
            return;
        }
        // No valid animated frames — fall through to static decode below.
        CFRelease(src);
        CFDataRef data2 = CFDataCreate(kCFAllocatorDefault,
                                        bytes.data(),
                                        static_cast<CFIndex>(bytes.size()));
        if (!data2) return;
        src = CGImageSourceCreateWithData(data2, nullptr);
        CFRelease(data2);
        if (!src) return;
    }

    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!img) return;
    _tkImages.emplace(key, tk::cg::make_image(img));
    CGImageRelease(img);
}

- (void)_startAnimTickIfNeeded {
    if (_animTimer || _tkAnimImages.empty()) return;
    __weak MainWindowController* weakSelf = self;
    _animTimer = [NSTimer scheduledTimerWithTimeInterval:0.016
                                                 repeats:YES
                                                   block:^(NSTimer* t) {
        [weakSelf _animTick:t];
    }];
    [[NSRunLoop currentRunLoop] addTimer:_animTimer
                                 forMode:NSRunLoopCommonModes];
}

- (void)_animTick:(NSTimer*)timer {
    if (_tkAnimImages.empty()) {
        [_animTimer invalidate];
        _animTimer = nil;
        return;
    }
    const std::int64_t now =
        static_cast<std::int64_t>([[NSDate date] timeIntervalSince1970] * 1000.0);
    bool any_changed = false;
    for (auto& [_, entry] : _tkAnimImages) {
        if (entry.frames.size() <= 1) continue;
        std::size_t steps = 0;
        while (now >= entry.next_advance_ms && steps < entry.frames.size()) {
            entry.current = (entry.current + 1) % entry.frames.size();
            entry.next_advance_ms += entry.delays_ms[entry.current];
            ++steps;
        }
        if (steps > 0) any_changed = true;
    }
    if (any_changed) {
        if (_msgSurface) _msgSurface->relayout();
        StickerPickerPanel* panel = [StickerPickerPanel sharedPanel];
        if (panel.isVisible) [panel invalidateImageCache];
    }
}

- (void)_ensureStickerImageAsync:(std::string)url {
    if (url.empty() || _tkImages.count(url) || _tkAnimImages.count(url)) return;
    if (!_stickerFetchesInFlight.insert(url).second) return;

    tesseract::Client* clientPtr = _client;
    __weak MainWindowController* weakSelf = self;
    auto bytes_holder =
        std::make_shared<std::vector<uint8_t>>();

    [self runAsync:[clientPtr, weakSelf, url, bytes_holder]() {
        *bytes_holder = clientPtr->fetch_source_bytes(url);
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController* s = weakSelf;
            if (!s) return;
            s->_stickerFetchesInFlight.erase(url);
            if (bytes_holder->empty()
                || s->_tkImages.count(url)
                || s->_tkAnimImages.count(url)) return;
            [s _decodeMediaBytes:*bytes_holder forKey:url];
            StickerPickerPanel* panel = [StickerPickerPanel sharedPanel];
            if (panel.isVisible) [panel invalidateImageCache];
        });
    }];
}

// ─────────────────────────────────────────────────────────────────────────
//  Sticker picker
// ─────────────────────────────────────────────────────────────────────────

- (void)handleImagePacksUpdated {
    StickerPickerPanel* panel = [StickerPickerPanel sharedPanel];
    panel.client = _client;
    [panel refreshPacks];
}

- (void)handleAccountPrefsUpdated:(NSString*)json {
    auto prefs = tesseract::Prefs::parse(json.UTF8String);
    if (!prefs.last_room.empty() && _pendingRestoreRoom.empty() && _currentRoomId.empty())
        _pendingRestoreRoom = prefs.last_room;
}

- (void)_showStickerPicker {
    if (!_composeSurface || _currentRoomId.empty()) return;
    StickerPickerPanel* panel = [StickerPickerPanel sharedPanel];
    panel.client = _client;

    __weak MainWindowController* weakSelf = self;

    [panel setImageProvider:
        [weakSelf](const std::string& cache_key,
                   const std::string& /*source_token*/) -> const tk::Image* {
            MainWindowController* s = weakSelf;
            if (!s) return nullptr;
            auto ait = s->_tkAnimImages.find(cache_key);
            if (ait != s->_tkAnimImages.end() && !ait->second.frames.empty())
                return ait->second.frames[ait->second.current].get();
            auto it = s->_tkImages.find(cache_key);
            if (it != s->_tkImages.end()) return it->second.get();
            [s _ensureStickerImageAsync:cache_key];
            return nullptr;
        }];

    panel.onSelected = ^(NSString* url, NSString* body, NSString* infoJson) {
        MainWindowController* s = weakSelf;
        if (!s || s->_currentRoomId.empty()) return;
        std::string u = url.UTF8String      ?: "";
        std::string b = body.UTF8String     ?: "";
        std::string j = infoJson.UTF8String ?: "{}";
        s->_client->send_sticker(s->_currentRoomId, b, u, j);
        [panel orderOut:nil];
    };

    NSView* anchor = (__bridge NSView*)_composeSurface->view_handle();
    [panel popupAboveView:anchor];
}

- (void)_showStickerContextMenuAt:(NSPoint)screenPt {
    if (_ctxStickerMxcUrl.empty()) return;
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Sticker"];
    NSMenuItem* item = [[NSMenuItem alloc]
        initWithTitle:@"Add to Saved Stickers"
               action:@selector(_onStickerSave:)
        keyEquivalent:@""];
    item.target = self;
    [menu addItem:item];
    [menu popUpMenuPositioningItem:nil atLocation:screenPt inView:nil];
}

- (void)_onStickerSave:(id)sender {
    if (_ctxStickerMxcUrl.empty()) return;
    _client->save_sticker_to_user_pack(
        _ctxStickerBody,
        _ctxStickerBody,
        _ctxStickerMxcUrl,
        "{}");
    _ctxStickerEventId.clear();
    _ctxStickerMxcUrl.clear();
    _ctxStickerBody.clear();
}

@end
