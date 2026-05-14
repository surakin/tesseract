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

#include "tk/anim_image_cache.h"
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
#include "views/VerificationBanner.h"

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

#include "app/ShellBase.h"
#include "app/EventHandlerBase.h"

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
//  MacShell — ShellBase subclass for the macOS AppKit shell.
//  ObjC++ @interface cannot inherit C++ classes, so we use composition:
//  MainWindowController holds a std::unique_ptr<MacShell>.
// ─────────────────────────────────────────────────────────────────────────

namespace {

class MacShell final : public tesseract::ShellBase {
public:
    // ctrl_ is non-owning: MacShell is owned by MainWindowController itself
    // (via _shell), so ctrl_ is always valid for MacShell's lifetime.
    explicit MacShell(MainWindowController* ctrl) : ctrl_(ctrl) {}

protected:
    void post_to_ui_(std::function<void()> fn) override;
    void on_rooms_updated_() override;
    void on_media_bytes_ready_(const std::string& key,
                                ShellBase::MediaKind kind,
                                std::vector<uint8_t> bytes) override;
    void generate_video_thumbnail_(const std::string& event_id,
                                    const std::string& video_url) override;

    void handle_timeline_reset_ui_(std::string room_id,
        std::vector<std::unique_ptr<tesseract::Event>> snapshot) override;
    void handle_message_inserted_ui_(std::string room_id, std::size_t index,
        std::unique_ptr<tesseract::Event> ev) override;
    void handle_message_updated_ui_(std::string room_id, std::size_t index,
        std::unique_ptr<tesseract::Event> ev) override;
    void handle_message_removed_ui_(std::string room_id,
        std::size_t index) override;
    void handle_sync_error_ui_(std::string context, std::string user_id,
        std::string description, bool soft_logout) override;
    void handle_backup_progress_ui_(tesseract::BackupProgress progress) override;
    void handle_image_packs_updated_ui_() override;
    void handle_verification_request_ui_(
        std::string flow_id, std::string user_id,
        std::string device_id, bool incoming) override;
    void handle_sas_ready_ui_(
        std::string flow_id,
        std::vector<tesseract::VerificationEmoji> emojis) override;
    void handle_verification_done_ui_(std::string flow_id) override;
    void handle_verification_cancelled_ui_(
        std::string flow_id, std::string reason) override;
    void handle_verification_state_ui_(bool is_verified) override;
    void handle_account_prefs_updated_ui_(std::string user_id,
        std::string json) override;
    void handle_notification_ui_(std::string user_id, std::string room_id,
        std::string room_name, std::string sender, std::string body,
        bool is_mention, std::vector<uint8_t> avatar_bytes) override;
    void on_room_list_state_ui_() override;
    void update_typing_bar_(const std::string& text, bool visible) override;

    // Expose ShellBase protected members so MainWindowController ObjC++ code
    // can reach them through _shell (composition, not inheritance).
public:
    using ShellBase::client_;
    using ShellBase::compose_typing_active_;
    using ShellBase::handle_compose_text_changed_;
    using ShellBase::handle_compose_room_leaving_;
    using ShellBase::event_handler_;
    using ShellBase::current_room_id_;
    using ShellBase::space_stack_;
    using ShellBase::tk_avatars_;
    using ShellBase::tk_images_;
    using ShellBase::anim_cache_;
    using ShellBase::voice_prefetched_;
    using ShellBase::video_thumb_in_flight_;
    using ShellBase::reply_details_requested_;
    using ShellBase::media_fetches_in_flight_;
    using ShellBase::sticker_fetches_in_flight_;
    using ShellBase::recovery_banner_dismissed_;
    using ShellBase::pagination_;
    using ShellBase::last_room_list_state_;
    using ShellBase::last_backup_state_;
    using ShellBase::last_imported_keys_;
    using ShellBase::sync_progress_shown_;
    using ShellBase::shutting_down_;
    using ShellBase::pending_restore_room_;
    using ShellBase::accounts_;
    using ShellBase::active_account_index_;
    using ShellBase::per_account_rooms_;
    using ShellBase::pending_login_client_;
    using ShellBase::pending_login_temp_dir_;
    using ShellBase::pending_login_is_add_account_;
    using ShellBase::add_account_return_idx_;
    using ShellBase::my_user_id_;
    using ShellBase::my_display_name_;
    using ShellBase::my_avatar_url_;
    using ShellBase::rooms_;
    using ShellBase::workers_mu_;
    using ShellBase::workers_cv_;
    using ShellBase::workers_in_flight_;
    using ShellBase::run_async_;
    using ShellBase::ensure_room_avatar_;
    using ShellBase::ensure_user_avatar_;
    using ShellBase::ensure_media_image_;
    using ShellBase::ensure_reply_details_;
    using ShellBase::ensure_row_media_;
    using ShellBase::push_rooms_;
    using ShellBase::push_paginate_result_;
    using ShellBase::push_room_list_state_;
    using ShellBase::maybe_send_read_receipt_;
    using ShellBase::mark_room_read_;

private:
    MainWindowController* ctrl_;  // non-owning, always valid (owner holds _shell)
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
- (void)handleVerificationState:(BOOL)isVerified;
- (void)handleVerificationRequest:(std::string)flowId incoming:(BOOL)incoming;
- (void)handleSasReady:(std::vector<tesseract::VerificationEmoji>)emojis;
- (void)handleVerificationDone;
- (void)handleVerificationCancelled:(std::string)reason;

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
- (void)_refreshRoomList;
- (void)_setRoomHeader:(const tesseract::RoomInfo&)info;
- (void)_relayoutRoomSurface;
- (void)_relayoutMsgSurface;
- (void)_onRoomListStateChanged;

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
- (void)_updateTypingBar:(NSString*)text;
@end

namespace {

// ── MacShell method implementations ──────────────────────────────────────

void MacShell::post_to_ui_(std::function<void()> fn) {
    auto* heap = new std::function<void()>(std::move(fn));
    dispatch_async(dispatch_get_main_queue(), ^{
        (*heap)();
        delete heap;
    });
}

void MacShell::on_rooms_updated_() {
    MainWindowController* c = ctrl_;
    if (!c) return;
    [c _refreshRoomList];
    if (!current_room_id_.empty()) {
        for (const auto& r : rooms_) {
            if (r.id == current_room_id_) {
                [c _setRoomHeader:r];
                break;
            }
        }
    } else if (!pending_restore_room_.empty()
               && last_room_list_state_ == tesseract::RoomListState::Running) {
        for (const auto& r : rooms_) {
            if (r.id == pending_restore_room_ && !r.is_space) {
                std::string target = std::move(pending_restore_room_);
                pending_restore_room_.clear();
                [c onRoomSelected:target];
                break;
            }
        }
    }
}

void MacShell::on_media_bytes_ready_(const std::string& key,
                                      ShellBase::MediaKind kind,
                                      std::vector<uint8_t> bytes) {
    MainWindowController* c = ctrl_;
    if (!c) return;
    if (kind == MediaKind::MediaImage) {
        [c _decodeMediaBytes:bytes forKey:key];
        [c _relayoutMsgSurface];
        return;
    }
    if (bytes.empty() || tk_avatars_.count(key)) return;
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
    tk_avatars_.emplace(key, tk::cg::make_image(img));
    CGImageRelease(img);
    if (kind == MediaKind::RoomAvatar)
        [c _relayoutRoomSurface];
    else if (kind == MediaKind::UserAvatar)
        [c _relayoutMsgSurface];
}

void MacShell::generate_video_thumbnail_(const std::string& event_id,
                                          const std::string& video_url) {
    MainWindowController* c = ctrl_;
    if (!c) return;
    std::string src = video_url;
    std::string eid = event_id;
    auto bytes_holder = std::make_shared<std::vector<uint8_t>>();
    run_async_([this, src, eid, bytes_holder]() mutable {
        *bytes_holder = client_->fetch_source_bytes(src);
        if (bytes_holder->empty()) return;
        NSString* tmpDir  = NSTemporaryDirectory();
        NSString* eidNS   = [NSString stringWithUTF8String:eid.c_str()];
        NSString* tmpPath = [tmpDir stringByAppendingPathComponent:
                              [NSString stringWithFormat:@"vtmp_%@.mp4", eidNS]];
        NSData* data = [NSData dataWithBytes:bytes_holder->data()
                                      length:bytes_holder->size()];
        [data writeToFile:tmpPath atomically:YES];
        NSURL* url   = [NSURL fileURLWithPath:tmpPath];
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
        AVAssetImageGenerator* gen =
            [[AVAssetImageGenerator alloc] initWithAsset:asset];
        gen.appliesPreferredTrackTransform = YES;
        CMTime t = CMTimeMake(0, 1);
        NSError* err = nil;
        CGImageRef frame = [gen copyCGImageAtTime:t actualTime:nil error:&err];
        [[NSFileManager defaultManager] removeItemAtPath:tmpPath error:nil];
        if (!frame) return;
        auto img_holder = std::make_shared<std::unique_ptr<tk::Image>>(
            tk::cg::make_image(frame));
        CGImageRelease(frame);
        std::string key = "thumb::" + eid;
        post_to_ui_([this, key, img_holder]() mutable {
            if (tk_images_.count(key)) return;
            tk_images_.emplace(key, std::move(*img_holder));
            MainWindowController* c2 = ctrl_;
            if (c2) [c2 _relayoutMsgSurface];
        });
    });
}

void MacShell::handle_timeline_reset_ui_(
    std::string room_id,
    std::vector<std::unique_ptr<tesseract::Event>> snapshot)
{
    MainWindowController* c = ctrl_;
    if (!c) return;
    std::vector<tesseract::Event*> raw;
    raw.reserve(snapshot.size());
    for (auto& p : snapshot) raw.push_back(p.release());
    NSString* rid = [NSString stringWithUTF8String:room_id.c_str()] ?: @"";
    [c handleTimelineReset:rid snapshot:raw];
}

void MacShell::handle_message_inserted_ui_(
    std::string room_id, std::size_t index,
    std::unique_ptr<tesseract::Event> ev)
{
    MainWindowController* c = ctrl_;
    if (!c || !ev) return;
    NSString* rid = [NSString stringWithUTF8String:room_id.c_str()] ?: @"";
    [c handleMessageInserted:rid index:index event:ev.release()];
}

void MacShell::handle_message_updated_ui_(
    std::string room_id, std::size_t index,
    std::unique_ptr<tesseract::Event> ev)
{
    MainWindowController* c = ctrl_;
    if (!c || !ev) return;
    NSString* rid = [NSString stringWithUTF8String:room_id.c_str()] ?: @"";
    [c handleMessageUpdated:rid index:index event:ev.release()];
}

void MacShell::handle_message_removed_ui_(std::string room_id, std::size_t index) {
    MainWindowController* c = ctrl_;
    if (!c) return;
    NSString* rid = [NSString stringWithUTF8String:room_id.c_str()] ?: @"";
    [c handleMessageRemoved:rid index:index];
}

void MacShell::handle_sync_error_ui_(std::string context, std::string /*user_id*/,
                                      std::string description, bool soft_logout) {
    MainWindowController* c = ctrl_;
    if (!c) return;
    NSString* ctx  = [NSString stringWithUTF8String:context.c_str()]     ?: @"";
    NSString* desc = [NSString stringWithUTF8String:description.c_str()] ?: @"";
    [c handleSyncErrorContext:ctx description:desc softLogout:soft_logout];
}

void MacShell::handle_backup_progress_ui_(tesseract::BackupProgress progress) {
    MainWindowController* c = ctrl_;
    if (c) [c handleBackupProgress:progress];
}

void MacShell::handle_image_packs_updated_ui_() {
    MainWindowController* c = ctrl_;
    if (c) [c handleImagePacksUpdated];
}

void MacShell::handle_verification_state_ui_(bool is_verified) {
    MainWindowController* c = ctrl_;
    if (c) [c handleVerificationState:is_verified ? YES : NO];
}

void MacShell::handle_verification_request_ui_(
    std::string flow_id, std::string /*user_id*/,
    std::string /*device_id*/, bool incoming)
{
    active_verification_flow_id_ = std::move(flow_id);
    MainWindowController* c = ctrl_;
    if (c) [c handleVerificationRequest:active_verification_flow_id_
                                incoming:incoming ? YES : NO];
}

void MacShell::handle_sas_ready_ui_(
    std::string /*flow_id*/, std::vector<tesseract::VerificationEmoji> emojis)
{
    MainWindowController* c = ctrl_;
    if (c) [c handleSasReady:std::move(emojis)];
}

void MacShell::handle_verification_done_ui_(std::string /*flow_id*/) {
    MainWindowController* c = ctrl_;
    if (c) [c handleVerificationDone];
}

void MacShell::handle_verification_cancelled_ui_(
    std::string /*flow_id*/, std::string reason)
{
    MainWindowController* c = ctrl_;
    if (c) [c handleVerificationCancelled:std::move(reason)];
}

void MacShell::handle_account_prefs_updated_ui_(std::string /*user_id*/,
                                                  std::string json) {
    MainWindowController* c = ctrl_;
    if (!c) return;
    NSString* ns = [NSString stringWithUTF8String:json.c_str()];
    [c handleAccountPrefsUpdated:ns];
}

void MacShell::handle_notification_ui_(std::string user_id, std::string room_id,
                                        std::string room_name, std::string sender,
                                        std::string body, bool is_mention,
                                        std::vector<uint8_t> /*avatar_bytes*/) {
    MainWindowController* c = ctrl_;
    if (c) [c handleNotification:room_id roomName:room_name sender:sender
                            body:body userId:user_id isMention:is_mention];
}

void MacShell::on_room_list_state_ui_() {
    MainWindowController* c = ctrl_;
    if (c) [c _onRoomListStateChanged];
}

void MacShell::update_typing_bar_(const std::string& text, bool visible) {
    MainWindowController* c = ctrl_;
    if (!c) return;
    NSString* ns = [NSString stringWithUTF8String:text.c_str()] ?: @"";
    [c _updateTypingBar:ns];
    _typingBar.hidden = !visible;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────

@implementation MainWindowController {
    // MacShell owns all multi-account state, image caches, worker threads,
    // and the EventHandlerBase bridges. It is created before _buildChrome.
    std::unique_ptr<MacShell>                        _shell;

    // When non-empty, the next emoji selection routes through
    // send_reaction for this event id (set by the "+" reaction chip).
    std::string                      _pendingReactionEventId;

    // Shared widget tree.
    std::unique_ptr<tk::macos::Surface>             _roomSurface;
    std::unique_ptr<tk::macos::Surface>             _msgSurface;
    tesseract::views::RoomListView*                 _roomListView;     // borrowed
    std::unique_ptr<tk::NativeTextField>            _roomSearchField;
    std::string                                     _pendingSearchText;
    tesseract::views::MessageListView*              _messageListView;  // borrowed

    // AppKit chrome.
    NSSplitView*   _splitView;
    NSView*        _sidebar;
    NSView*        _content;
    NSTextField*   _roomTitleLabel;
    LoginView*     _loginView;
    NSStackView*   _contentStack;

    NSTextField*   _typingBar;

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

    // Verification banner — shared widget on a tk::macos::Surface.
    std::unique_ptr<tk::macos::Surface>             _verifSurface;
    tesseract::views::VerificationBanner*           _verifShared;  // borrowed
    NSLayoutConstraint*                              _verifHeightCon;

    NSTimer*                                         _animTimer;

    // System-tray icon (menu-bar status item). Created after login; nil
    // until then. When non-nil and `is_available()`, closing the window
    // hides it instead of terminating the app.
    std::unique_ptr<MacOSTrayIcon>                    _tray;

    // Image/sticker lightbox overlay.
    std::unique_ptr<tk::macos::Surface>              _imgViewerSurface;
    tesseract::views::ImageViewerOverlay*            _imgViewer;  // borrowed
    NSView*                                          _imgViewerView;
    id                                               _escapeMonitor;

    // Video lightbox overlay.
    std::unique_ptr<tk::macos::Surface>              _vidViewerSurface;
    tesseract::views::VideoViewerOverlay*            _vidViewer;  // borrowed
    NSView*                                          _vidViewerView;
    // macOS-specific: NSMutableSet used by _videoThumbInFlight check.
    // ShellBase owns video_thumb_in_flight_ (C++ unordered_set); this ObjC
    // set is no longer used — the C++ set in ShellBase deduplicates.

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

    // Account picker popover (left-click on user strip).
    NSPopover*                                        _accountPickerPopover;
    std::unique_ptr<tk::macos::Surface>               _accountPickerSurface;
    tesseract::views::AccountPicker*                  _accountPickerShared;  // borrowed
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

    _shell = std::make_unique<MacShell>(self);
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
            auto it = _shell->tk_avatars_.find(mxc);
            return it == _shell->tk_avatars_.end() ? nullptr : it->second.get();
        });
    _roomListView->on_room_selected =
        [self](const std::string& room_id) { [self onRoomSelected:room_id]; };
    _roomListView->on_scroll = [self] {
        [NSObject cancelPreviousPerformRequestsWithTarget:self
                  selector:@selector(_onRoomScrollDebounce)
                  object:nil];
        [self performSelector:@selector(_onRoomScrollDebounce)
                   withObject:nil
                   afterDelay:0.3];
    };
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
            auto it = _shell->tk_avatars_.find(mxc);
            return it == _shell->tk_avatars_.end() ? nullptr : it->second.get();
        });
    _messageListView->set_image_provider(
        [self](const std::string& mxc) -> const tk::Image* {
            if (auto* f = _shell->anim_cache_.current_frame(mxc)) return f;
            auto it = _shell->tk_images_.find(mxc);
            return it == _shell->tk_images_.end() ? nullptr : it->second.get();
        });
    {
        __weak MainWindowController* weakSelf = self;
        _messageListView->on_reaction_toggled =
            [weakSelf](const std::string& event_id, const std::string& key) {
                MainWindowController* s = weakSelf;
                if (!s) return;
                if (s->_shell->current_room_id_.empty()) return;
                s->_shell->client_->send_reaction(s->_shell->current_room_id_, event_id, key);
            };
        _messageListView->on_add_reaction_requested =
            [weakSelf](const std::string& event_id, tk::Rect anchor) {
                MainWindowController* s = weakSelf;
                if (!s) return;
                if (s->_shell->current_room_id_.empty()) return;
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
            if (s->_shell->current_room_id_.empty()) return;
            [s requestMoreHistoryForRoom:s->_shell->current_room_id_];
        };
        _messageListView->on_receipt_needed = [weakSelf](const std::string& eid) {
            MainWindowController* s = weakSelf;
            if (!s) return;
            s->_shell->maybe_send_read_receipt_(s->_shell->current_room_id_, eid);
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
                                    hit.natural_w, hit.natural_h,
                                    hit.autoplay, hit.loop,
                                    hit.no_audio, hit.hide_controls);
                [s->_vidViewerView setHidden:NO];
                [s->_vidViewerView.window makeFirstResponder:s->_vidViewerView];
                // Async byte fetch.
                auto bytes_holder = std::make_shared<std::vector<uint8_t>>();
                std::string src = hit.source_json;
                s->_shell->run_async_([weakSelf, src, bytes_holder,
                                       clientPtr = s->_shell->client_]() {
                    *bytes_holder = clientPtr->fetch_source_bytes(src);
                    dispatch_async(dispatch_get_main_queue(), ^{
                        MainWindowController* s2 = weakSelf;
                        if (!s2 || !s2->_vidViewer) return;
                        s2->_vidViewer->load_bytes(bytes_holder->data(),
                                                    bytes_holder->size());
                    });
                });
            };
        __weak MainWindowController* wkSelf = self;
        _messageListView->set_video_player_factory(
            [wkSelf]() -> std::unique_ptr<tk::VideoPlayer> {
                MainWindowController* s = wkSelf;
                if (!s || !s->_msgSurface) return nullptr;
                return s->_msgSurface->host().make_video_player();
            });
        _messageListView->set_video_fetch_provider(
            [wkSelf](const std::string& src,
                     std::function<void(std::vector<std::uint8_t>)> on_ready) {
                MainWindowController* s = wkSelf;
                if (!s) return;
                auto bytes_holder = std::make_shared<std::vector<uint8_t>>();
                s->_shell->run_async_([wkSelf, src, bytes_holder,
                                       on_ready = std::move(on_ready),
                                       clientPtr = s->_shell->client_]() mutable {
                    *bytes_holder = clientPtr->fetch_source_bytes(src);
                    dispatch_async(dispatch_get_main_queue(), ^{
                        on_ready(std::move(*bytes_holder));
                    });
                });
            });
    }
    // Voice (MSC3245) playback — AVAudioPlayer-backed tk::AudioPlayer.
    if (auto player = _msgSurface->host().make_audio_player()) {
        _messageListView->set_audio_player(std::move(player));
    }
    _messageListView->set_voice_bytes_provider(
        [self](const std::string& source_json) -> std::vector<std::uint8_t> {
            return _shell->client_->fetch_source_bytes(source_json);
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
            if (s->_shell->client_->user_pack_has_sticker(hit->mxc_url)) return;
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

    // Verification banner — shared widget on a tk::macos::Surface. Initially
    // hidden; shown by handleVerificationState: when isVerified=NO.
    _verifSurface = std::make_unique<tk::macos::Surface>(tk::Theme::light());
    {
        auto banner = std::make_unique<tesseract::views::VerificationBanner>();
        _verifShared = banner.get();
        __weak MainWindowController* weakSelf = self;
        _verifShared->on_verify = [weakSelf] {
            MainWindowController* s = weakSelf;
            if (s && s->_shell->client_)
                s->_shell->client_->request_self_verification();
        };
        _verifShared->on_accept = [weakSelf] {
            MainWindowController* s = weakSelf;
            if (!s || !s->_shell->client_) return;
            s->_shell->client_->accept_verification(s->_shell->active_verification_flow_id_);
            s->_shell->client_->start_sas(s->_shell->active_verification_flow_id_);
        };
        _verifShared->on_match = [weakSelf] {
            MainWindowController* s = weakSelf;
            if (!s || !s->_shell->client_) return;
            s->_shell->client_->confirm_sas(s->_shell->active_verification_flow_id_);
            if (s->_verifShared) s->_verifShared->set_state(
                tesseract::views::VerificationBanner::State::Confirming);
            if (s->_verifSurface) s->_verifSurface->relayout();
        };
        _verifShared->on_mismatch = [weakSelf] {
            MainWindowController* s = weakSelf;
            if (s && s->_shell->client_)
                s->_shell->client_->cancel_verification(s->_shell->active_verification_flow_id_);
        };
        _verifShared->on_cancel = [weakSelf] {
            MainWindowController* s = weakSelf;
            if (s && s->_shell->client_)
                s->_shell->client_->cancel_verification(s->_shell->active_verification_flow_id_);
        };
        _verifShared->on_dismiss = [weakSelf] {
            MainWindowController* s = weakSelf;
            if (!s) return;
            s->_shell->verification_banner_dismissed_ = true;
            NSView* v = (__bridge NSView*)s->_verifSurface->view_handle();
            v.hidden = YES;
            s->_verifHeightCon.constant = 0;
        };
        _verifShared->on_done = [weakSelf] {
            MainWindowController* s = weakSelf;
            if (!s) return;
            NSView* v = (__bridge NSView*)s->_verifSurface->view_handle();
            v.hidden = YES;
            s->_verifHeightCon.constant = 0;
        };
        _verifSurface->set_root(std::move(banner));
    }
    NSView* verifView = (__bridge NSView*)_verifSurface->view_handle();
    verifView.translatesAutoresizingMaskIntoConstraints = NO;
    verifView.hidden = YES;

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
            if (!s || s->_shell->current_room_id_.empty()) return;
            std::string trimmed = trim(body);
            if (trimmed.empty()) return;
            auto res = s->_shell->client_->send_message(s->_shell->current_room_id_, trimmed);
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
            if (c) c->_shell->handle_compose_text_changed_(s);
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
            const auto limit = c->_shell->client_->media_upload_limit();
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
                if (!s || body.empty() || s->_shell->current_room_id_.empty()) return;
                s->_shell->client_->send_reply(s->_shell->current_room_id_, reply_event_id, body);
                if (s->_composeTextArea) s->_composeTextArea->set_text("");
                if (s->_composeShared)   s->_composeShared->set_current_text({});
            };
        _composeShared->on_send_edit =
            [weakSelf](const std::string& event_id,
                       const std::string& new_body) {
                MainWindowController* s = weakSelf;
                if (!s || new_body.empty() || s->_shell->current_room_id_.empty()) return;
                s->_shell->client_->send_edit(s->_shell->current_room_id_, event_id, new_body);
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

    _typingBar = [NSTextField labelWithString:@""];
    _typingBar.textColor = NSColor.secondaryLabelColor;
    _typingBar.font = [NSFont systemFontOfSize:11];
    _typingBar.editable = NO;
    _typingBar.bordered = NO;
    _typingBar.drawsBackground = NO;
    _typingBar.translatesAutoresizingMaskIntoConstraints = NO;
    [_typingBar addConstraint:
        [_typingBar.heightAnchor constraintEqualToConstant:20]];

    _contentStack = [NSStackView stackViewWithViews:@[header, recoveryView, verifView,
                                                       msgSurfaceView, _typingBar,
                                                       composeView]];
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
        [verifView.leadingAnchor  constraintEqualToAnchor:_contentStack.leadingAnchor],
        [verifView.trailingAnchor constraintEqualToAnchor:_contentStack.trailingAnchor],
        [msgSurfaceView.leadingAnchor  constraintEqualToAnchor:_contentStack.leadingAnchor],
        [msgSurfaceView.trailingAnchor constraintEqualToAnchor:_contentStack.trailingAnchor],
        [_typingBar.leadingAnchor  constraintEqualToAnchor:_contentStack.leadingAnchor
                                                 constant:8],
        [_typingBar.trailingAnchor constraintEqualToAnchor:_contentStack.trailingAnchor],
        [composeView.leadingAnchor  constraintEqualToAnchor:_contentStack.leadingAnchor],
        [composeView.trailingAnchor constraintEqualToAnchor:_contentStack.trailingAnchor],
    ]];
    _verifHeightCon = [verifView.heightAnchor constraintEqualToConstant:0];
    _verifHeightCon.active = YES;
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
                if (auto* f = s->_shell->anim_cache_.current_frame(url)) return f;
                auto it = s->_shell->tk_images_.find(url);
                return it == s->_shell->tk_images_.end() ? nullptr : it->second.get();
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
                auto it = s->_shell->tk_images_.find(url);
                return it == s->_shell->tk_images_.end() ? nullptr : it->second.get();
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
    // worker calls `client_.fetch_*` (which takes `&mut self` on the
    // Rust side); racing one against `~ClientFfi` is a data race that
    // surfaces as `panic_in_cleanup` through cxx's `prevent_unwind`.
    _shell->shutting_down_.store(true, std::memory_order_release);
    {
        std::unique_lock<std::mutex> lk(_shell->workers_mu_);
        _shell->workers_cv_.wait_for(lk, std::chrono::seconds(5),
            [self]{ return self->_shell->workers_in_flight_ == 0; });
    }
    for (auto& acc : _shell->accounts_) {
        if (acc->sync_started) acc->client->stop_sync();
    }
}

- (void)showEmojiPicker:(id)sender {
    if (!_composeSurface) return;
    EmojiPickerPanel* panel = [EmojiPickerPanel sharedPanel];
    panel.client = _shell->client_;
    __weak MainWindowController* weakSelf = self;
    panel.onSelect = ^(NSString* glyph) {
        MainWindowController* s = weakSelf;
        if (!s || glyph.length == 0) return;
        // Reaction mode — "+" chip set _pendingReactionEventId.
        if (!s->_pendingReactionEventId.empty()) {
            std::string ev = std::move(s->_pendingReactionEventId);
            s->_pendingReactionEventId.clear();
            if (!s->_shell->current_room_id_.empty()) {
                s->_shell->client_->send_reaction(s->_shell->current_room_id_, ev,
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
    panel.client = _shell->client_;
    __weak MainWindowController* weakSelf = self;
    panel.onSelect = ^(NSString* glyph) {
        MainWindowController* s = weakSelf;
        if (!s || glyph.length == 0) return;
        if (!s->_pendingReactionEventId.empty()) {
            std::string ev = std::move(s->_pendingReactionEventId);
            s->_pendingReactionEventId.clear();
            if (!s->_shell->current_room_id_.empty()) {
                s->_shell->client_->send_reaction(s->_shell->current_room_id_, ev,
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
    if (_shell->current_room_id_.empty() || !_composeSurface) return;
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
    auto res = _shell->client_->send_image(_shell->current_room_id_, enc.bytes, enc.mime,
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
    if (_shell->current_room_id_.empty()) return;
    auto res = _shell->client_->send_file(_shell->current_room_id_, bytes, mime,
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

        auto* bridge_ptr = new tesseract::EventHandlerBase(_shell.get());
        bridge_ptr->set_user_id(uid);
        session->bridge.reset(bridge_ptr);

        session->user_id      = session->client->get_user_id();
        session->display_name = session->client->get_display_name();
        session->avatar_url   = session->client->get_avatar_url();
        session->last_room    = tesseract::Prefs::parse(
            session->client->load_prefs_json()).last_room;
        session->sync_started = true;
        session->client->start_sync(session->bridge.get());

        _shell->accounts_.push_back(std::move(session));
    }

    if (_shell->accounts_.empty()) {
        _shell->pending_login_client_ = std::make_unique<tesseract::Client>();
        auto ms = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        _shell->pending_login_temp_dir_ = tesseract::SessionStore::account_dir("pending-" + ms);
        _shell->pending_login_client_->set_data_dir(
            (_shell->pending_login_temp_dir_ / "matrix-store").string());
        [_loginView setClient:_shell->pending_login_client_.get()];
        [_loginView setMode:tesseract::views::LoginView::Mode::Initial];
        [_loginView reset];
        _splitView.hidden = YES;
        _loginView.hidden = NO;
        return;
    }

    int firstActive = 0;
    for (int i = 0; i < (int)_shell->accounts_.size(); ++i) {
        if (_shell->accounts_[i]->user_id == index.active_user_id) {
            firstActive = i;
            break;
        }
    }
    [self _switchActiveAccount:firstActive];
}

- (void)loginViewDidSucceed:(LoginView*)view {
    if (!_shell->pending_login_client_) return;

    std::string sessionJson = _shell->pending_login_client_->export_session();
    std::string newUserId   = _shell->pending_login_client_->get_user_id();

    auto finalDir = tesseract::SessionStore::account_dir(newUserId);
    std::error_code ec;
    std::filesystem::create_directories(finalDir.parent_path(), ec);
    std::filesystem::rename(_shell->pending_login_temp_dir_, finalDir, ec);
    if (ec) finalDir = _shell->pending_login_temp_dir_;  // EXDEV fallback: keep as-is
    _shell->pending_login_client_.reset();  // close SQLite handles before reopen
    _shell->pending_login_temp_dir_ = {};

    auto session = std::make_unique<tesseract::AccountSession>();
    session->client = std::make_unique<tesseract::Client>();
    session->client->set_data_dir(
        tesseract::SessionStore::sdk_store_dir(newUserId).string());
    if (!session->client->restore_session(sessionJson)) return;

    auto* bridge_ptr = new tesseract::EventHandlerBase(_shell.get());
    bridge_ptr->set_user_id(newUserId);
    session->bridge.reset(bridge_ptr);

    session->user_id      = newUserId;
    session->display_name = session->client->get_display_name();
    session->avatar_url   = session->client->get_avatar_url();
    session->last_room    = tesseract::Prefs::parse(
        session->client->load_prefs_json()).last_room;
    session->sync_started = true;
    session->client->start_sync(session->bridge.get());

    tesseract::SessionStore::save_account(newUserId, sessionJson);
    auto idxData = tesseract::SessionStore::load_index();
    auto& ids = idxData.user_ids;
    if (std::find(ids.begin(), ids.end(), newUserId) == ids.end())
        ids.push_back(newUserId);
    idxData.active_user_id = newUserId;
    tesseract::SessionStore::save_index(idxData);

    int newIdx = (int)_shell->accounts_.size();
    _shell->accounts_.push_back(std::move(session));
    _shell->pending_login_is_add_account_ = false;
    _shell->add_account_return_idx_ = -1;

    [self _switchActiveAccount:newIdx];
}

- (void)loginViewDidCancel:(LoginView*)view {
    _shell->pending_login_client_.reset();
    if (_shell->pending_login_temp_dir_ != std::filesystem::path()) {
        std::error_code ec;
        std::filesystem::remove_all(_shell->pending_login_temp_dir_, ec);
        _shell->pending_login_temp_dir_ = {};
    }
    _shell->pending_login_is_add_account_ = false;
    int returnIdx = _shell->add_account_return_idx_;
    _shell->add_account_return_idx_ = -1;
    if (returnIdx >= 0 && returnIdx < (int)_shell->accounts_.size())
        [self _switchActiveAccount:returnIdx];
}

- (void)_switchActiveAccount:(int)idx {
    _shell->active_account_index_ = idx;
    auto* session = _shell->accounts_[idx].get();
    _shell->client_ = session->client.get();
    _shell->event_handler_ = session->bridge.get();

    _shell->my_user_id_      = session->user_id;
    _shell->my_display_name_ = session->display_name;
    _shell->my_avatar_url_   = session->avatar_url;
    _shell->pending_restore_room_ = session->last_room;

    auto idxData = tesseract::SessionStore::load_index();
    idxData.active_user_id = _shell->my_user_id_;
    tesseract::SessionStore::save_index(idxData);

    _shell->current_room_id_.clear();
    _shell->space_stack_.clear();

    auto it = _shell->per_account_rooms_.find(_shell->my_user_id_);
    _shell->rooms_ = (it != _shell->per_account_rooms_.end())
        ? it->second : std::vector<tesseract::RoomInfo>{};
    [self _refreshRoomList];
    _messageListView->set_messages({});
    _msgSurface->relayout();

    _splitView.hidden = NO;
    _loginView.hidden = YES;

    [self _populateUserStrip];
    [self _maybeShowRecoveryBanner];

    if (!_shell->pending_restore_room_.empty()) {
        for (const auto& r : _shell->rooms_) {
            if (r.id == _shell->pending_restore_room_ && !r.is_space) {
                std::string target = std::move(_shell->pending_restore_room_);
                _shell->pending_restore_room_.clear();
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
    _shell->pending_login_is_add_account_ = true;
    _shell->add_account_return_idx_ = _shell->active_account_index_;
    _shell->pending_login_client_ = std::make_unique<tesseract::Client>();
    auto ms = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    _shell->pending_login_temp_dir_ = tesseract::SessionStore::account_dir("pending-" + ms);
    _shell->pending_login_client_->set_data_dir(
        (_shell->pending_login_temp_dir_ / "matrix-store").string());
    [_loginView setClient:_shell->pending_login_client_.get()];
    [_loginView setMode:tesseract::views::LoginView::Mode::AddAccount];
    [_loginView reset];
    _splitView.hidden = YES;
    _loginView.hidden = NO;
}

- (void)_populateUserStrip {
    NSString* shown = _shell->my_display_name_.empty()
        ? [NSString stringWithUTF8String:_shell->my_user_id_.c_str()]
        : [NSString stringWithUTF8String:_shell->my_display_name_.c_str()];
    _userNameLabel.stringValue = shown ?: @"";
    _userIdLabel.stringValue = [NSString stringWithUTF8String:_shell->my_user_id_.c_str()] ?: @"";
    _userStrip.hidden = NO;
    _userStripHeightCon.constant = 48;

    if (!_shell->my_avatar_url_.empty()) {
        std::string url = _shell->my_avatar_url_;
        __weak MainWindowController* weakSelf = self;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            MainWindowController* s = weakSelf;
            if (!s) return;
            auto bytes = s->_shell->client_->fetch_media_bytes(url);
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

- (void)_onRoomScrollDebounce {
    if (!_roomListView || !_shell->client_) return;
    auto ids = _roomListView->visible_room_ids();
    _shell->client_->stop_background_backfill();
    _shell->client_->start_background_backfill(ids);
}

- (void)_onSpaceBack {
    if (!_shell->space_stack_.empty()) _shell->space_stack_.pop_back();
    [self _refreshRoomList];
}

- (void)_onUserStripRightClick:(NSGestureRecognizer*)gr {
    if (gr.state != NSGestureRecognizerStateEnded) return;
    NSString* logoutTitle = [NSString stringWithFormat:@"Log Out %@",
        _shell->my_display_name_.empty()
            ? [NSString stringWithUTF8String:_shell->my_user_id_.c_str()]
            : [NSString stringWithUTF8String:_shell->my_display_name_.c_str()]];
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
    if (_shell->accounts_.size() < 2) return;
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
            for (int i = 0; i < (int)s->_shell->accounts_.size(); ++i) {
                if (s->_shell->accounts_[i]->user_id == uid) {
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
                auto it = s->_shell->tk_avatars_.find(mxc);
                return it == s->_shell->tk_avatars_.end() ? nullptr : it->second.get();
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
    for (int i = 0; i < (int)_shell->accounts_.size(); ++i) {
        auto& acc = *_shell->accounts_[i];
        tesseract::views::AccountEntry e;
        e.user_id      = acc.user_id;
        e.display_name = acc.display_name;
        e.avatar_url   = acc.avatar_url;
        e.active       = (i == _shell->active_account_index_);
        entries.push_back(std::move(e));
        if (!acc.avatar_url.empty()) _shell->ensure_user_avatar_(acc.avatar_url);
    }
    _accountPickerShared->set_entries(std::move(entries));

    CGFloat rowH = 48.0f;
    NSSize sz = NSMakeSize(220, rowH * (CGFloat)_shell->accounts_.size());
    _accountPickerPopover.contentSize = sz;
    _accountPickerSurface->relayout();

    [_accountPickerPopover showRelativeToRect:_userStrip.bounds
                                       ofView:_userStrip
                                preferredEdge:NSRectEdgeMaxY];
}

- (void)_logoutActiveAccount {
    if (_shell->active_account_index_ < 0 ||
        _shell->active_account_index_ >= (int)_shell->accounts_.size()) return;
    auto* session = _shell->accounts_[_shell->active_account_index_].get();
    std::string uid = session->user_id;

    if (!_shell->current_room_id_.empty()) {
        _shell->client_->unsubscribe_room(_shell->current_room_id_);
        _shell->current_room_id_.clear();
    }
    session->client->logout();
    session->client->stop_sync();
    session->sync_started = false;

    tesseract::SessionStore::clear_account(uid);
    _shell->per_account_rooms_.erase(uid);
    _shell->accounts_.erase(_shell->accounts_.begin() + _shell->active_account_index_);

    auto idxData = tesseract::SessionStore::load_index();
    auto& ids = idxData.user_ids;
    ids.erase(std::remove(ids.begin(), ids.end(), uid), ids.end());

    if (_shell->accounts_.empty()) {
        _shell->active_account_index_ = -1;
        _shell->client_ = nullptr;
        _shell->event_handler_ = nullptr;
        idxData.active_user_id.clear();
        tesseract::SessionStore::save_index(idxData);

        _shell->rooms_.clear();
        _shell->space_stack_.clear();
        [self _refreshRoomList];
        _messageListView->set_messages({});
        _msgSurface->relayout();
        _userStrip.hidden = YES;
        _userStripHeightCon.constant = 0;

        _shell->pending_login_client_ = std::make_unique<tesseract::Client>();
        auto ms = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        _shell->pending_login_temp_dir_ = tesseract::SessionStore::account_dir("pending-" + ms);
        _shell->pending_login_client_->set_data_dir(
            (_shell->pending_login_temp_dir_ / "matrix-store").string());
        [_loginView setClient:_shell->pending_login_client_.get()];
        [_loginView setMode:tesseract::views::LoginView::Mode::Initial];
        [_loginView reset];
        _splitView.hidden = YES;
        _loginView.hidden = NO;
    } else {
        int newIdx = std::min(_shell->active_account_index_, (int)_shell->accounts_.size() - 1);
        idxData.active_user_id = _shell->accounts_[newIdx]->user_id;
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
            && _shell->active_account_index_ >= 0
            && (int)_shell->accounts_.size() > _shell->active_account_index_
            && _shell->accounts_[_shell->active_account_index_]->user_id == userId
            && _shell->current_room_id_ == roomId) return;

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
    BOOL activeAccount = uid && _shell->active_account_index_ >= 0
        && (int)_shell->accounts_.size() > _shell->active_account_index_
        && _shell->accounts_[_shell->active_account_index_]->user_id == std::string(uid.UTF8String ?: "");
    if (self.window.isKeyWindow && activeAccount
            && rid && _shell->current_room_id_ == std::string(rid.UTF8String ?: "")) {
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
        for (int i = 0; i < (int)_shell->accounts_.size(); ++i) {
            if (_shell->accounts_[i]->user_id == target_uid) {
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

// Note: _ensureRoomAvatar, _ensureUserAvatar, _ensureMediaImage, and
// _ensureReplyDetails are now handled by ShellBase via MacShell.

// ─────────────────────────────────────────────────────────────────────────
//  Event-bridge callbacks (main thread)
// ─────────────────────────────────────────────────────────────────────────

- (void)handleMessageInserted:(NSString*)roomId
                         index:(std::size_t)index
                         event:(tesseract::Event*)event
{
    std::unique_ptr<tesseract::Event> guard(event);
    if (!event) return;
    if (std::string(roomId.UTF8String ?: "") != _shell->current_room_id_) return;
    if (event->type == tesseract::EventType::Unhandled) return;
    _shell->ensure_row_media_(*event);
    _shell->ensure_reply_details_(event->in_reply_to_id);
    _messageListView->insert_message(index, tesseract::views::make_row_data(*event, _shell->my_user_id_));
    _msgSurface->relayout();
}

- (void)handleMessageUpdated:(NSString*)roomId
                        index:(std::size_t)index
                        event:(tesseract::Event*)event
{
    std::unique_ptr<tesseract::Event> guard(event);
    if (!event) return;
    if (std::string(roomId.UTF8String ?: "") != _shell->current_room_id_) return;
    if (event->type == tesseract::EventType::Unhandled) return;
    _shell->ensure_row_media_(*event);
    _shell->ensure_reply_details_(event->in_reply_to_id);
    _messageListView->update_message(index, tesseract::views::make_row_data(*event, _shell->my_user_id_));
    _msgSurface->relayout();
}

- (void)handleMessageRemoved:(NSString*)roomId
                        index:(std::size_t)index
{
    if (std::string(roomId.UTF8String ?: "") != _shell->current_room_id_) return;
    _messageListView->remove_message(index);
    _msgSurface->relayout();
}

// updateRoomsForUserId: was the old ObjC EventBridge hook. EventHandlerBase now
// calls ShellBase::push_rooms_() directly, which invokes on_rooms_updated_()
// → _refreshRoomList + restore-room logic. No ObjC forwarding needed.

- (void)handleSyncErrorContext:(NSString*)ctx
                    description:(NSString*)desc
                    softLogout:(BOOL)soft {
    if ([ctx isEqualToString:@"sync_auth_error"]) {
        if (soft && _shell->client_ && _shell->active_account_index_ >= 0) {
            std::string uid = _shell->accounts_[_shell->active_account_index_]->user_id;
            if (auto saved = tesseract::SessionStore::load_account(uid)) {
                if (_shell->client_->restore_session(*saved)) {
                    _shell->client_->start_sync(_shell->event_handler_);
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
        roomId.UTF8String && std::string(roomId.UTF8String) == _shell->current_room_id_;
    if (current) {
        std::vector<tesseract::views::MessageRowData> rows;
        rows.reserve(snapshot.size());
        for (auto* ev : snapshot) {
            if (!ev) continue;
            _shell->ensure_row_media_(*ev);
            _shell->ensure_reply_details_(ev->in_reply_to_id);
            rows.push_back(tesseract::views::make_row_data(*ev, _shell->my_user_id_));
        }
        _messageListView->set_messages(std::move(rows));
        _msgSurface->relayout();
    }
    for (auto* ev : snapshot) delete ev;
}

- (void)handleVerificationState:(BOOL)isVerified {
    if (!_verifSurface) return;
    NSView* v = (__bridge NSView*)_verifSurface->view_handle();
    if (!isVerified && !_shell->verification_banner_dismissed_) {
        if (v.hidden) {
            if (_verifShared) _verifShared->set_state(
                tesseract::views::VerificationBanner::State::Prompt);
            v.hidden = NO;
            _verifHeightCon.constant = 48;
            _verifSurface->relayout();
        }
    } else if (!v.hidden) {
        v.hidden = YES;
        _verifHeightCon.constant = 0;
    }
}

- (void)handleVerificationRequest:(std::string)flowId incoming:(BOOL)incoming {
    if (!_verifShared) return;
    if (incoming) {
        _verifShared->set_state(
            tesseract::views::VerificationBanner::State::IncomingRequest);
    } else {
        _verifShared->set_state(
            tesseract::views::VerificationBanner::State::Waiting);
        if (_shell->client_)
            _shell->client_->start_sas(_shell->active_verification_flow_id_);
    }
    if (_verifSurface) _verifSurface->relayout();
}

- (void)handleSasReady:(std::vector<tesseract::VerificationEmoji>)emojis {
    if (!_verifShared) return;
    _verifShared->set_emojis(emojis);
    _verifHeightCon.constant = 124;
    if (_verifSurface) _verifSurface->relayout();
}

- (void)handleVerificationDone {
    if (!_verifShared) return;
    _verifShared->set_state(tesseract::views::VerificationBanner::State::Done);
    if (_verifSurface) _verifSurface->relayout();
    __weak MainWindowController* weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        MainWindowController* s = weakSelf;
        if (s && s->_verifShared && s->_verifShared->on_done)
            s->_verifShared->on_done();
    });
}

- (void)handleVerificationCancelled:(std::string)reason {
    if (!_verifShared) return;
    _verifShared->set_state(
        tesseract::views::VerificationBanner::State::Cancelled);
    _verifShared->set_cancel_reason(std::move(reason));
    if (_verifSurface) _verifSurface->relayout();
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
        && _shell->client_ && !_shell->client_->needs_recovery()
        && _recoverySurface)
    {
        ((__bridge NSView*)_recoverySurface->view_handle()).hidden = YES;
    }
}

- (void)_relayoutRoomSurface {
    if (_roomSurface) _roomSurface->relayout();
}

- (void)_relayoutMsgSurface {
    if (_msgSurface) _msgSurface->relayout();
}

- (void)_updateTypingBar:(NSString*)text {
    _typingBar.stringValue = text ?: @"";
}

- (void)_onRoomListStateChanged {
    // Update window title with sync progress (no status bar on macOS).
    // Priority: Init/SettingUp → "Syncing rooms…",
    //           Recovering     → "Reconnecting…",
    //           keys busy      → "Downloading encryption keys (N)…",
    //           else           → clear progress suffix.
    using RLS = tesseract::RoomListState;
    using BS  = tesseract::BackupState;

    const bool room_busy    = (_shell->last_room_list_state_ == RLS::Init
                            || _shell->last_room_list_state_ == RLS::SettingUp);
    const bool reconnecting = (_shell->last_room_list_state_ == RLS::Recovering);
    const bool keys_busy    = (_shell->last_backup_state_ == BS::Downloading);

    NSString* suffix = nil;
    if (room_busy) {
        suffix = @" — Syncing rooms…";
    } else if (reconnecting) {
        suffix = @" — Reconnecting…";
    } else if (keys_busy) {
        suffix = [NSString stringWithFormat:@" — Downloading encryption keys (%llu)…",
                  (unsigned long long)_shell->last_imported_keys_];
    }

    if (suffix) {
        _shell->sync_progress_shown_ = true;
        self.window.title = [@"Tesseract" stringByAppendingString:suffix];
    } else if (_shell->sync_progress_shown_) {
        _shell->sync_progress_shown_ = false;
        self.window.title = @"Tesseract";
    }

    // Once Running, attempt the deferred room restore (we waited for Running
    // to avoid subscribing to a room during initial sync, which triggers the
    // imbl promote_front data race in matrix-sdk-ui).
    if (_shell->last_room_list_state_ == RLS::Running
        && _shell->current_room_id_.empty()
        && !_shell->pending_restore_room_.empty())
    {
        for (const auto& r : _shell->rooms_) {
            if (r.id == _shell->pending_restore_room_ && !r.is_space) {
                std::string target = std::move(_shell->pending_restore_room_);
                _shell->pending_restore_room_.clear();
                [self onRoomSelected:target];
                break;
            }
        }
    }
}

- (void)_maybeShowRecoveryBanner {
    if (_shell->recovery_banner_dismissed_)           return;
    if (!_shell->client_ || !_shell->client_->needs_recovery()) return;
    if (!_recoverySurface)                            return;
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
    _shell->run_async_([weakSelf, key, clientPtr = _shell->client_]() {
        auto res = clientPtr->recover(key);
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
    });
}

- (void)_onRecoveryDismiss {
    _shell->recovery_banner_dismissed_ = true;
    if (_recoverySurface) {
        ((__bridge NSView*)_recoverySurface->view_handle()).hidden = YES;
    }
}

- (void)_refreshRoomList {
    std::vector<tesseract::RoomInfo> filtered;
    if (_shell->space_stack_.empty()) {
        std::unordered_set<std::string> in_space;
        if (_shell->client_) {
            for (const auto& r : _shell->rooms_) {
                if (!r.is_space) continue;
                for (const auto& id : _shell->client_->space_children(r.id))
                    in_space.insert(id);
            }
        }
        filtered.reserve(_shell->rooms_.size());
        for (const auto& r : _shell->rooms_)
            if (!r.is_space && (!in_space.count(r.id) || r.is_favorite)) filtered.push_back(r);
        for (const auto& r : _shell->rooms_)
            if ( r.is_space) filtered.push_back(r);
        _spaceNavBar.hidden = YES;
        _spaceNavHeightCon.constant = 0;
    } else {
        auto child_ids = _shell->client_
            ? _shell->client_->space_children(_shell->space_stack_.back())
            : std::vector<std::string>{};
        for (const auto& r : _shell->rooms_) {
            if (std::find(child_ids.begin(), child_ids.end(), r.id)
                != child_ids.end()) {
                filtered.push_back(r);
            }
        }
        for (const auto& r : _shell->rooms_) {
            if (r.id == _shell->space_stack_.back()) {
                _spaceNameLabel.stringValue =
                    [NSString stringWithUTF8String:r.name.c_str()] ?: @"";
                break;
            }
        }
        _spaceNavBar.hidden = NO;
        _spaceNavHeightCon.constant = 36;
    }
    for (const auto& r : filtered) _shell->ensure_room_avatar_(r);

    _roomListView->set_rooms(filtered);
    if (!_shell->current_room_id_.empty()) {
        _roomListView->set_selected_room(_shell->current_room_id_);
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
    for (const auto& r : _shell->rooms_) {
        if (r.id == roomId && r.is_space) {
            _shell->space_stack_.push_back(roomId);
            [self _refreshRoomList];
            return;
        }
    }
    _shell->handle_compose_room_leaving_(_shell->current_room_id_);
    if (!_shell->current_room_id_.empty() && _shell->current_room_id_ != roomId) {
        _shell->client_->unsubscribe_room(_shell->current_room_id_);
    }
    _shell->current_room_id_ = roomId;
    _shell->mark_room_read_(roomId);
    _shell->reply_details_requested_.clear();
    {
        auto prefs = tesseract::Prefs::parse(_shell->client_->load_prefs_json());
        prefs.last_room = roomId;
        _shell->client_->save_prefs_json(tesseract::Prefs::serialize(prefs));
    }
    if (_composeShared) {
        _composeShared->clear_reply();
        _composeShared->clear_editing();
    }
    _shell->update_typing_bar_({}, false);
    for (const auto& r : _shell->rooms_) {
        if (r.id == _shell->current_room_id_) { [self _setRoomHeader:r]; break; }
    }

    // subscribe_room + paginate_back both block inside the Rust runtime;
    // run them on a background queue so the main thread stays responsive.
    std::vector<std::string> visibleIds =
        _roomListView ? _roomListView->visible_room_ids()
                      : std::vector<std::string>{};
    std::string subRoom = _shell->current_room_id_;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto res = self->_shell->client_->subscribe_room(subRoom);
        BOOL reached = NO;
        if (res) {
            auto pr = self->_shell->client_->paginate_back_with_status(subRoom, 50);
            reached = pr.ok && pr.reached_start;
            self->_shell->client_->start_background_backfill(visibleIds);
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            [self handleSubscribeResultForRoom:subRoom reached:reached];
        });
    });
}

- (void)requestMoreHistoryForRoom:(std::string)roomId {
    if (roomId.empty()) return;
    auto& state = _shell->pagination_[roomId];
    if (state.in_flight || state.reached_start) return;
    state.in_flight = true;

    // Run the blocking paginate call on a background queue; marshal the
    // result back to the main thread.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto pr = self->_shell->client_->paginate_back_with_status(roomId, 50);
        BOOL reached = pr.ok && pr.reached_start;
        dispatch_async(dispatch_get_main_queue(), ^{
            [self handlePaginateResultForRoom:roomId reached_start:reached];
        });
    });
}

- (void)handlePaginateResultForRoom:(std::string)roomId
                      reached_start:(BOOL)reached {
    auto it = _shell->pagination_.find(roomId);
    if (it == _shell->pagination_.end()) return;
    it->second.in_flight     = false;
    it->second.reached_start = reached;
    if (roomId == _shell->current_room_id_ && _messageListView)
        _messageListView->reset_near_top_latch();
}

- (void)handleSubscribeResultForRoom:(std::string)roomId reached:(BOOL)reached {
    if (roomId != _shell->current_room_id_) return;
    auto& state = _shell->pagination_[roomId];
    state.in_flight     = false;
    state.reached_start = reached;
}

// Note: _ensureRowMedia is now handled by ShellBase::ensure_row_media_() via
// MacShell. MacShell::generate_video_thumbnail_ handles client-side first-frame
// generation for m.video when the server provides no thumbnail.

// ─────────────────────────────────────────────────────────────────────────
//  Animated sticker support
// ─────────────────────────────────────────────────────────────────────────

- (void)_decodeMediaBytes:(const std::vector<uint8_t>&)bytes
                   forKey:(const std::string&)key {
    if (bytes.empty() || _shell->tk_images_.count(key) || _shell->anim_cache_.has(key)) return;
    CFDataRef data = CFDataCreate(kCFAllocatorDefault,
                                   bytes.data(),
                                   static_cast<CFIndex>(bytes.size()));
    if (!data) return;
    CGImageSourceRef src = CGImageSourceCreateWithData(data, nullptr);
    CFRelease(data);
    if (!src) return;

    std::size_t count = CGImageSourceGetCount(src);
    if (count > 1) {
        std::vector<std::unique_ptr<tk::Image>> frames;
        std::vector<int> delays;
        frames.reserve(count);
        delays.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            CGImageRef frame = CGImageSourceCreateImageAtIndex(src, i, nullptr);
            if (!frame) continue;
            frames.push_back(tk::cg::make_image(frame));
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
            delays.push_back(std::max(delay_ms, 20));
        }
        if (!frames.empty()) {
            CFRelease(src);
            const std::int64_t now_ms = static_cast<std::int64_t>(
                [[NSDate date] timeIntervalSince1970] * 1000.0);
            _shell->anim_cache_.store(key, std::move(frames), std::move(delays), now_ms);
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
    _shell->tk_images_.emplace(key, tk::cg::make_image(img));
    CGImageRelease(img);
}

- (void)_startAnimTickIfNeeded {
    if (_animTimer || _shell->anim_cache_.empty()) return;
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
    if (_shell->anim_cache_.empty()) {
        [_animTimer invalidate];
        _animTimer = nil;
        return;
    }
    const std::int64_t now =
        static_cast<std::int64_t>([[NSDate date] timeIntervalSince1970] * 1000.0);
    if (_shell->anim_cache_.advance(now)) {
        if (_msgSurface) _msgSurface->relayout();
        StickerPickerPanel* panel = [StickerPickerPanel sharedPanel];
        if (panel.isVisible) [panel invalidateImageCache];
    }
}

- (void)_ensureStickerImageAsync:(std::string)url {
    if (url.empty() || _shell->tk_images_.count(url) || _shell->anim_cache_.has(url)) return;
    if (!_shell->sticker_fetches_in_flight_.insert(url).second) return;

    __weak MainWindowController* weakSelf = self;
    auto bytes_holder = std::make_shared<std::vector<uint8_t>>();

    _shell->run_async_([weakSelf, url, bytes_holder,
                         clientPtr = _shell->client_]() {
        *bytes_holder = clientPtr->fetch_source_bytes(url);
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController* s = weakSelf;
            if (!s) return;
            s->_shell->sticker_fetches_in_flight_.erase(url);
            if (bytes_holder->empty()
                || s->_shell->tk_images_.count(url)
                || s->_shell->anim_cache_.has(url)) return;
            [s _decodeMediaBytes:*bytes_holder forKey:url];
            StickerPickerPanel* panel = [StickerPickerPanel sharedPanel];
            if (panel.isVisible) [panel invalidateImageCache];
        });
    });
}

// ─────────────────────────────────────────────────────────────────────────
//  Sticker picker
// ─────────────────────────────────────────────────────────────────────────

- (void)handleImagePacksUpdated {
    StickerPickerPanel* panel = [StickerPickerPanel sharedPanel];
    panel.client = _shell->client_;
    [panel refreshPacks];
}

- (void)handleAccountPrefsUpdated:(NSString*)json {
    auto prefs = tesseract::Prefs::parse(json.UTF8String);
    if (!prefs.last_room.empty()
        && _shell->pending_restore_room_.empty()
        && _shell->current_room_id_.empty())
        _shell->pending_restore_room_ = prefs.last_room;
}

- (void)_showStickerPicker {
    if (!_composeSurface || _shell->current_room_id_.empty()) return;
    StickerPickerPanel* panel = [StickerPickerPanel sharedPanel];
    panel.client = _shell->client_;

    __weak MainWindowController* weakSelf = self;

    [panel setImageProvider:
        [weakSelf](const std::string& cache_key,
                   const std::string& /*source_token*/) -> const tk::Image* {
            MainWindowController* s = weakSelf;
            if (!s) return nullptr;
            if (auto* f = s->_shell->anim_cache_.current_frame(cache_key)) return f;
            auto it = s->_shell->tk_images_.find(cache_key);
            if (it != s->_shell->tk_images_.end()) return it->second.get();
            [s _ensureStickerImageAsync:cache_key];
            return nullptr;
        }];

    panel.onSelected = ^(NSString* url, NSString* body, NSString* infoJson) {
        MainWindowController* s = weakSelf;
        if (!s || s->_shell->current_room_id_.empty()) return;
        std::string u = url.UTF8String      ?: "";
        std::string b = body.UTF8String     ?: "";
        std::string j = infoJson.UTF8String ?: "{}";
        s->_shell->client_->send_sticker(s->_shell->current_room_id_, b, u, j);
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
    _shell->client_->save_sticker_to_user_pack(
        _ctxStickerBody,
        _ctxStickerBody,
        _ctxStickerMxcUrl,
        "{}");
    _ctxStickerEventId.clear();
    _ctxStickerMxcUrl.clear();
    _ctxStickerBody.clear();
}

@end
