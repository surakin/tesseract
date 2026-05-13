#import "MainWindowController.h"
#import "LoginView.h"
#import "EmojiPicker.h"
#import "StickerPicker.h"

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
#include "views/MessageListView.h"
#include "views/RecoveryBanner.h"
#include "views/RoomListView.h"

#include <ImageIO/ImageIO.h>

// Animated WebP properties landed in macOS 11 SDK; define them ourselves
// when building against an older SDK so we can still attempt WebP delay
// extraction at runtime on newer OS versions.
#ifndef kCGImagePropertyWebPDictionary
#define kCGImagePropertyWebPDictionary CFSTR("{WebP}")
#endif
#ifndef kCGImagePropertyWebPDelayTime
#define kCGImagePropertyWebPDelayTime  CFSTR("DelayTime")
#endif

#include <atomic>
#include <cstdint>
#include <memory>
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
- (void)updateRooms:(std::vector<tesseract::RoomInfo>)rooms;
- (void)handleSyncErrorContext:(NSString*)ctx
                    description:(NSString*)desc
                    softLogout:(BOOL)soft;
- (void)handleBackupProgress:(tesseract::BackupProgress)progress;

- (void)onRoomSelected:(std::string)roomId;
- (void)_onRecoveryVerify;
- (void)_onRecoveryDismiss;
- (void)_maybeShowRecoveryBanner;
- (void)showEmojiPickerAtRect:(tk::Rect)anchor;
- (void)_sendComposedImage:(std::vector<std::uint8_t>)bytes
                       mime:(std::string)mime
                   filename:(std::string)filename
                    caption:(std::string)caption;
- (void)_sendComposedFile:(std::vector<std::uint8_t>)bytes
                      mime:(std::string)mime
                  filename:(std::string)filename
                   caption:(std::string)caption;

// Sticker picker + animated stickers.
- (void)handleImagePacksUpdated;
- (void)_showStickerPicker;
- (void)_showStickerContextMenuAt:(NSPoint)screenPt;
- (void)_onStickerSave:(id)sender;
- (void)_startAnimTickIfNeeded;
- (void)_animTick:(NSTimer*)timer;
- (void)_ensureStickerImageAsync:(std::string)url;
- (void)_decodeMediaBytes:(const std::vector<uint8_t>&)bytes
                   forKey:(const std::string&)key;
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

void EventBridge::on_image_packs_updated() {
    MainWindowController* c = controller_;
    if (!c) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        [c handleImagePacksUpdated];
    });
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────

@implementation MainWindowController {
    tesseract::Client                _client;
    std::unique_ptr<EventBridge>     _bridge;
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
    NSLayoutConstraint*  _userStripHeightCon;
    std::string          _myDisplayName;
    std::string          _myAvatarUrl;
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
        [_userNameLabel.centerYAnchor   constraintEqualToAnchor:_userStrip.centerYAnchor],
    ]];
    _userStrip.translatesAutoresizingMaskIntoConstraints = NO;
    _userStripHeightCon = [_userStrip.heightAnchor constraintEqualToConstant:0];

    // Add right-click logout menu to the user strip via a click gesture.
    NSClickGestureRecognizer* stripClick =
        [[NSClickGestureRecognizer alloc] initWithTarget:self
                                                   action:@selector(_onUserStripRightClick:)];
    stripClick.buttonMask = 0x2;  // right mouse button
    [_userStrip addGestureRecognizer:stripClick];

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
    }
    // Voice (MSC3245) playback — AVAudioPlayer-backed tk::AudioPlayer.
    if (auto player = _msgSurface->host().make_audio_player()) {
        _messageListView->set_audio_player(std::move(player));
    }
    _messageListView->set_voice_bytes_provider(
        [self](const std::string& source_json) -> std::vector<std::uint8_t> {
            return _client.fetch_source_bytes(source_json);
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
            if (s->_client.user_pack_has_sticker(hit->mxc_url)) return;
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
            auto res = s->_client.send_message(s->_currentRoomId, trimmed);
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
            const auto limit = c->_client.media_upload_limit();
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
                s->_client.send_reply(s->_currentRoomId, reply_event_id, body);
                if (s->_composeTextArea) s->_composeTextArea->set_text("");
                if (s->_composeShared)   s->_composeShared->set_current_text({});
            };
        _composeShared->on_send_edit =
            [weakSelf](const std::string& event_id,
                       const std::string& new_body) {
                MainWindowController* s = weakSelf;
                if (!s || new_body.empty() || s->_currentRoomId.empty()) return;
                s->_client.send_edit(s->_currentRoomId, event_id, new_body);
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
    _loginView = [[LoginView alloc] initWithClient:&_client];
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
    auto res = _client.send_image(_currentRoomId, enc.bytes, enc.mime,
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
    auto res = _client.send_file(_currentRoomId, bytes, mime,
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
    tesseract::SessionStore::save(_client.export_session());
    [self _afterAuthSucceeded];
}

- (void)_afterAuthSucceeded {
    _myUserId           = _client.get_user_id();
    _myDisplayName      = _client.get_display_name();
    _myAvatarUrl        = _client.get_avatar_url();
    _pendingRestoreRoom = tesseract::Prefs::load_last_room();
    _client.start_sync(_bridge.get());
    _splitView.hidden = NO;
    _loginView.hidden = YES;
    [self _populateUserStrip];
}

- (void)_populateUserStrip {
    NSString* shown = _myDisplayName.empty()
        ? [NSString stringWithUTF8String:_myUserId.c_str()]
        : [NSString stringWithUTF8String:_myDisplayName.c_str()];
    _userNameLabel.stringValue = shown ?: @"";
    _userStrip.hidden = NO;
    _userStripHeightCon.constant = 48;

    if (!_myAvatarUrl.empty()) {
        std::string url = _myAvatarUrl;
        __weak MainWindowController* weakSelf = self;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            MainWindowController* s = weakSelf;
            if (!s) return;
            auto bytes = s->_client.fetch_media_bytes(url);
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
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
    [menu addItemWithTitle:@"Log Out"
                   action:@selector(_doLogout)
            keyEquivalent:@""];
    [NSMenu popUpContextMenu:menu withEvent:NSApp.currentEvent forView:_userStrip];
}

- (void)_doLogout {
    _client.logout();
    tesseract::SessionStore::clear();
    _client.stop_sync();
    if (!_currentRoomId.empty()) _client.unsubscribe_room(_currentRoomId);
    _currentRoomId.clear();
    _myUserId.clear();
    _myDisplayName.clear();
    _myAvatarUrl.clear();
    _rooms.clear();
    _spaceStack.clear();
    [self _refreshRoomList];
    _messageListView->set_messages({});
    _msgSurface->relayout();
    _userStrip.hidden = YES;
    _userStripHeightCon.constant = 0;
    _splitView.hidden = YES;
    _loginView.hidden = NO;
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
    if (!_mediaFetchesInFlight.insert(r.avatar_url).second) return;

    tesseract::Client* clientPtr = &_client;
    __weak MainWindowController* weakSelf = self;
    std::string roomId = r.id;
    std::string mxc    = r.avatar_url;
    auto bytes_holder = std::make_shared<std::vector<uint8_t>>();

    std::thread([clientPtr, weakSelf, roomId, mxc, bytes_holder]() {
        *bytes_holder = clientPtr->fetch_avatar_bytes(roomId);
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController* s = weakSelf;
            if (!s) return;
            s->_mediaFetchesInFlight.erase(mxc);
            if (bytes_holder->empty() || s->_tkAvatars.count(mxc)) return;
            [s _decodeAndCache:*bytes_holder
                          forKey:mxc
                        destMap:s->_tkAvatars
                             cap:tesseract::visual::kRoomAvatarSize];
            if (s->_roomSurface) s->_roomSurface->relayout();
        });
    }).detach();
}

- (void)_ensureUserAvatar:(const std::string&)mxc {
    if (mxc.empty() || _tkAvatars.count(mxc)) return;
    if (!_mediaFetchesInFlight.insert(mxc).second) return;

    tesseract::Client* clientPtr = &_client;
    __weak MainWindowController* weakSelf = self;
    std::string key = mxc;
    auto bytes_holder = std::make_shared<std::vector<uint8_t>>();

    std::thread([clientPtr, weakSelf, key, bytes_holder]() {
        *bytes_holder = clientPtr->fetch_media_bytes(key);
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController* s = weakSelf;
            if (!s) return;
            s->_mediaFetchesInFlight.erase(key);
            if (bytes_holder->empty() || s->_tkAvatars.count(key)) return;
            [s _decodeAndCache:*bytes_holder
                          forKey:key
                        destMap:s->_tkAvatars
                             cap:tesseract::visual::kMsgAvatarSize];
            if (s->_msgSurface) s->_msgSurface->relayout();
        });
    }).detach();
}

- (void)_ensureMediaImage:(const std::string&)url cap:(int)cap {
    if (url.empty() || _tkImages.count(url) || _tkAnimImages.count(url)) return;
    if (!_mediaFetchesInFlight.insert(url).second) return;

    tesseract::Client* clientPtr = &_client;
    __weak MainWindowController* weakSelf = self;
    std::string key = url;
    auto bytes_holder = std::make_shared<std::vector<uint8_t>>();

    std::thread([clientPtr, weakSelf, key, bytes_holder]() {
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
    }).detach();
}

- (void)_ensureReplyDetails:(const std::string&)eventId {
    if (eventId.empty() || _currentRoomId.empty()) return;
    if (!_replyDetailsRequested.insert(eventId).second) return;
    _client.fetch_reply_details(_currentRoomId, eventId);
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
        std::unordered_set<std::string> in_space;
        for (const auto& r : _rooms) {
            if (!r.is_space) continue;
            for (const auto& id : _client.space_children(r.id))
                in_space.insert(id);
        }
        filtered.reserve(_rooms.size());
        for (const auto& r : _rooms)
            if (!r.is_space && !in_space.count(r.id)) filtered.push_back(r);
        for (const auto& r : _rooms)
            if ( r.is_space) filtered.push_back(r);
        _spaceNavBar.hidden = YES;
        _spaceNavHeightCon.constant = 0;
    } else {
        auto child_ids = _client.space_children(_spaceStack.back());
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
        _client.unsubscribe_room(_currentRoomId);
    }
    _currentRoomId = roomId;
    _replyDetailsRequested.clear();
    tesseract::Prefs::save_last_room(roomId);
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
        auto res = self->_client.subscribe_room(subRoom);
        BOOL reached = NO;
        if (res) {
            auto pr = self->_client.paginate_back_with_status(subRoom, 50);
            reached = pr.ok && pr.reached_start;
            self->_client.start_background_backfill();
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
        auto pr = self->_client.paginate_back_with_status(roomId, 50);
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
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                (void)_client.fetch_source_bytes(src);
            });
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

    tesseract::Client* clientPtr = &_client;
    __weak MainWindowController* weakSelf = self;
    auto bytes_holder =
        std::make_shared<std::vector<uint8_t>>();

    std::thread([clientPtr, weakSelf, url, bytes_holder]() {
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
    }).detach();
}

// ─────────────────────────────────────────────────────────────────────────
//  Sticker picker
// ─────────────────────────────────────────────────────────────────────────

- (void)handleImagePacksUpdated {
    StickerPickerPanel* panel = [StickerPickerPanel sharedPanel];
    panel.client = &_client;
    [panel refreshPacks];
}

- (void)_showStickerPicker {
    if (!_composeSurface || _currentRoomId.empty()) return;
    StickerPickerPanel* panel = [StickerPickerPanel sharedPanel];
    panel.client = &_client;

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
        s->_client.send_sticker(s->_currentRoomId, b, u, j);
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
    _client.save_sticker_to_user_pack(
        _ctxStickerBody,
        _ctxStickerBody,
        _ctxStickerMxcUrl,
        "{}");
    _ctxStickerEventId.clear();
    _ctxStickerMxcUrl.clear();
    _ctxStickerBody.clear();
}

@end
