#import "MainWindowController.h"
#import "LoginView.h"
#import "EmojiPicker.h"
#import "StickerPicker.h"
#import "MacOSTrayIcon.h"
#import "MacScreenLock.h"
#import "RoomWindowController.h"

#include <tesseract/client.h>
#include <tesseract/event_handler.h>
#include <tesseract/session_store.h>
#include <tesseract/prefs.h>
#include <tesseract/paths.h>
#include <tesseract/settings.h>
#include <tesseract/visual.h>

#include "tk/anim_image_cache.h"
#include "tk/canvas_cg.h"
#include "tk/host.h"
#include "tk/host_macos.h"
#include "tk/theme.h"
#include "util.h"
#include "views/BrandView.h"
#include "views/MainAppWidget.h"
#include "views/SettingsView.h"
#include "views/ShortcodeEngine.h"
#include "views/ShortcodePopup.h"

#include <ImageIO/ImageIO.h>
#import <AVFoundation/AVFoundation.h>
#import <UserNotifications/UserNotifications.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

// Animated WebP properties landed in macOS 11 SDK; define them ourselves
// when building against an older SDK so we can still attempt WebP delay
// extraction at runtime on newer OS versions.
#ifndef kCGImagePropertyWebPDictionary
#define kCGImagePropertyWebPDictionary CFSTR("{WebP}")
#endif
#ifndef kCGImagePropertyWebPDelayTime
#define kCGImagePropertyWebPDelayTime CFSTR("DelayTime")
#endif

#include <tesseract/account_session.h>

#include "app/ShellBase.h"
#include "app/EventHandlerBase.h"
#include "app/SettingsController.h"

#include "views/AccountPicker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

namespace
{

class MacShell final : public tesseract::ShellBase
{
public:
    // ctrl_ is non-owning: MacShell is owned by MainWindowController itself
    // (via _shell), so ctrl_ is always valid for MacShell's lifetime.
    explicit MacShell(MainWindowController* ctrl) : ctrl_(ctrl)
    {
    }

protected:
    void post_to_ui_(std::function<void()> fn) override;
    void on_rooms_updated_() override;
    void on_media_bytes_ready_(const std::string& key,
                               ShellBase::MediaKind kind,
                               std::vector<uint8_t> bytes) override;
    void generate_video_thumbnail_(const std::string& event_id,
                                   const std::string& video_url) override;
    void cache_rgba_image_(const std::string& key, int w, int h,
                           std::vector<uint8_t> rgba) override;

    std::int64_t monotonic_ms_() override;
    void start_anim_tick_() override;
    void repaint_pickers_() override;

    void handle_timeline_reset_ui_(
        std::string room_id,
        std::vector<std::unique_ptr<tesseract::Event>> snapshot) override;
    void
    handle_message_inserted_ui_(std::string room_id, std::size_t index,
                                std::unique_ptr<tesseract::Event> ev) override;
    void
    handle_message_updated_ui_(std::string room_id, std::size_t index,
                               std::unique_ptr<tesseract::Event> ev) override;
    void handle_message_removed_ui_(std::string room_id,
                                    std::size_t index) override;
    void handle_sync_error_ui_(std::string context, std::string user_id,
                               std::string description,
                               bool soft_logout) override;
    void
    handle_backup_progress_ui_(tesseract::BackupProgress progress) override;
    void refresh_pickers_packs_() override;
    void handle_verification_request_ui_(std::string flow_id,
                                         std::string user_id,
                                         std::string device_id,
                                         bool incoming) override;
    void handle_sas_ready_ui_(
        std::string flow_id,
        std::vector<tesseract::VerificationEmoji> emojis) override;
    void handle_verification_done_ui_(std::string flow_id) override;
    void handle_verification_cancelled_ui_(std::string flow_id,
                                           std::string reason) override;
    void handle_verification_state_ui_(bool is_verified) override;
    void handle_notification_ui_(std::string user_id, std::string room_id,
                                 std::string room_name, std::string sender,
                                 std::string body, bool is_mention,
                                 std::vector<uint8_t> avatar_bytes,
                                 std::vector<uint8_t> image_bytes) override;
    void handle_voice_waveform_ready_ui_(std::string room_id,
                                         std::string event_id,
                                         std::vector<std::uint16_t> waveform) override;
    void on_room_list_state_ui_() override;
    void on_server_info_ready_ui_() override;
    void update_typing_bar_(const std::string& text, bool visible) override;
    void on_url_preview_ready_(
        const std::string& url,
        const tesseract::Client::UrlPreview& preview) override;
    void on_url_preview_failed_(const std::string& url) override;

    tk::ThemeMode os_color_scheme_() const override;
    void apply_theme_ui_(const tk::Theme& t) override;
    tesseract::RoomWindowBase*
    create_secondary_room_window_(const std::string& room_id) override;

    // Tab management hooks.
    void on_tab_state_changed_ui_() override;
    float get_message_scroll_fraction_() override;
    void set_message_scroll_fraction_(float t) override;
    std::string get_compose_draft_() override;
    void set_compose_draft_(const std::string&) override;
    const std::vector<tesseract::views::MessageRowData>*
    get_current_messages_() override;
    void apply_cached_messages_(
        const std::vector<tesseract::views::MessageRowData>& msgs) override;

    // Expose ShellBase protected members so MainWindowController ObjC++ code
    // can reach them through _shell (composition, not inheritance).
public:
    using ShellBase::accounts_;
    using ShellBase::active_account_index_;
    using ShellBase::active_tab_idx_;
    using ShellBase::active_verification_flow_id_;
    using ShellBase::add_account_return_idx_;
    using ShellBase::anim_cache_;
    using ShellBase::apply_current_theme_;
    using ShellBase::begin_focused_subscription_;
    using ShellBase::build_rows_;
    using ShellBase::cached_emoticons_;
    using ShellBase::clear_focused_state_;
    using ShellBase::client_;
    using ShellBase::compose_typing_active_;
    using ShellBase::current_room_id_;
    DecodedImage decode_image_(const std::vector<uint8_t>& bytes, int max_w,
                               int max_h) override;
    using ShellBase::emoji_fetches_in_flight_;
    using ShellBase::ensure_media_image_;
    using ShellBase::ensure_picker_image_;
    using ShellBase::ensure_reply_details_;
    using ShellBase::ensure_room_avatar_;
    using ShellBase::ensure_row_media_;
    using ShellBase::ensure_tile_async;
    using ShellBase::ensure_user_avatar_;
    using ShellBase::event_handler_;
    using ShellBase::handle_compose_room_leaving_;
    using ShellBase::handle_compose_text_changed_;
    using ShellBase::last_backup_state_;
    using ShellBase::last_imported_keys_;
    using ShellBase::last_room_list_state_;
    using ShellBase::mark_room_read_;
    using ShellBase::maybe_send_read_receipt_;
    using ShellBase::media_fetches_in_flight_;
    using ShellBase::message_cache_;
    using ShellBase::message_cache_lru_;
    using ShellBase::my_avatar_url_;
    using ShellBase::my_display_name_;
    using ShellBase::my_user_id_;
    using ShellBase::pagination_;
    using ShellBase::pending_login_client_;
    using ShellBase::pending_login_is_add_account_;
    using ShellBase::pending_login_temp_dir_;
    using ShellBase::pending_restore_room_;
    using ShellBase::per_account_rooms_;
    using ShellBase::push_paginate_result_;
    using ShellBase::push_room_list_state_;
    using ShellBase::push_rooms_;
    using ShellBase::recovery_banner_dismissed_;
    using ShellBase::reply_details_requested_;
    using ShellBase::request_forward_history_;
    using ShellBase::return_to_live_;
    using ShellBase::room_subscription_refs_;
    using ShellBase::rooms_;
    using ShellBase::run_async_;
    using ShellBase::set_screen_lock_;
    using ShellBase::set_theme_preference_;
    using ShellBase::shutting_down_;
    using ShellBase::space_stack_;
    using ShellBase::sticker_fetches_in_flight_;
    using ShellBase::sync_progress_shown_;
    using ShellBase::tab_close;
    using ShellBase::tab_navigate_room;
    using ShellBase::tab_open_room;
    using ShellBase::tab_select_room;
    using ShellBase::tabs_;
    using ShellBase::tk_avatars_;
    using ShellBase::tk_images_;
    using ShellBase::verification_banner_dismissed_;
    using ShellBase::video_thumb_in_flight_;
    using ShellBase::view_displayed_room_id_;
    using ShellBase::voice_prefetched_;
    using ShellBase::capture_;
    using ShellBase::wire_voice_capture_;
    using ShellBase::workers_cv_;
    using ShellBase::workers_in_flight_;
    using ShellBase::workers_mu_;
    using ShellBase::reset_server_info_;
    using ShellBase::server_info_;

    // Public method to call the protected update_typing_bar_ method
    void update_typing_bar(const std::string& text, bool visible)
    {
        update_typing_bar_(text, visible);
    }

    // Extract thumbnail, dimensions, and duration from raw bytes on a
    // background thread; posts result back via post_to_ui_.
    void extract_media_info_(std::uint32_t pending_gen,
                             std::vector<std::uint8_t> bytes,
                             std::string mime);

    // Borrowed pointers set by ObjC side after building the chat surface.
    // Exposed here so MacShell C++ methods can call them without crossing
    // the ObjC ivar boundary (which is private to @implementation).
    tesseract::views::RoomView* room_view_ = nullptr;
    tesseract::views::MainAppWidget* main_app_ = nullptr;
    tk::macos::Surface* app_surface_ = nullptr;

    // SettingsController — created at login, reset on account switch.
    std::unique_ptr<tesseract::SettingsController> settings_controller_;

    // Shortcode engine + transient state (owned here, accessed via _shell->).
    tesseract::views::ShortcodeEngine shortcode_engine_;
    tesseract::views::ShortcodeMatch shortcode_active_match_{};
    std::vector<tesseract::views::ShortcodeSuggestion>
        shortcode_current_suggestions_;

    std::unordered_map<std::string, tesseract::views::UrlPreviewData>
        url_preview_data_;

private:
    MainWindowController*
        ctrl_; // non-owning, always valid (owner holds _shell)
};

using tesseract::macos::trim;

} // namespace

// Decoded-image cache entries — owned by the controller, referenced by
// borrowed pointer from the shared views' avatar/image providers.
using TkImagePtr = std::unique_ptr<tk::Image>;

// ─────────────────────────────────────────────────────────────────────────
//  Internal IBO that the C++ EventBridge calls back into.
// ─────────────────────────────────────────────────────────────────────────

@interface MainWindowController () <
    LoginViewDelegate, UNUserNotificationCenterDelegate, NSWindowDelegate>
- (void)handleTimelineReset:(NSString*)roomId
                   snapshot:(std::vector<tesseract::Event*>)snapshot;
- (void)handleMessageInserted:(NSString*)roomId
                        index:(std::size_t)index
                        event:(tesseract::Event*)event;
- (void)handleMessageUpdated:(NSString*)roomId
                       index:(std::size_t)index
                       event:(tesseract::Event*)event;
- (void)handleMessageRemoved:(NSString*)roomId index:(std::size_t)index;
- (void)handlePaginateResultForRoom:(std::string)roomId
                      reached_start:(BOOL)reached;
- (void)handleSubscribeResultForRoom:(std::string)roomId reached:(BOOL)reached;
- (void)requestMoreHistoryForRoom:(std::string)roomId;
- (void)openJumpToDateDialog;
- (void)handleSyncErrorContext:(NSString*)ctx
                   description:(NSString*)desc
                    softLogout:(BOOL)soft;
- (void)_switchActiveAccount:(int)idx;
- (void)_buildSettingsController;
- (void)_beginAddAccount;
- (void)_logoutActiveAccount;
- (void)_openSettings;
- (void)loginViewDidCancel:(LoginView*)view;
- (void)_openAccountPicker;
- (void)handleBackupProgress:(tesseract::BackupProgress)progress;
- (void)handleVerificationState:(BOOL)isVerified;
- (void)handleVerificationRequest:(std::string)flowId incoming:(BOOL)incoming;
- (void)handleSasReady:(std::vector<tesseract::VerificationEmoji>)emojis;
- (void)handleVerificationDone;
- (void)handleVerificationCancelled:(std::string)reason;

- (void)onRoomSelected:(std::string)roomId;
- (void)_setComposeDraft:(const std::string&)draft;
- (void)_onRecoveryVerify;
- (void)_onRecoveryDismiss;
- (void)_maybeShowRecoveryBanner;
- (void)showShortcodePopupWithSuggestions:
            (const std::vector<tesseract::views::ShortcodeSuggestion>&)
                suggestions
                               cursorRect:(tk::Rect)cursor;
- (void)hideShortcodePopup;
- (BOOL)shortcodePopupVisible;
- (void)showEmojiPickerAtRect:(tk::Rect)anchor;
- (void)_sendComposedImage:(std::vector<std::uint8_t>)bytes
                      mime:(std::string)mime
                  filename:(std::string)filename
                   caption:(std::string)caption
                     width:(std::uint32_t)width
                    height:(std::uint32_t)height
                isAnimated:(bool)is_animated
              replyEventId:(std::string)reply_event_id;
- (void)_sendComposedVideo:(std::vector<std::uint8_t>)bytes
                      mime:(std::string)mime
                  filename:(std::string)filename
                   caption:(std::string)caption
                     width:(std::uint32_t)width
                    height:(std::uint32_t)height
                thumbBytes:(std::vector<std::uint8_t>)thumb_bytes
                thumbWidth:(std::uint32_t)thumb_width
               thumbHeight:(std::uint32_t)thumb_height
                durationMs:(std::uint64_t)duration_ms
              replyEventId:(std::string)reply_event_id;
- (void)_sendComposedAudio:(std::vector<std::uint8_t>)bytes
                      mime:(std::string)mime
                  filename:(std::string)filename
                   caption:(std::string)caption
                durationMs:(std::uint64_t)duration_ms
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
                 isMention:(BOOL)isMention
               avatarBytes:(const std::vector<std::uint8_t>&)avatarBytes
                imageBytes:(const std::vector<std::uint8_t>&)imageBytes;
- (void)_navigateToRoom:(std::string)roomId;
- (void)_refreshRoomList;
- (void)_relayoutRoomSurface;
- (void)_relayoutChatSurface;
- (void)_onRoomListStateChanged;
- (void)_onServerInfoReady;

// Sticker picker + animated stickers.
- (void)handleImagePacksUpdated;
- (void)_showStickerPicker;
- (void)_showStickerPickerAtRect:(tk::Rect)btn;
- (void)_showStickerContextMenuAt:(NSPoint)screenPt;
- (void)_onStickerSave:(id)sender;
- (void)_startAnimTickIfNeeded;
- (void)_animTick:(NSTimer*)timer;
- (void)_ensureStickerImageAsync:(std::string)url;
- (void)_ensureEmojiImageAsync:(std::string)url;
- (void)_applyTheme:(const tk::Theme&)t;
- (void)_decodeMediaBytes:(const std::vector<uint8_t>&)bytes
                   forKey:(const std::string&)key;
- (void)_onSpaceBack;
- (void)_onComposeSend;
- (void)_relayoutShortcodePopupIfVisible;
@end

namespace
{

// ── MacShell method implementations ──────────────────────────────────────

void MacShell::post_to_ui_(std::function<void()> fn)
{
    auto* heap = new std::function<void()>(std::move(fn));
    dispatch_async(dispatch_get_main_queue(), ^{
        (*heap)();
        delete heap;
    });
}

void MacShell::on_rooms_updated_()
{
    MainWindowController* c = ctrl_;
    if (!c)
    {
        return;
    }
    [c _refreshRoomList];
    if (!current_room_id_.empty())
    {
        for (const auto& r : rooms_)
        {
            if (r.id == current_room_id_)
            {
                if (room_view_)
                {
                    room_view_->set_room(r);
                    [c _relayoutChatSurface];
                }
                break;
            }
        }
    }
    else if (!pending_restore_room_.empty() &&
             last_room_list_state_ == tesseract::RoomListState::Running)
    {
        for (const auto& r : rooms_)
        {
            if (r.id == pending_restore_room_ && !r.is_space)
            {
                std::string target = std::move(pending_restore_room_);
                pending_restore_room_.clear();
                [c onRoomSelected:target];
                break;
            }
        }
    }

    update_secondary_room_infos_();
}

void MacShell::on_media_bytes_ready_(const std::string& key,
                                     ShellBase::MediaKind kind,
                                     std::vector<uint8_t> bytes)
{
    MainWindowController* c = ctrl_;
    if (!c)
    {
        return;
    }
    if (bytes.empty())
    {
        return;
    }
    if (kind == MediaKind::MediaImage)
    {
        [c _decodeMediaBytes:bytes forKey:key];
        [c _relayoutChatSurface];
        [c _relayoutShortcodePopupIfVisible];
        return;
    }
    if (kind == MediaKind::Tile)
    {
        if (tk_images_.count(key))
        {
            return;
        }
        CFDataRef data = CFDataCreate(kCFAllocatorDefault, bytes.data(),
                                      static_cast<CFIndex>(bytes.size()));
        if (!data)
        {
            return;
        }
        CGImageSourceRef src = CGImageSourceCreateWithData(data, nullptr);
        CFRelease(data);
        if (!src)
        {
            return;
        }
        CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
        CFRelease(src);
        if (!img)
        {
            return;
        }
        tk_images_.emplace(key, tk::cg::make_image(img));
        CGImageRelease(img);
        if (room_view_)
        {
            room_view_->message_list()->invalidate_data();
        }
        [c _relayoutChatSurface];
        return;
    }
    if (bytes.empty() || tk_avatars_.count(key))
    {
        return;
    }
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, bytes.data(),
                                  static_cast<CFIndex>(bytes.size()));
    if (!data)
    {
        return;
    }
    CGImageSourceRef src = CGImageSourceCreateWithData(data, nullptr);
    CFRelease(data);
    if (!src)
    {
        return;
    }
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!img)
    {
        return;
    }
    tk_avatars_.emplace(key, tk::cg::make_image(img));
    CGImageRelease(img);
    if (kind == MediaKind::RoomAvatar)
    {
        [c _relayoutRoomSurface];
    }
    else if (kind == MediaKind::UserAvatar)
    {
        [c _relayoutChatSurface];
    }
}

void MacShell::generate_video_thumbnail_(const std::string& event_id,
                                         const std::string& video_url)
{
    MainWindowController* c = ctrl_;
    if (!c)
    {
        return;
    }
    std::string src = video_url;
    std::string eid = event_id;
    auto bytes_holder = std::make_shared<std::vector<uint8_t>>();
    run_async_(
        [this, src, eid, bytes_holder]() mutable
        {
            *bytes_holder = client_->fetch_source_bytes(src);
            if (bytes_holder->empty())
            {
                return;
            }
            NSString* tmpDir = NSTemporaryDirectory();
            NSString* eidNS = [NSString stringWithUTF8String:eid.c_str()];
            NSString* tmpPath =
                [tmpDir stringByAppendingPathComponent:
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
            if (!frame)
            {
                return;
            }
            auto img_holder = std::make_shared<std::unique_ptr<tk::Image>>(
                tk::cg::make_image(frame));
            CGImageRelease(frame);
            std::string key = "thumb::" + eid;
            post_to_ui_(
                [this, key, img_holder]() mutable
                {
                    if (tk_images_.count(key))
                    {
                        return;
                    }
                    tk_images_.emplace(key, std::move(*img_holder));
                    MainWindowController* c2 = ctrl_;
                    if (c2)
                    {
                        [c2 _relayoutChatSurface];
                    }
                });
        });
}

void MacShell::extract_media_info_(std::uint32_t pending_gen,
                                   std::vector<std::uint8_t> bytes,
                                   std::string mime)
{
    run_async_([this, pending_gen, bytes = std::move(bytes), mime = std::move(mime)]() mutable
    {
        tesseract::views::MediaInfo info;
        info.pending_gen = pending_gen;

        // ── Animated image detection (gif / webp) ──────────────────────────
        if (mime == "image/gif" || mime == "image/webp")
        {
            NSData* nsdata = [NSData dataWithBytes:bytes.data()
                                            length:bytes.size()];
            CGImageSourceRef src = CGImageSourceCreateWithData(
                (__bridge CFDataRef)nsdata, nullptr);
            if (src)
            {
                info.is_animated = CGImageSourceGetCount(src) > 1;
                CFRelease(src);
            }
        }
        // ── Video: thumbnail + dimensions + duration via AVFoundation ──────
        else if (mime.rfind("video/", 0) == 0)
        {
            NSString* tmpPath =
                [NSTemporaryDirectory() stringByAppendingPathComponent:
                    [NSString stringWithFormat:@"tesseract_drop_%@",
                        [[NSUUID UUID] UUIDString]]];
            NSData* nsdata = [NSData dataWithBytes:bytes.data()
                                            length:bytes.size()];
            [nsdata writeToFile:tmpPath atomically:NO];
            NSURL* url = [NSURL fileURLWithPath:tmpPath];

            AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];

            // Duration
            CMTime dur = asset.duration;
            if (CMTIME_IS_VALID(dur) && !CMTIME_IS_INDEFINITE(dur))
            {
                double secs = CMTimeGetSeconds(dur);
                if (secs > 0.0)
                    info.duration_ms = static_cast<std::uint64_t>(secs * 1000.0);
            }

            // Video dimensions (apply preferred transform to handle rotation)
            NSArray<AVAssetTrack*>* vTracks =
                [asset tracksWithMediaType:AVMediaTypeVideo];
            if (vTracks.count > 0)
            {
                AVAssetTrack* track = vTracks[0];
                CGAffineTransform t = track.preferredTransform;
                CGSize natural = track.naturalSize;
                CGSize sz = CGSizeApplyAffineTransform(natural, t);
                info.video_w =
                    static_cast<std::uint32_t>(std::abs(sz.width));
                info.video_h =
                    static_cast<std::uint32_t>(std::abs(sz.height));
            }

            // First-frame thumbnail → JPEG bytes
            AVAssetImageGenerator* gen =
                [[AVAssetImageGenerator alloc] initWithAsset:asset];
            gen.appliesPreferredTrackTransform = YES;
            NSError* err = nil;
            CGImageRef frame = [gen copyCGImageAtTime:kCMTimeZero
                                           actualTime:nil
                                                error:&err];
            if (frame)
            {
                NSBitmapImageRep* rep =
                    [[NSBitmapImageRep alloc] initWithCGImage:frame];
                CGImageRelease(frame);
                NSData* jpeg = [rep
                    representationUsingType:NSBitmapImageFileTypeJPEG
                                 properties:@{
                                     NSImageCompressionFactor : @0.85
                                 }];
                if (jpeg)
                {
                    const auto* p =
                        static_cast<const std::uint8_t*>(jpeg.bytes);
                    info.thumb_bytes.assign(p, p + jpeg.length);
                    info.thumb_w = static_cast<std::uint32_t>(rep.pixelsWide);
                    info.thumb_h = static_cast<std::uint32_t>(rep.pixelsHigh);
                }
            }

            [[NSFileManager defaultManager] removeItemAtPath:tmpPath error:nil];
        }
        // ── Audio: duration only via AVFoundation ──────────────────────────
        else if (mime.rfind("audio/", 0) == 0)
        {
            NSString* tmpPath =
                [NSTemporaryDirectory() stringByAppendingPathComponent:
                    [NSString stringWithFormat:@"tesseract_drop_%@",
                        [[NSUUID UUID] UUIDString]]];
            NSData* nsdata = [NSData dataWithBytes:bytes.data()
                                            length:bytes.size()];
            [nsdata writeToFile:tmpPath atomically:NO];
            NSURL* url = [NSURL fileURLWithPath:tmpPath];

            AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
            CMTime dur = asset.duration;
            if (CMTIME_IS_VALID(dur) && !CMTIME_IS_INDEFINITE(dur))
            {
                double secs = CMTimeGetSeconds(dur);
                if (secs > 0.0)
                    info.duration_ms = static_cast<std::uint64_t>(secs * 1000.0);
            }

            [[NSFileManager defaultManager] removeItemAtPath:tmpPath error:nil];
        }

        // Post result back to the UI thread; resolve compose_bar() at call
        // time to avoid dangling pointer if the view was freed.
        post_to_ui_([this, info = std::move(info)]() mutable
        {
            if (room_view_)
                room_view_->compose_bar()->update_pending_attachment(info);
        });
    });
}

void MacShell::cache_rgba_image_(const std::string& key, int w, int h,
                                 std::vector<uint8_t> rgba)
{
    if (tk_images_.count(key))
    {
        return;
    }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(rgba.data(), w, h, 8, w * 4, cs,
                                             kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    if (!ctx)
    {
        return;
    }
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    if (!img)
    {
        return;
    }
    tk_images_.emplace(key, tk::cg::make_image(img));
    CGImageRelease(img);
    MainWindowController* c = ctrl_;
    if (c)
    {
        [c _relayoutChatSurface];
    }
}

// Pure decode — no cache mutation. Safe OFF the main queue: CGImageSource
// and tk::cg::make_image are thread-safe. This is the (former) inline
// _decodeMediaBytes CGImageSource logic, returning a DecodedImage so the
// shared ensure_picker_image_/finalize_picker_image_ path can route it.
tesseract::ShellBase::DecodedImage
MacShell::decode_image_(const std::vector<uint8_t>& bytes, int /*max_w*/,
                        int /*max_h*/)
{
    DecodedImage d;
    if (bytes.empty())
    {
        return d;
    }
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, bytes.data(),
                                  static_cast<CFIndex>(bytes.size()));
    if (!data)
    {
        return d;
    }
    CGImageSourceRef src = CGImageSourceCreateWithData(data, nullptr);
    CFRelease(data);
    if (!src)
    {
        return d;
    }

    std::size_t count = CGImageSourceGetCount(src);
    if (count > 1)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            CGImageRef frame = CGImageSourceCreateImageAtIndex(src, i, nullptr);
            if (!frame)
            {
                continue;
            }
            d.frames.push_back(tk::cg::make_image(frame));
            CGImageRelease(frame);
            int delay_ms = 100;
            CFDictionaryRef props =
                CGImageSourceCopyPropertiesAtIndex(src, i, nullptr);
            if (props)
            {
                auto try_delay =
                    [&](CFStringRef dk, CFStringRef uk, CFStringRef ck)
                {
                    auto* dd = (CFDictionaryRef)CFDictionaryGetValue(props, dk);
                    if (!dd)
                    {
                        return;
                    }
                    auto* v = (CFNumberRef)CFDictionaryGetValue(dd, uk);
                    if (!v)
                    {
                        v = (CFNumberRef)CFDictionaryGetValue(dd, ck);
                    }
                    if (!v)
                    {
                        return;
                    }
                    double secs = 0;
                    CFNumberGetValue(v, kCFNumberDoubleType, &secs);
                    if (secs > 0)
                    {
                        delay_ms = static_cast<int>(secs * 1000.0);
                    }
                };
                try_delay(kCGImagePropertyGIFDictionary,
                          kCGImagePropertyGIFUnclampedDelayTime,
                          kCGImagePropertyGIFDelayTime);
                try_delay(kCGImagePropertyPNGDictionary,
                          kCGImagePropertyAPNGUnclampedDelayTime,
                          kCGImagePropertyAPNGDelayTime);
                if (@available(macOS 11.0, *))
                {
                    try_delay(kCGImagePropertyWebPDictionary,
                              kCGImagePropertyWebPDelayTime,
                              kCGImagePropertyWebPDelayTime);
                }
                CFRelease(props);
            }
            d.delays_ms.push_back(std::max(delay_ms, 20));
        }
        if (!d.frames.empty())
        {
            CFRelease(src);
            return d;
        }
        d.delays_ms.clear();
        CFRelease(src);
        CFDataRef data2 = CFDataCreate(kCFAllocatorDefault, bytes.data(),
                                       static_cast<CFIndex>(bytes.size()));
        if (!data2)
        {
            return d;
        }
        src = CGImageSourceCreateWithData(data2, nullptr);
        CFRelease(data2);
        if (!src)
        {
            return d;
        }
    }
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!img)
    {
        return d;
    }
    d.still = tk::cg::make_image(img);
    CGImageRelease(img);
    return d;
}

std::int64_t MacShell::monotonic_ms_()
{
    return static_cast<std::int64_t>([[NSDate date] timeIntervalSince1970] *
                                     1000.0);
}

void MacShell::start_anim_tick_()
{
    if (ctrl_)
    {
        [ctrl_ _startAnimTickIfNeeded];
    }
}

void MacShell::repaint_pickers_()
{
    if (ctrl_)
    {
        [ctrl_ _relayoutChatSurface];
    }
    EmojiPickerPanel* ep = [EmojiPickerPanel sharedPanel];
    if (ep.isVisible)
    {
        [ep invalidateImageCache];
    }
    StickerPickerPanel* sp = [StickerPickerPanel sharedPanel];
    if (sp.isVisible)
    {
        [sp invalidateImageCache];
    }
}

void MacShell::handle_timeline_reset_ui_(
    std::string room_id,
    std::vector<std::unique_ptr<tesseract::Event>> snapshot)
{
    dispatch_timeline_reset_secondary_(room_id, snapshot);

    MainWindowController* c = ctrl_;
    if (!c)
    {
        return;
    }
    std::vector<tesseract::Event*> raw;
    raw.reserve(snapshot.size());
    for (auto& p : snapshot)
    {
        raw.push_back(p.release());
    }
    NSString* rid = [NSString stringWithUTF8String:room_id.c_str()] ?: @"";
    [c handleTimelineReset:rid snapshot:raw];
}

void MacShell::handle_message_inserted_ui_(std::string room_id,
                                           std::size_t index,
                                           std::unique_ptr<tesseract::Event> ev)
{
    if (ev && ev->type != tesseract::EventType::Unhandled)
    {
        dispatch_message_inserted_secondary_(room_id, index, *ev);
    }
    MainWindowController* c = ctrl_;
    if (!c || !ev)
    {
        return;
    }
    NSString* rid = [NSString stringWithUTF8String:room_id.c_str()] ?: @"";
    [c handleMessageInserted:rid index:index event:ev.release()];
}

void MacShell::handle_message_updated_ui_(std::string room_id,
                                          std::size_t index,
                                          std::unique_ptr<tesseract::Event> ev)
{
    if (ev && ev->type != tesseract::EventType::Unhandled)
    {
        dispatch_message_updated_secondary_(room_id, index, *ev);
    }
    MainWindowController* c = ctrl_;
    if (!c || !ev)
    {
        return;
    }
    NSString* rid = [NSString stringWithUTF8String:room_id.c_str()] ?: @"";
    [c handleMessageUpdated:rid index:index event:ev.release()];
}

void MacShell::handle_message_removed_ui_(std::string room_id,
                                          std::size_t index)
{
    dispatch_message_removed_secondary_(room_id, index);
    MainWindowController* c = ctrl_;
    if (!c)
    {
        return;
    }
    NSString* rid = [NSString stringWithUTF8String:room_id.c_str()] ?: @"";
    [c handleMessageRemoved:rid index:index];
}

void MacShell::handle_sync_error_ui_(std::string context,
                                     std::string /*user_id*/,
                                     std::string description, bool soft_logout)
{
    MainWindowController* c = ctrl_;
    if (!c)
    {
        return;
    }
    NSString* ctx = [NSString stringWithUTF8String:context.c_str()] ?: @"";
    NSString* desc = [NSString stringWithUTF8String:description.c_str()] ?: @"";
    [c handleSyncErrorContext:ctx description:desc softLogout:soft_logout];
}

void MacShell::handle_backup_progress_ui_(tesseract::BackupProgress progress)
{
    MainWindowController* c = ctrl_;
    if (c)
    {
        [c handleBackupProgress:progress];
    }
}

void MacShell::refresh_pickers_packs_()
{
    MainWindowController* c = ctrl_;
    if (c)
    {
        [c handleImagePacksUpdated];
    }
}

void MacShell::handle_verification_state_ui_(bool is_verified)
{
    MainWindowController* c = ctrl_;
    if (c)
    {
        [c handleVerificationState:is_verified ? YES : NO];
    }
}

void MacShell::handle_verification_request_ui_(std::string flow_id,
                                               std::string /*user_id*/,
                                               std::string /*device_id*/,
                                               bool incoming)
{
    active_verification_flow_id_ = std::move(flow_id);
    MainWindowController* c = ctrl_;
    if (c)
    {
        [c handleVerificationRequest:active_verification_flow_id_
                            incoming:incoming ? YES : NO];
    }
}

void MacShell::handle_sas_ready_ui_(
    std::string /*flow_id*/, std::vector<tesseract::VerificationEmoji> emojis)
{
    MainWindowController* c = ctrl_;
    if (c)
    {
        [c handleSasReady:std::move(emojis)];
    }
}

void MacShell::handle_verification_done_ui_(std::string /*flow_id*/)
{
    MainWindowController* c = ctrl_;
    if (c)
    {
        [c handleVerificationDone];
    }
}

void MacShell::handle_verification_cancelled_ui_(std::string /*flow_id*/,
                                                 std::string reason)
{
    MainWindowController* c = ctrl_;
    if (c)
    {
        [c handleVerificationCancelled:std::move(reason)];
    }
}

void MacShell::handle_notification_ui_(std::string user_id, std::string room_id,
                                       std::string room_name,
                                       std::string sender, std::string body,
                                       bool is_mention,
                                       std::vector<uint8_t> avatar_bytes,
                                       std::vector<uint8_t> image_bytes)
{
    if (!tesseract::Settings::instance().notifications_enabled)
    {
        return;
    }
    if (!notification_image_allowed_())
    {
        image_bytes.clear();
    }
    MainWindowController* c = ctrl_;
    if (c)
    {
        [c handleNotification:room_id
                     roomName:room_name
                       sender:sender
                         body:body
                       userId:user_id
                    isMention:is_mention
                  avatarBytes:avatar_bytes
                   imageBytes:image_bytes];
    }
}

void MacShell::handle_voice_waveform_ready_ui_(
    std::string room_id, std::string event_id,
    std::vector<std::uint16_t> waveform)
{
    if (room_id != current_room_id_)
        return;
    if (auto* ml = room_view_->message_list())
        ml->update_voice_waveform(event_id, std::move(waveform));
}

void MacShell::on_room_list_state_ui_()
{
    MainWindowController* c = ctrl_;
    if (c)
    {
        [c _onRoomListStateChanged];
    }
}

void MacShell::on_server_info_ready_ui_()
{
    MainWindowController* c = ctrl_;
    if (c)
    {
        [c _onServerInfoReady];
    }
}

void MacShell::update_typing_bar_(const std::string& text, bool /*visible*/)
{
    // Typing-indicator visibility is driven by set_typing_text content inside
    // MessageListView — the visible param is intentionally unused.
    MainWindowController* c = ctrl_;
    if (!c)
    {
        return;
    }
    if (room_view_)
    {
        room_view_->set_typing_text(text);
    }
}

void MacShell::on_url_preview_ready_(
    const std::string& url, const tesseract::Client::UrlPreview& preview)
{
    tesseract::views::UrlPreviewData d;
    d.title = preview.title;
    d.description = preview.description;
    d.image_mxc = preview.image_mxc;
    d.image_w = preview.image_w;
    d.image_h = preview.image_h;
    url_preview_data_.emplace(url, std::move(d));

    if (!preview.image_mxc.empty())
    {
        ensure_media_image_(preview.image_mxc, 64, 64);
    }

    if (room_view_)
    {
        room_view_->notify_url_preview_ready(url);
    }
    if (ctrl_)
    {
        [ctrl_ _relayoutChatSurface];
    }

    for (const auto& [rid, w] : secondary_windows_)
    {
        if (w->room_view())
        {
            w->room_view()->notify_url_preview_ready(url);
            w->request_relayout();
        }
    }
}

void MacShell::on_url_preview_failed_(const std::string& url)
{
    // No card to show (height unchanged) — just release the room-switch
    // gate so it doesn't wait the full timeout on a dead link.
    if (room_view_)
    {
        room_view_->notify_url_preview_ready(url);
    }
    for (const auto& [rid, w] : secondary_windows_)
    {
        if (w->room_view())
        {
            w->room_view()->notify_url_preview_ready(url);
        }
    }
}

tesseract::RoomWindowBase*
MacShell::create_secondary_room_window_(const std::string& room_id)
{
    return tesseract::make_mac_room_window(this, room_id, &url_preview_data_);
}

tk::ThemeMode MacShell::os_color_scheme_() const
{
    NSAppearanceName name = NSApp.effectiveAppearance.name;
    return [name containsString:@"Dark"] ? tk::ThemeMode::Dark
                                         : tk::ThemeMode::Light;
}

void MacShell::apply_theme_ui_(const tk::Theme& t)
{
    if (ctrl_)
    {
        [ctrl_ _applyTheme:t];
    }
    apply_theme_to_secondary_windows_(t);
}

// ── Tab management (ShellBase virtual hooks) ──────────────────────────────────

void MacShell::on_tab_state_changed_ui_()
{
    if (!main_app_)
    {
        return;
    }

    auto* tb = main_app_->tab_bar();
    const bool show_bar = tabs_.size() > 1;
    main_app_->set_tab_bar_visible(show_bar);

    if (tb)
    {
        // Rebuild in tabs_ order so visual order is always stable.
        tb->clear();
        for (const auto& t : tabs_)
        {
            const tk::Image* avatar = nullptr;
            std::string name;
            for (const auto& r : rooms_)
            {
                if (r.id != t.room_id)
                {
                    continue;
                }
                name = r.name;
                if (!r.avatar_url.empty())
                {
                    auto it = tk_avatars_.find(r.avatar_url);
                    if (it != tk_avatars_.end())
                    {
                        avatar = it->second.get();
                    }
                }
                break;
            }
            tb->add_tab(t.room_id, name, avatar);
        }

        if (active_tab_idx_ < tabs_.size())
        {
            tb->set_active(tabs_[active_tab_idx_].room_id);
        }
    }

    if (ctrl_ && active_tab_idx_ < tabs_.size())
    {
        const auto& active = tabs_[active_tab_idx_];
        try_restore_message_cache_(active.room_id);
        [ctrl_ onRoomSelected:active.room_id];
        if (!active.compose_draft.empty())
        {
            [ctrl_ _setComposeDraft:active.compose_draft];
        }
    }

    if (app_surface_)
    {
        app_surface_->relayout();
    }
}

float MacShell::get_message_scroll_fraction_()
{
    if (!room_view_ || !room_view_->message_list())
    {
        return 0.f;
    }
    return room_view_->message_list()->scroll_fraction();
}

void MacShell::set_message_scroll_fraction_(float t)
{
    if (!room_view_ || !room_view_->message_list())
    {
        return;
    }
    room_view_->message_list()->scroll_to_offset(t);
}

std::string MacShell::get_compose_draft_()
{
    if (!room_view_ || !room_view_->compose_bar())
    {
        return {};
    }
    return room_view_->compose_bar()->current_text();
}

void MacShell::set_compose_draft_(const std::string& draft)
{
    if (ctrl_)
    {
        [ctrl_ _setComposeDraft:draft];
    }
}

const std::vector<tesseract::views::MessageRowData>*
MacShell::get_current_messages_()
{
    auto* ml = room_view_ ? room_view_->message_list() : nullptr;
    return ml ? &ml->messages() : nullptr;
}

void MacShell::apply_cached_messages_(
    const std::vector<tesseract::views::MessageRowData>& msgs)
{
    if (room_view_)
    {
        room_view_->set_messages(msgs, /*room_switch=*/false);
    }
    if (app_surface_)
    {
        app_surface_->relayout();
    }
}

// ─────────────────────────────────────────────────────────────────────────────

} // namespace

// ─────────────────────────────────────────────────────────────────────────

@implementation MainWindowController
{
    // MacShell owns all multi-account state, image caches, worker threads,
    // and the EventHandlerBase bridges. It is created before _buildChrome.
    std::unique_ptr<MacShell> _shell;

    // When non-empty, the next emoji selection routes through
    // send_reaction for this event id (set by the "+" reaction chip).
    std::string _pendingReactionEventId;

    // Branding splash shown before the session check completes.
    std::unique_ptr<tk::macos::Surface> _brandingSurface;

    // Single surface hosting the full main-app widget tree.
    std::unique_ptr<tk::macos::Surface> _mainAppSurface;
    tesseract::views::MainAppWidget* _mainApp; // borrowed from root
    std::string _pendingSearchText;

    // Settings surface — full-window sibling of mainAppView and _loginView.
    std::unique_ptr<tk::macos::Surface> _settingsSurface;
    tesseract::views::SettingsView*
        _settingsView; // borrowed from _settingsSurface root

    // Native overlay fields positioned via _mainAppSurface->set_on_layout().
    std::unique_ptr<tk::NativeTextField> _roomSearchField;
    std::unique_ptr<tk::NativeTextArea> _roomTextArea;
    std::unique_ptr<tk::NativeTextArea> _topicTextArea;
    std::unique_ptr<tk::NativeTextField> _recoveryKeyField;

    // Settings name field — positioned via _settingsSurface->set_on_layout().
    std::unique_ptr<tk::NativeTextField> _settingsNameField;

    // Borrowed sub-view aliases (set after building _mainAppSurface).
    tesseract::views::RoomListView* _roomListView;      // via _mainApp
    tesseract::views::RoomView* _roomView;              // via _mainApp
    tesseract::views::RecoveryBanner* _recoveryShared;  // via _mainApp
    tesseract::views::VerificationBanner* _verifShared; // via _mainApp
    tesseract::views::ImageViewerOverlay* _imgViewer;   // via _mainApp
    tesseract::views::VideoViewerOverlay* _vidViewer;   // via _mainApp

    // Shortcode suggestion popup — NSPanel hosting a tk::macos::Surface.
    NSPanel* _shortcodePanel;
    std::unique_ptr<tk::macos::Surface> _shortcodePopupSurface;
    tesseract::views::ShortcodePopup*
        _shortcodePopupWidget; // borrowed from root

    // AppKit chrome.
    LoginView* _loginView;

    NSTimer* _animTimer;
    NSTimer* _markReadTimer;

    // System-tray icon (menu-bar status item). Created after login; nil
    // until then. When non-nil and `is_available()`, closing the window
    // hides it instead of terminating the app.
    std::unique_ptr<MacOSTrayIcon> _tray;

    id _escapeMonitor;

    // Right-click context menu sticker state.
    std::string _ctxStickerEventId;
    std::string _ctxStickerMxcUrl;
    std::string _ctxStickerBody;
    std::string _ctxStickerInfoJson;

    // Account picker popover (left-click on user strip).
    NSPopover* _accountPickerPopover;
    std::unique_ptr<tk::macos::Surface> _accountPickerSurface;
    tesseract::views::AccountPicker* _accountPickerShared; // borrowed

    // Floating tooltip popover for truncated room topics.
    NSPopover* _topicTooltipPopover;

    // Dock-bounce token; 0 = no request in flight.
    NSInteger _attentionRequestToken;
}

- (instancetype)init
{
    NSRect frame = NSMakeRect(0, 0, 1100, 768);
    NSWindowStyleMask mask =
        NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    NSWindow* window =
        [[NSWindow alloc] initWithContentRect:frame
                                    styleMask:mask
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    window.title = @"Tesseract";
    window.minSize = NSMakeSize(720, 480);
    window.titlebarAppearsTransparent = NO;
    window.releasedWhenClosed = NO;

    self = [super initWithWindow:window];
    if (!self)
    {
        return nil;
    }

    _shell = std::make_unique<MacShell>(self);
    _shell->set_screen_lock_(std::make_unique<mac::MacScreenLock>());
    _accountPickerShared = nullptr;
    window.delegate = self;
    [self _buildChrome];

    // Load saved theme preference and apply it to all surfaces.
    tesseract::Settings::instance().load_from_disk(tesseract::config_dir());
    _shell->apply_current_theme_();

    // Re-apply when the OS switches light/dark (only in System mode).
    [NSApp addObserver:self
            forKeyPath:@"effectiveAppearance"
               options:NSKeyValueObservingOptionNew
               context:nil];

    return self;
}

// Intercept the red traffic-light / Cmd-W. If the tray icon is up, hide the
// window instead of closing it; the user can bring it back via the menu-bar
// item. Returns NO to swallow the close.
- (BOOL)windowShouldClose:(NSWindow*)sender
{
    if (_tray && _tray->is_available())
    {
        [sender orderOut:nil];
        return NO;
    }
    return YES;
}

- (void)_buildChrome
{
    NSView* content = self.window.contentView;
    content.wantsLayer = YES;

    // ── Single surface hosting the full main-app widget tree ──────────
    _mainAppSurface = std::make_unique<tk::macos::Surface>(tk::Theme::light());
    {
        auto main_app_owner =
            std::make_unique<tesseract::views::MainAppWidget>();
        _mainApp = main_app_owner.get();

        // Wire borrowed sub-view aliases.
        _roomListView = _mainApp->room_list_view();
        _roomView = _mainApp->room_view();
        _recoveryShared = _mainApp->recovery_banner();
        _verifShared = _mainApp->verif_banner();
        _imgViewer = _mainApp->image_viewer();
        _vidViewer = _mainApp->video_viewer();
        _shell->room_view_ = _roomView;
        _shell->main_app_ = _mainApp;
        _shell->app_surface_ = _mainAppSurface.get();

        // Space nav callback.
        _mainApp->on_space_back = [self]
        {
            [self _onSpaceBack];
        };

        // UserInfo callbacks: left-click → account picker, right-click → context menu.
        __weak MainWindowController* weakSelf = self;
        _mainApp->user_info()->set_image_provider(
            [weakSelf](const std::string& mxc) -> const tk::Image*
            {
                MainWindowController* s = weakSelf;
                if (!s)
                {
                    return nullptr;
                }
                auto it = s->_shell->tk_avatars_.find(mxc);
                return it == s->_shell->tk_avatars_.end() ? nullptr
                                                          : it->second.get();
            });
        _mainApp->user_info()->on_primary = [weakSelf](tk::Point /*p*/)
        {
            MainWindowController* s = weakSelf;
            if (s)
            {
                [s _openAccountPicker];
            }
        };
        _mainApp->user_info()->on_secondary = [weakSelf](tk::Point p)
        {
            MainWindowController* s = weakSelf;
            if (!s)
            {
                return;
            }
            NSString* logoutTitle = [NSString
                stringWithFormat:
                    @"Log Out %@",
                    s->_shell->my_display_name_.empty()
                        ? [NSString stringWithUTF8String:s->_shell->my_user_id_
                                                             .c_str()]
                        : [NSString
                              stringWithUTF8String:s->_shell->my_display_name_
                                                       .c_str()]];
            NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
            [menu addItemWithTitle:@"Settings…"
                            action:@selector(_openSettings)
                     keyEquivalent:@""];
            [menu addItemWithTitle:@"Add Account…"
                            action:@selector(_beginAddAccount)
                     keyEquivalent:@""];
            [menu addItemWithTitle:logoutTitle
                            action:@selector(_logoutActiveAccount)
                     keyEquivalent:@""];
            [menu addItem:[NSMenuItem separatorItem]];
            [menu addItemWithTitle:@"Quit"
                            action:@selector(terminate:)
                     keyEquivalent:@""];
            NSView* view = (__bridge NSView*)s->_mainAppSurface->view_handle();
            NSPoint local = NSMakePoint(p.x, p.y);
            NSPoint screen = [view.window
                convertPointToScreen:[view convertPoint:local toView:nil]];
            [menu popUpMenuPositioningItem:nil atLocation:screen inView:nil];
        };

        // TabBar callbacks.
        _mainApp->tab_bar()->on_tab_selected =
            [weakSelf](const std::string& room_id)
        {
            MainWindowController* s = weakSelf;
            if (s)
            {
                s->_shell->tab_select_room(room_id);
            }
        };
        _mainApp->tab_bar()->on_tab_closed =
            [weakSelf](const std::string& room_id)
        {
            MainWindowController* s = weakSelf;
            if (s)
            {
                s->_shell->tab_close(room_id);
            }
        };

        // Space nav + RoomListView avatar providers.
        _mainApp->set_avatar_provider(
            [weakSelf](const std::string& mxc) -> const tk::Image*
            {
                MainWindowController* s = weakSelf;
                if (!s)
                {
                    return nullptr;
                }
                auto it = s->_shell->tk_avatars_.find(mxc);
                return it == s->_shell->tk_avatars_.end() ? nullptr
                                                          : it->second.get();
            });
        _mainApp->room_list_view()->set_avatar_provider(
            [weakSelf](const std::string& mxc) -> const tk::Image*
            {
                MainWindowController* s = weakSelf;
                if (!s)
                {
                    return nullptr;
                }
                auto it = s->_shell->tk_avatars_.find(mxc);
                return it == s->_shell->tk_avatars_.end() ? nullptr
                                                          : it->second.get();
            });
        _mainApp->room_list_view()->set_sticker_provider(
            [weakSelf](const std::string& mxc) -> const tk::Image*
            {
                MainWindowController* s = weakSelf;
                if (!s)
                {
                    return nullptr;
                }
                if (const auto* f = s->_shell->anim_cache_.current_frame(mxc))
                {
                    return f;
                }
                auto it = s->_shell->tk_images_.find(mxc);
                if (it != s->_shell->tk_images_.end())
                {
                    return it->second.get();
                }
                s->_shell->ensure_media_image_(mxc, 64, 64);
                return nullptr;
            });
        _mainApp->room_list_view()->on_room_selected =
            [weakSelf](const std::string& room_id)
        {
            MainWindowController* s = weakSelf;
            if (!s)
            {
                return;
            }
            NSEventModifierFlags mods = [NSEvent modifierFlags];
            if (mods & NSEventModifierFlagCommand)
            {
                s->_shell->tab_open_room(room_id);
            }
            else
            {
                s->_shell->tab_select_room(room_id);
            }
        };
        _mainApp->room_list_view()->on_scroll = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (!s)
            {
                return;
            }
            [NSObject
                cancelPreviousPerformRequestsWithTarget:s
                                               selector:
                                                   @selector(
                                                       _onRoomScrollDebounce)
                                                 object:nil];
            [s performSelector:@selector(_onRoomScrollDebounce)
                    withObject:nil
                    afterDelay:0.3];
        };

        // RecoveryBanner callbacks.
        _mainApp->recovery_banner()->on_verify =
            [weakSelf](const std::string& /*key*/)
        {
            MainWindowController* s = weakSelf;
            if (s)
            {
                [s _onRecoveryVerify];
            }
        };
        _mainApp->recovery_banner()->on_dismiss = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (s)
            {
                [s _onRecoveryDismiss];
            }
        };

        // VerificationBanner callbacks.
        _mainApp->verif_banner()->on_verify = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (s && s->_shell->client_)
            {
                s->_shell->client_->request_self_verification();
            }
        };
        _mainApp->verif_banner()->on_accept = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (!s || !s->_shell->client_)
            {
                return;
            }
            s->_shell->client_->accept_verification(
                s->_shell->active_verification_flow_id_);
            s->_shell->client_->start_sas(
                s->_shell->active_verification_flow_id_);
        };
        _mainApp->verif_banner()->on_match = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (!s || !s->_shell->client_)
            {
                return;
            }
            s->_shell->client_->confirm_sas(
                s->_shell->active_verification_flow_id_);
            if (s->_verifShared)
            {
                s->_verifShared->set_state(
                    tesseract::views::VerificationBanner::State::Confirming);
            }
            if (s->_mainAppSurface)
            {
                s->_mainAppSurface->relayout();
            }
        };
        _mainApp->verif_banner()->on_mismatch = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (s && s->_shell->client_)
            {
                s->_shell->client_->cancel_verification(
                    s->_shell->active_verification_flow_id_);
            }
        };
        _mainApp->verif_banner()->on_cancel = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (s && s->_shell->client_)
            {
                s->_shell->client_->cancel_verification(
                    s->_shell->active_verification_flow_id_);
            }
        };
        _mainApp->verif_banner()->on_dismiss = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (!s)
            {
                return;
            }
            s->_shell->verification_banner_dismissed_ = true;
            s->_mainApp->show_verif_banner(false);
            s->_mainAppSurface->relayout();
        };
        _mainApp->verif_banner()->on_done = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (!s)
            {
                return;
            }
            s->_mainApp->show_verif_banner(false);
            s->_mainAppSurface->relayout();
        };
        _mainApp->verif_banner()->on_use_recovery_key = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (!s)
            {
                return;
            }
            s->_mainApp->show_verif_banner(false);
            s->_mainAppSurface->relayout();
            [s _maybeShowRecoveryBanner];
        };

        // ImageViewerOverlay callbacks.
        _mainApp->image_viewer()->set_image_provider(
            [weakSelf](const std::string& url) -> const tk::Image*
            {
                MainWindowController* s = weakSelf;
                if (!s)
                {
                    return nullptr;
                }
                if (auto* f = s->_shell->anim_cache_.current_frame(url))
                {
                    return f;
                }
                auto it = s->_shell->tk_images_.find(url);
                return it == s->_shell->tk_images_.end() ? nullptr
                                                         : it->second.get();
            });
        _mainApp->image_viewer()->on_close = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (!s)
            {
                return;
            }
            s->_mainApp->show_image_viewer(false);
            s->_mainAppSurface->relayout();
            // Restore keyboard focus to the compose bar now that the overlay
            // is gone and the NSTextView overlay is visible again.
            if (!s->_mainApp->compose_text_area_rect().empty())
            {
                s->_roomTextArea->set_focused(true);
            }
        };
        _mainApp->image_viewer()->set_repaint_requester(
            [weakSelf]
            {
                MainWindowController* s = weakSelf;
                if (s && s->_mainAppSurface)
                {
                    s->_mainAppSurface->relayout();
                }
            });
        _mainApp->image_viewer()->on_save =
            [weakSelf](std::string source_url, std::string filename_hint)
        {
            MainWindowController* s = weakSelf;
            if (!s)
                return;
            NSSavePanel* panel = [NSSavePanel savePanel];
            NSString* suggested = filename_hint.empty()
                ? @"image"
                : [NSString stringWithUTF8String:filename_hint.c_str()];
            panel.nameFieldStringValue = suggested;
            NSModalResponse resp = [panel runModal];
            if (resp != NSModalResponseOK || !panel.URL)
                return;
            std::string dest = panel.URL.path.UTF8String;
            auto bytes_holder = std::make_shared<std::vector<uint8_t>>();
            s->_shell->run_async_(
                [weakSelf, source_url = std::move(source_url), dest,
                 bytes_holder, clientPtr = s->_shell->client_]()
                {
                    *bytes_holder = clientPtr->fetch_source_bytes(source_url);
                    dispatch_async(dispatch_get_main_queue(), ^{
                        if (bytes_holder->empty())
                            return;
                        std::ofstream f(dest, std::ios::binary);
                        f.write(reinterpret_cast<const char*>(bytes_holder->data()),
                                static_cast<std::streamsize>(bytes_holder->size()));
                    });
                });
        };

        // VideoViewerOverlay callbacks.
        _mainApp->video_viewer()->set_image_provider(
            [weakSelf](const std::string& url) -> const tk::Image*
            {
                MainWindowController* s = weakSelf;
                if (!s)
                {
                    return nullptr;
                }
                auto it = s->_shell->tk_images_.find(url);
                return it == s->_shell->tk_images_.end() ? nullptr
                                                         : it->second.get();
            });
        _mainApp->video_viewer()->set_video_player(
            _mainAppSurface->host().make_video_player());
        _mainApp->video_viewer()->set_repaint_requester(
            [weakSelf]
            {
                MainWindowController* s = weakSelf;
                if (s && s->_mainAppSurface)
                {
                    s->_mainAppSurface->relayout();
                }
            });
        _mainApp->video_viewer()->on_close = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (!s)
            {
                return;
            }
            s->_mainApp->show_video_viewer(false);
            s->_mainAppSurface->relayout();
            if (!s->_mainApp->compose_text_area_rect().empty())
            {
                s->_roomTextArea->set_focused(true);
            }
        };
        _mainApp->video_viewer()->on_save =
            [weakSelf](std::string source_json, std::string mime_type)
        {
            MainWindowController* s = weakSelf;
            if (!s)
                return;
            std::string ext = ".mp4";
            auto slash = mime_type.find('/');
            if (slash != std::string::npos)
                ext = "." + mime_type.substr(slash + 1);
            NSSavePanel* panel = [NSSavePanel savePanel];
            panel.nameFieldStringValue =
                [NSString stringWithUTF8String:("video" + ext).c_str()];
            NSModalResponse resp = [panel runModal];
            if (resp != NSModalResponseOK || !panel.URL)
                return;
            std::string dest = panel.URL.path.UTF8String;
            auto bytes_holder = std::make_shared<std::vector<uint8_t>>();
            s->_shell->run_async_(
                [weakSelf, source_json = std::move(source_json), dest,
                 bytes_holder, clientPtr = s->_shell->client_]()
                {
                    *bytes_holder = clientPtr->fetch_source_bytes(source_json);
                    dispatch_async(dispatch_get_main_queue(), ^{
                        if (bytes_holder->empty())
                            return;
                        std::ofstream f(dest, std::ios::binary);
                        f.write(reinterpret_cast<const char*>(bytes_holder->data()),
                                static_cast<std::streamsize>(bytes_holder->size()));
                    });
                });
        };

        // RoomView callbacks.
        _mainApp->room_view()->set_avatar_provider(
            [weakSelf](const std::string& mxc) -> const tk::Image*
            {
                MainWindowController* s = weakSelf;
                if (!s)
                {
                    return nullptr;
                }
                auto it = s->_shell->tk_avatars_.find(mxc);
                return it == s->_shell->tk_avatars_.end() ? nullptr
                                                          : it->second.get();
            });
        _mainApp->room_view()->set_image_provider(
            [weakSelf](const std::string& mxc) -> const tk::Image*
            {
                MainWindowController* s = weakSelf;
                if (!s)
                {
                    return nullptr;
                }
                if (auto* f = s->_shell->anim_cache_.current_frame(mxc))
                {
                    return f;
                }
                auto it = s->_shell->tk_images_.find(mxc);
                return it == s->_shell->tk_images_.end() ? nullptr
                                                         : it->second.get();
            });
        _mainApp->room_view()->set_preview_provider(
            [weakSelf](const std::string& url)
                -> const tesseract::views::UrlPreviewData*
            {
                MainWindowController* s = weakSelf;
                if (!s)
                {
                    return nullptr;
                }
                auto it = s->_shell->url_preview_data_.find(url);
                if (it == s->_shell->url_preview_data_.end())
                {
                    return nullptr;
                }
                if (!it->second.image_mxc.empty() &&
                    !s->_shell->tk_images_.count(it->second.image_mxc) &&
                    !s->_shell->anim_cache_.has(it->second.image_mxc))
                {
                    s->_shell->ensure_media_image_(it->second.image_mxc, 64,
                                                   64);
                }
                return &it->second;
            });
        _mainApp->room_view()->set_voice_bytes_provider(
            [weakSelf](
                const std::string& source_json) -> std::vector<std::uint8_t>
            {
                MainWindowController* s = weakSelf;
                if (!s)
                {
                    return {};
                }
                return s->_shell->client_->fetch_source_bytes(source_json);
            });
        _mainApp->room_view()->set_video_player_factory(
            [weakSelf]() -> std::unique_ptr<tk::VideoPlayer>
            {
                MainWindowController* s = weakSelf;
                if (!s || !s->_mainAppSurface)
                {
                    return nullptr;
                }
                return s->_mainAppSurface->host().make_video_player();
            });
        _mainApp->room_view()->set_video_fetch_provider(
            [weakSelf](const std::string& src,
                       std::function<void(std::vector<std::uint8_t>)> on_ready)
            {
                MainWindowController* s = weakSelf;
                if (!s)
                {
                    return;
                }
                auto bytes_holder = std::make_shared<std::vector<uint8_t>>();
                s->_shell->run_async_(
                    [weakSelf, src, bytes_holder,
                     on_ready = std::move(on_ready),
                     clientPtr = s->_shell->client_]() mutable
                    {
                        *bytes_holder = clientPtr->fetch_source_bytes(src);
                        dispatch_async(dispatch_get_main_queue(), ^{
                            on_ready(std::move(*bytes_holder));
                        });
                    });
            });
        if (auto player = _mainAppSurface->host().make_audio_player())
        {
            _mainApp->room_view()->set_audio_player(std::move(player));
        }
        _shell->capture_ = _mainAppSurface->host().make_audio_capture();
        {
            __weak MainWindowController* ws = self;
            _shell->wire_voice_capture_(
                _mainApp->room_view(),
                [ws]() { if (ws) [ws _relayoutChatSurface]; },
                [shell = _shell.get()]() { return shell->current_room_id_; },
                [ws]()
                {
                    MainWindowController* s = ws;
                    if (!s) return;
                    if (s->_roomTextArea)
                        s->_roomTextArea->set_text("");
                    if (s->_roomView)
                        s->_roomView->set_current_text({});
                });
        }

        _mainApp->room_view()->on_send = [weakSelf](const std::string& body)
        {
            MainWindowController* s = weakSelf;
            if (!s || s->_shell->current_room_id_.empty())
            {
                return;
            }
            std::string trimmed = trim(body);
            if (trimmed.empty())
            {
                return;
            }
            if (s->_shell->client_->send_message(s->_shell->current_room_id_,
                                                 trimmed))
            {
                if (s->_roomTextArea)
                {
                    s->_roomTextArea->set_text("");
                }
                if (s->_roomView)
                {
                    s->_roomView->set_current_text({});
                }
            }
        };
        _mainApp->room_view()->on_send_reply =
            [weakSelf](const std::string& reply_event_id,
                       const std::string& body)
        {
            MainWindowController* s = weakSelf;
            if (!s || body.empty() || s->_shell->current_room_id_.empty())
            {
                return;
            }
            s->_shell->client_->send_reply(s->_shell->current_room_id_,
                                           reply_event_id, body);
            if (s->_roomTextArea)
            {
                s->_roomTextArea->set_text("");
            }
            if (s->_roomView)
            {
                s->_roomView->set_current_text({});
            }
        };
        _mainApp->room_view()->on_send_edit =
            [weakSelf](const std::string& event_id, const std::string& new_body)
        {
            MainWindowController* s = weakSelf;
            if (!s || new_body.empty() || s->_shell->current_room_id_.empty())
            {
                return;
            }
            s->_shell->client_->send_edit(s->_shell->current_room_id_, event_id,
                                          new_body);
            if (s->_roomTextArea)
            {
                s->_roomTextArea->set_text("");
            }
            if (s->_roomView)
            {
                s->_roomView->set_current_text({});
            }
        };
        _mainApp->room_view()->on_send_image =
            [weakSelf](std::vector<std::uint8_t> bytes, std::string mime,
                       std::string filename, std::string caption, int src_w,
                       int src_h, bool is_animated, std::string reply_event_id)
        {
            MainWindowController* s = weakSelf;
            if (!s)
            {
                return;
            }
            [s _sendComposedImage:std::move(bytes)
                             mime:std::move(mime)
                         filename:std::move(filename)
                          caption:std::move(caption)
                            width:static_cast<std::uint32_t>(
                                      src_w < 0 ? 0 : src_w)
                           height:static_cast<std::uint32_t>(
                                      src_h < 0 ? 0 : src_h)
                       isAnimated:is_animated
                     replyEventId:std::move(reply_event_id)];
        };
        _mainApp->room_view()->on_send_video =
            [weakSelf](std::vector<std::uint8_t> bytes, std::string mime,
                       std::string filename, std::string caption, int w, int h,
                       std::vector<std::uint8_t> thumb_bytes, int thumb_w,
                       int thumb_h, std::uint64_t duration_ms,
                       std::string reply_event_id)
        {
            MainWindowController* s = weakSelf;
            if (!s)
            {
                return;
            }
            [s _sendComposedVideo:std::move(bytes)
                             mime:std::move(mime)
                         filename:std::move(filename)
                          caption:std::move(caption)
                            width:static_cast<std::uint32_t>(w < 0 ? 0 : w)
                           height:static_cast<std::uint32_t>(h < 0 ? 0 : h)
                       thumbBytes:std::move(thumb_bytes)
                       thumbWidth:static_cast<std::uint32_t>(
                                      thumb_w < 0 ? 0 : thumb_w)
                      thumbHeight:static_cast<std::uint32_t>(
                                      thumb_h < 0 ? 0 : thumb_h)
                       durationMs:duration_ms
                     replyEventId:std::move(reply_event_id)];
        };
        _mainApp->room_view()->on_send_audio =
            [weakSelf](std::vector<std::uint8_t> bytes, std::string mime,
                       std::string filename, std::string caption,
                       std::uint64_t duration_ms, std::string reply_event_id)
        {
            MainWindowController* s = weakSelf;
            if (!s)
            {
                return;
            }
            [s _sendComposedAudio:std::move(bytes)
                             mime:std::move(mime)
                         filename:std::move(filename)
                          caption:std::move(caption)
                       durationMs:duration_ms
                     replyEventId:std::move(reply_event_id)];
        };
        _mainApp->room_view()->on_send_file =
            [weakSelf](std::vector<std::uint8_t> bytes, std::string mime,
                       std::string filename, std::string caption,
                       std::string reply_event_id)
        {
            MainWindowController* s = weakSelf;
            if (!s)
            {
                return;
            }
            [s _sendComposedFile:std::move(bytes)
                            mime:std::move(mime)
                        filename:std::move(filename)
                         caption:std::move(caption)
                    replyEventId:std::move(reply_event_id)];
        };
        _mainApp->room_view()->on_delete_requested =
            [weakSelf](const std::string& event_id)
        {
            MainWindowController* s = weakSelf;
            if (!s || s->_shell->current_room_id_.empty())
            {
                return;
            }
            s->_shell->client_->redact_event(s->_shell->current_room_id_,
                                             event_id);
        };
        _mainApp->room_view()->on_reaction_toggled =
            [weakSelf](const std::string& event_id, const std::string& key)
        {
            MainWindowController* s = weakSelf;
            if (!s || s->_shell->current_room_id_.empty())
            {
                return;
            }
            s->_shell->client_->send_reaction(s->_shell->current_room_id_,
                                              event_id, key);
        };
        _mainApp->room_view()->on_add_reaction_requested =
            [weakSelf](const std::string& event_id, tk::Rect anchor)
        {
            MainWindowController* s = weakSelf;
            if (!s || s->_shell->current_room_id_.empty())
            {
                return;
            }
            s->_pendingReactionEventId = event_id;
            [s showEmojiPickerAtRect:anchor];
        };
        _mainApp->room_view()->on_receipt_needed =
            [weakSelf](const std::string& eid)
        {
            MainWindowController* s = weakSelf;
            if (!s)
            {
                return;
            }
            s->_shell->maybe_send_read_receipt_(s->_shell->current_room_id_,
                                                eid);
        };
        _mainApp->room_view()->message_list()->on_tile_needed =
            [weakSelf](int z, int x, int y)
        {
            MainWindowController* s = weakSelf;
            if (s)
            {
                s->_shell->ensure_tile_async(z, x, y);
            }
        };
        _mainApp->room_view()->message_list()->on_show_copy_menu =
            [weakSelf]()
        {
            MainWindowController* s = weakSelf;
            if (!s || !s->_mainApp || !s->_mainAppSurface)
                return;
            auto* ml = s->_mainApp->room_view()->message_list();
            NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
            NSMenuItem* item = [[NSMenuItem alloc]
                initWithTitle:NSLocalizedString(@"Copy", nil)
                       action:@selector(copy:)
                keyEquivalent:@""];
            [menu addItem:item];
            NSEvent* event = [NSApp currentEvent];
            NSView* view =
                (__bridge NSView*)s->_mainAppSurface->view_handle();
            if (event && view)
                [NSMenu popUpContextMenu:menu withEvent:event forView:view];
        };
        _mainApp->room_view()->on_image_clicked =
            [weakSelf](const tesseract::views::MessageListView::ImageHit& hit)
        {
            MainWindowController* s = weakSelf;
            if (!s || !s->_mainApp || !s->_mainAppSurface)
            {
                return;
            }
            const std::string src_tok   = hit.source    ? hit.source->fetch_token()    : std::string{};
            const std::string thumb_tok = hit.thumbnail ? hit.thumbnail->fetch_token() : std::string{};
            s->_imgViewer->open(src_tok, thumb_tok, hit.body,
                                hit.natural_w, hit.natural_h);
            s->_mainApp->show_image_viewer(true);
            s->_mainAppSurface->relayout();
            s->_shell->ensure_media_image_(src_tok,
                                           tesseract::visual::kMaxInlineImageWidth,
                                           tesseract::visual::kMaxInlineImageHeight);
            NSView* view = (__bridge NSView*)s->_mainAppSurface->view_handle();
            [view.window makeFirstResponder:view];
        };
        _mainApp->room_view()->on_video_clicked =
            [weakSelf](const tesseract::views::MessageListView::VideoHit& hit)
        {
            MainWindowController* s = weakSelf;
            if (!s || !s->_mainApp || !s->_mainAppSurface)
            {
                return;
            }
            const std::string src_tok   = hit.source    ? hit.source->fetch_token()    : std::string{};
            const std::string thumb_tok = hit.thumbnail ? hit.thumbnail->fetch_token() : std::string{};
            s->_vidViewer->open(src_tok, thumb_tok,
                                hit.mime_type, hit.duration_ms, hit.natural_w,
                                hit.natural_h, hit.autoplay, hit.loop,
                                hit.no_audio, hit.hide_controls);
            s->_mainApp->show_video_viewer(true);
            s->_mainAppSurface->relayout();
            NSView* view = (__bridge NSView*)s->_mainAppSurface->view_handle();
            [view.window makeFirstResponder:view];
            auto bytes_holder = std::make_shared<std::vector<uint8_t>>();
            std::string src = src_tok;
            s->_shell->run_async_(
                [weakSelf, src, bytes_holder, clientPtr = s->_shell->client_]()
                {
                    *bytes_holder = clientPtr->fetch_source_bytes(src);
                    dispatch_async(dispatch_get_main_queue(), ^{
                        MainWindowController* s2 = weakSelf;
                        if (!s2 || !s2->_vidViewer)
                        {
                            return;
                        }
                        s2->_vidViewer->load_bytes(bytes_holder->data(),
                                                   bytes_holder->size());
                    });
                });
        };
        _mainApp->room_view()->on_file_clicked =
            [weakSelf](const tesseract::views::MessageListView::FileHit& hit)
        {
            MainWindowController* s = weakSelf;
            if (!s || !s->_mainApp || !s->_mainAppSurface)
                return;
            NSSavePanel* panel = [NSSavePanel savePanel];
            NSString* suggested = hit.file_name.empty()
                ? @"download"
                : [NSString stringWithUTF8String:hit.file_name.c_str()];
            panel.nameFieldStringValue = suggested;
            NSModalResponse resp = [panel runModal];
            if (resp != NSModalResponseOK || !panel.URL)
                return;
            std::string dest = panel.URL.path.UTF8String;
            std::string url  = hit.source ? hit.source->fetch_token() : std::string{};
            auto bytes_holder = std::make_shared<std::vector<uint8_t>>();
            s->_shell->run_async_(
                [weakSelf, url, dest, bytes_holder, clientPtr = s->_shell->client_]()
                {
                    *bytes_holder = clientPtr->fetch_source_bytes(url);
                    dispatch_async(dispatch_get_main_queue(), ^{
                        if (bytes_holder->empty())
                            return;
                        std::ofstream f(dest, std::ios::binary);
                        f.write(reinterpret_cast<const char*>(bytes_holder->data()),
                                static_cast<std::streamsize>(bytes_holder->size()));
                    });
                });
        };
        _mainApp->room_view()->on_link_clicked = [](const std::string& url)
        {
            tesseract::Client::open_in_browser(url);
        };
        {
            __weak MainWindowController* weakSelf = self;
            _mainApp->room_view()->on_set_clipboard = [weakSelf](std::string_view t)
            {
                MainWindowController* s = weakSelf;
                if (s && s->_mainAppSurface)
                    s->_mainAppSurface->host().set_clipboard_text(t);
            };
        }
        {
            auto hovered = std::make_shared<bool>(false);
            _mainApp->room_view()->on_link_hovered =
                [hovered](const std::string& url)
            {
                if (!url.empty() && !*hovered)
                {
                    [[NSCursor pointingHandCursor] push];
                    *hovered = true;
                }
                else if (url.empty() && *hovered)
                {
                    [NSCursor pop];
                    *hovered = false;
                }
            };
        }
        _mainApp->room_view()->on_show_tooltip =
            [weakSelf](std::string text, tk::Rect anchor)
        {
            MainWindowController* s = weakSelf;
            if (!s || !s->_mainAppSurface)
            {
                return;
            }
            if (!s->_topicTooltipPopover)
            {
                NSTextField* lbl = [NSTextField wrappingLabelWithString:@""];
                lbl.translatesAutoresizingMaskIntoConstraints = NO;
                NSView* cv = [[NSView alloc] init];
                cv.translatesAutoresizingMaskIntoConstraints = NO;
                [cv addSubview:lbl];
                [NSLayoutConstraint activateConstraints:@[
                    [lbl.leadingAnchor constraintEqualToAnchor:cv.leadingAnchor
                                                      constant:8],
                    [lbl.trailingAnchor
                        constraintEqualToAnchor:cv.trailingAnchor
                                       constant:-8],
                    [lbl.topAnchor constraintEqualToAnchor:cv.topAnchor
                                                  constant:6],
                    [lbl.bottomAnchor constraintEqualToAnchor:cv.bottomAnchor
                                                     constant:-6],
                    [cv.widthAnchor constraintLessThanOrEqualToConstant:360],
                ]];
                NSViewController* vc = [[NSViewController alloc] init];
                vc.view = cv;
                NSPopover* pop = [[NSPopover alloc] init];
                pop.contentViewController = vc;
                pop.behavior = NSPopoverBehaviorTransient;
                pop.animates = NO;
                s->_topicTooltipPopover = pop;
            }
            NSTextField* lbl =
                (NSTextField*)s->_topicTooltipPopover.contentViewController.view
                    .subviews.firstObject;
            lbl.stringValue = [NSString stringWithUTF8String:text.c_str()];
            [s->_topicTooltipPopover.contentViewController
                    .view layoutSubtreeIfNeeded];
            s->_topicTooltipPopover.contentSize =
                [s->_topicTooltipPopover.contentViewController
                        .view fittingSize];
            NSView* view = (__bridge NSView*)s->_mainAppSurface->view_handle();
            CGFloat viewH = view.bounds.size.height;
            NSRect anchorRect = NSMakeRect(
                anchor.x, viewH - anchor.y - anchor.h, anchor.w, anchor.h);
            [s->_topicTooltipPopover showRelativeToRect:anchorRect
                                                 ofView:view
                                          preferredEdge:NSRectEdgeMinY];
        };
        _mainApp->room_view()->on_hide_tooltip = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (s && s->_topicTooltipPopover && s->_topicTooltipPopover.shown)
            {
                [s->_topicTooltipPopover close];
            }
        };
        _mainApp->room_view()->on_near_top = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (!s || s->_shell->current_room_id_.empty())
            {
                return;
            }
            [s requestMoreHistoryForRoom:s->_shell->current_room_id_];
        };
        _mainApp->room_view()->on_near_bottom = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (!s || s->_shell->current_room_id_.empty())
            {
                return;
            }
            s->_shell->request_forward_history_(s->_shell->current_room_id_);
        };
        _mainApp->room_view()->on_return_to_live = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (!s || s->_shell->current_room_id_.empty())
            {
                return;
            }
            s->_shell->return_to_live_(s->_shell->current_room_id_);
        };
        _mainApp->room_view()->on_scroll_to_original =
            [weakSelf](const std::string& event_id)
        {
            MainWindowController* s = weakSelf;
            if (!s || s->_shell->current_room_id_.empty())
            {
                return;
            }
            std::string room = s->_shell->current_room_id_;
            s->_shell->begin_focused_subscription_(room, event_id);
            dispatch_async(
                dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                    MainWindowController* s2 = weakSelf;
                    if (s2)
                    {
                        s2->_shell->client_->subscribe_room_at(room, event_id);
                    }
                });
        };
        _mainApp->room_view()->on_jump_to_date_requested = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (s)
            {
                [s openJumpToDateDialog];
            }
        };
        _mainApp->room_view()->on_emoji = [weakSelf](tk::Rect btn)
        {
            MainWindowController* s = weakSelf;
            if (s)
            {
                [s showEmojiPickerAtRect:btn];
            }
        };
        _mainApp->room_view()->on_sticker = [weakSelf](tk::Rect btn)
        {
            MainWindowController* s = weakSelf;
            if (s)
            {
                [s _showStickerPickerAtRect:btn];
            }
        };
        _mainApp->room_view()->on_edit_cancelled = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (!s)
            {
                return;
            }
            if (s->_roomTextArea)
            {
                s->_roomTextArea->set_text("");
            }
            if (s->_roomView)
            {
                s->_roomView->set_current_text({});
            }
        };
        _mainApp->room_view()->on_edit_prefill =
            [weakSelf](const std::string& body)
        {
            MainWindowController* s = weakSelf;
            if (!s)
            {
                return;
            }
            if (s->_roomTextArea)
            {
                s->_roomTextArea->set_text(body);
            }
            if (s->_roomView)
            {
                s->_roomView->set_current_text(body);
            }
            if (s->_roomTextArea)
            {
                s->_roomTextArea->set_focused(true);
            }
        };
        _mainApp->room_view()->on_reply_focus = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (s && s->_roomTextArea)
            {
                s->_roomTextArea->set_focused(true);
            }
        };
        _mainApp->room_view()->on_fetch_room_members =
            [weakSelf](std::string room_id)
        {
            MainWindowController* s = weakSelf;
            if (!s || !s->_shell->client_)
                return;
            auto* c = s->_shell->client_;
            s->_shell->run_async_(
                [weakSelf, c, room_id = std::move(room_id)]() mutable
                {
                    auto members = c->get_room_members(room_id);
                    auto members_holder =
                        std::make_shared<std::vector<tesseract::RoomMember>>(
                            std::move(members));
                    dispatch_async(dispatch_get_main_queue(), ^{
                        MainWindowController* s2 = weakSelf;
                        if (!s2 || !s2->_mainApp)
                            return;
                        for (const auto& m : *members_holder)
                            s2->_shell->ensure_user_avatar_(m.avatar_url);
                        s2->_mainApp->room_view()->set_room_members(
                            std::move(*members_holder));
                    });
                });
        };
        _mainApp->room_view()->on_save_topic =
            [weakSelf](std::string room_id, std::string topic)
        {
            MainWindowController* s = weakSelf;
            if (!s || !s->_shell->client_)
                return;
            auto* c = s->_shell->client_;
            s->_shell->run_async_(
                [c, room_id = std::move(room_id),
                 topic = std::move(topic)]() mutable
                {
                    c->set_room_topic(room_id, topic);
                });
        };
        _mainApp->room_view()->on_leave_room =
            [weakSelf](std::string room_id)
        {
            MainWindowController* s = weakSelf;
            if (!s || !s->_shell->client_)
                return;
            auto* c = s->_shell->client_;
            s->_shell->run_async_(
                [weakSelf, c, room_id = std::move(room_id)]() mutable
                {
                    auto result = c->leave_room(room_id);
                    if (result.ok)
                    {
                        dispatch_async(dispatch_get_main_queue(), ^{
                            MainWindowController* s2 = weakSelf;
                            if (!s2)
                                return;
                            s2->_shell->tab_close(room_id);
                            // Fallback: if the room wasn't in a tab (only tab,
                            // or not found), tab_close is a no-op — clear manually.
                            if (s2->_shell->current_room_id_ == room_id)
                            {
                                s2->_shell->current_room_id_.clear();
                                s2->_mainApp->room_view()->clear_room();
                                s2->_mainApp->room_list_view()
                                    ->set_selected_room("");
                                if (s2->_mainAppSurface)
                                    s2->_mainAppSurface->relayout();
                            }
                        });
                    }
                });
        };
        _mainApp->room_view()->on_open_dm =
            [weakSelf](std::string user_id)
        {
            MainWindowController* s = weakSelf;
            if (!s || !s->_shell->client_)
                return;
            auto* c = s->_shell->client_;
            s->_shell->run_async_(
                [weakSelf, c, user_id = std::move(user_id)]() mutable
                {
                    std::string dm_room = c->get_or_create_dm(user_id);
                    if (!dm_room.empty())
                    {
                        dispatch_async(dispatch_get_main_queue(), ^{
                            MainWindowController* s2 = weakSelf;
                            if (!s2)
                                return;
                            s2->_shell->tab_navigate_room(dm_room);
                        });
                    }
                });
        };
        _mainApp->room_view()->on_ignore_user =
            [weakSelf](std::string user_id)
        {
            MainWindowController* s = weakSelf;
            if (!s || !s->_shell->client_)
                return;
            auto* c = s->_shell->client_;
            s->_shell->run_async_(
                [c, user_id = std::move(user_id)]() mutable
                {
                    c->ignore_user(user_id);
                });
        };
        _mainApp->room_view()->set_repaint_requester(
            [weakSelf]
            {
                MainWindowController* s = weakSelf;
                if (s && s->_mainAppSurface)
                {
                    s->_mainAppSurface->relayout();
                }
            });
        _mainApp->room_view()->set_post_delayed(
            [weakSelf](int ms, std::function<void()> fn)
            {
                MainWindowController* s = weakSelf;
                if (s && s->_mainAppSurface)
                {
                    s->_mainAppSurface->host().post_delayed(ms, std::move(fn));
                }
            });
        _mainApp->room_view()->on_layout_changed = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (s)
            {
                [s _relayoutChatSurface];
            }
        };

        _mainAppSurface->set_on_right_click(
            [weakSelf](tk::Point p)
            {
                MainWindowController* s = weakSelf;
                if (!s || !s->_mainApp)
                {
                    return;
                }
                // Dispatch to user info widget if the click lands there.
                if (s->_mainApp->hit_test(p) == s->_mainApp->user_info())
                {
                    if (s->_mainApp->user_info()->on_secondary)
                    {
                        s->_mainApp->user_info()->on_secondary(p);
                    }
                    return;
                }
                // Sticker context menu.
                if (!s->_roomView)
                {
                    return;
                }
                auto hit = s->_roomView->message_list()->sticker_hit_at(p);
                if (!hit)
                {
                    return;
                }
                s->_ctxStickerEventId = hit->event_id;
                s->_ctxStickerMxcUrl = hit->source ? hit->source->mxc_url() : std::string{};
                s->_ctxStickerBody = hit->body;
                s->_ctxStickerInfoJson = hit->info_json;
                NSView* view =
                    (__bridge NSView*)s->_mainAppSurface->view_handle();
                NSPoint local = NSMakePoint(p.x, p.y);
                NSPoint screen = [view.window
                    convertPointToScreen:[view convertPoint:local toView:nil]];
                [s _showStickerContextMenuAt:screen];
            });

        _mainAppSurface->set_root(std::move(main_app_owner));

        // Native overlays.
        _roomTextArea = _mainAppSurface->host().make_text_area();
        _roomTextArea->set_placeholder("Message…");
        // Topic-edit overlay (positioned by set_on_layout below).
        _topicTextArea = _mainAppSurface->host().make_text_area();
        _topicTextArea->set_on_changed(
            [weakSelf](const std::string& t)
            {
                MainWindowController* s = weakSelf;
                if (s && s->_mainApp)
                    s->_mainApp->room_view()->set_topic_edit_text(t);
            });
        _topicTextArea->set_visible(false);
        _roomTextArea->set_on_changed(
            [weakSelf](const std::string& s)
            {
                MainWindowController* c = weakSelf;
                if (!c)
                {
                    return;
                }
                c->_shell->handle_compose_text_changed_(s);
                if (c->_roomView)
                {
                    c->_roomView->set_current_text(s);
                }

                // ── Shortcode detection ─────────────────────────────────────────
                int cursor = (int)s.size();

                auto complete =
                    c->_shell->shortcode_engine_.find_complete(s, cursor);
                if (complete)
                {
                    auto hits = c->_shell->shortcode_engine_.lookup(
                        complete->prefix, c->_shell->cached_emoticons_, 1);
                    std::string r =
                        (!hits.empty() && !hits.front().glyph.empty())
                            ? hits.front().glyph
                            : ":" + complete->prefix + ":";
                    c->_roomTextArea->replace_range(complete->start,
                                                    complete->end, r);
                    [c hideShortcodePopup];
                    return;
                }

                auto prefix_match =
                    c->_shell->shortcode_engine_.find_prefix(s, cursor);
                if (prefix_match && prefix_match->prefix.size() >= 2)
                {
                    c->_shell->shortcode_current_suggestions_ =
                        c->_shell->shortcode_engine_.lookup(
                            prefix_match->prefix, c->_shell->cached_emoticons_);
                    if (!c->_shell->shortcode_current_suggestions_.empty())
                    {
                        c->_shell->shortcode_active_match_ = *prefix_match;
                        for (const auto& sugg :
                             c->_shell->shortcode_current_suggestions_)
                        {
                            if (!sugg.emoticon.url.empty())
                            {
                                c->_shell->ensure_media_image_(
                                    sugg.emoticon.url, 28, 28);
                            }
                        }
                        bool was_visible = [c shortcodePopupVisible];
                        [c showShortcodePopupWithSuggestions:
                                c->_shell->shortcode_current_suggestions_
                                                  cursorRect:
                                                      c->_roomTextArea
                                                          ->cursor_rect()];
                        if (!was_visible)
                        {
                            c->_roomTextArea->set_on_popup_nav(
                                [weakSelf](
                                    tk::NativeTextArea::NavKey nk) -> bool
                                {
                                    MainWindowController* c2 = weakSelf;
                                    if (!c2 || ![c2 shortcodePopupVisible])
                                    {
                                        return false;
                                    }
                                    int cur = c2->_shortcodePopupWidget
                                                  ->selected_index();
                                    int n = c2->_shortcodePopupWidget
                                                ->visible_rows();
                                    if (n <= 0)
                                    {
                                        return true;
                                    }
                                    int next = cur;
                                    switch (nk)
                                    {
                                    case tk::NativeTextArea::NavKey::Up:
                                        next = std::max(0, cur - 1);
                                        break;
                                    case tk::NativeTextArea::NavKey::Down:
                                        next = std::min(n - 1, cur + 1);
                                        break;
                                    case tk::NativeTextArea::NavKey::Tab:
                                    {
                                        int sel = c2->_shortcodePopupWidget
                                                      ->selected_index();
                                        auto& suggs =
                                            c2->_shell
                                                ->shortcode_current_suggestions_;
                                        if (sel >= 0 && sel < (int)suggs.size())
                                        {
                                            auto& s = suggs[sel];
                                            std::string r =
                                                s.glyph.empty()
                                                    ? ":" + s.shortcode + ":"
                                                    : s.glyph;
                                            c2->_roomTextArea->replace_range(
                                                c2->_shell
                                                    ->shortcode_active_match_
                                                    .start,
                                                c2->_shell
                                                    ->shortcode_active_match_
                                                    .end,
                                                r);
                                        }
                                        [c2 hideShortcodePopup];
                                        return true;
                                    }
                                    case tk::NativeTextArea::NavKey::ShiftTab:
                                        return false;
                                    case tk::NativeTextArea::NavKey::Escape:
                                        [c2 hideShortcodePopup];
                                        return true;
                                    }
                                    c2->_shortcodePopupWidget
                                        ->set_selected_index(next);
                                    c2->_shortcodePopupSurface->host()
                                        .request_repaint();
                                    return true;
                                });
                        }
                        return;
                    }
                }
                [c hideShortcodePopup];
                // ── End shortcode detection ─────────────────────────────────────
            });
        _roomTextArea->set_on_submit(
            [weakSelf]
            {
                MainWindowController* c = weakSelf;
                if (!c)
                {
                    return;
                }
                if ([c shortcodePopupVisible])
                {
                    int sel = c->_shortcodePopupWidget->selected_index();
                    if (sel >= 0 &&
                        sel < (int)c->_shell->shortcode_current_suggestions_
                                  .size())
                    {
                        auto& sugg =
                            c->_shell->shortcode_current_suggestions_[sel];
                        std::string r = sugg.glyph.empty()
                                            ? ":" + sugg.shortcode + ":"
                                            : sugg.glyph;
                        c->_roomTextArea->replace_range(
                            c->_shell->shortcode_active_match_.start,
                            c->_shell->shortcode_active_match_.end, r);
                        [c hideShortcodePopup];
                        return;
                    }
                    [c hideShortcodePopup];
                }
                [c _onComposeSend];
            });
        _roomTextArea->set_on_edit_last(
            [weakSelf]() -> bool
            {
                MainWindowController* c = weakSelf;
                return c && c->_roomView && c->_roomView->edit_last_own();
            });
        _roomTextArea->set_on_height_changed(
            [weakSelf](float h)
            {
                MainWindowController* c = weakSelf;
                if (!c || !c->_roomView)
                {
                    return;
                }
                c->_roomView->set_text_area_natural_height(h);
                [c _relayoutChatSurface];
            });
        _roomTextArea->set_on_image_paste(
            [weakSelf](std::vector<std::uint8_t> bytes, std::string mime)
            {
                MainWindowController* c = weakSelf;
                if (c && c->_roomView)
                {
                    c->_roomView->compose_bar()->set_pending_image(
                        std::move(bytes), std::move(mime));
                }
            });

        _mainAppSurface->set_on_file_drop(
            [weakSelf](std::vector<std::uint8_t> bytes, std::string mime,
                       std::string filename)
            {
                MainWindowController* c = weakSelf;
                if (!c || !c->_roomView)
                {
                    return;
                }
                const auto limit = c->_shell->client_->media_upload_limit();
                if (limit > 0 && bytes.size() > limit)
                {
                    return;
                }
                if (bytes.empty()) return;
                auto* cb = c->_roomView->compose_bar();
                if (mime == "image/gif" || mime == "image/webp")
                {
                    // Show first frame immediately; detect animation in background.
                    cb->set_pending_image(bytes, mime, filename,
                                         /*is_animated=*/false);
                    auto gen = cb->pending_gen();
                    c->_shell->extract_media_info_(gen, std::move(bytes),
                                                   std::move(mime));
                }
                else if (mime.rfind("image/", 0) == 0)
                {
                    cb->set_pending_image(std::move(bytes), std::move(mime),
                                         std::move(filename), /*is_animated=*/false);
                }
                else if (mime.rfind("video/", 0) == 0)
                {
                    cb->set_pending_video(bytes, mime, filename);
                    auto gen = cb->pending_gen();
                    c->_shell->extract_media_info_(gen, std::move(bytes),
                                                   std::move(mime));
                }
                else if (mime.rfind("audio/", 0) == 0)
                {
                    cb->set_pending_audio(bytes, mime, filename);
                    auto gen = cb->pending_gen();
                    c->_shell->extract_media_info_(gen, std::move(bytes),
                                                   std::move(mime));
                }
                else
                {
                    cb->set_pending_file(std::move(bytes), std::move(mime),
                                         std::move(filename));
                }
            });

        _roomSearchField = _mainAppSurface->host().make_text_field();
        _roomSearchField->set_placeholder("Search");
        _roomSearchField->set_visible(false);
        _roomSearchField->set_on_changed(
            [weakSelf](const std::string& q)
            {
                MainWindowController* s = weakSelf;
                if (!s)
                {
                    return;
                }
                s->_pendingSearchText = q;
                [NSObject
                    cancelPreviousPerformRequestsWithTarget:s
                                                   selector:
                                                       @selector(
                                                           _applySearchFilter)
                                                     object:nil];
                [s performSelector:@selector(_applySearchFilter)
                        withObject:nil
                        afterDelay:0.5];
            });

        _recoveryKeyField = _mainAppSurface->host().make_text_field();
        _recoveryKeyField->set_placeholder("Recovery key or passphrase");
        _recoveryKeyField->set_password(true);
        _recoveryKeyField->set_on_changed(
            [weakSelf](const std::string& k)
            {
                MainWindowController* s = weakSelf;
                if (s && s->_recoveryShared)
                {
                    s->_recoveryShared->set_current_key(k);
                }
            });
        _recoveryKeyField->set_on_submit(
            [weakSelf]
            {
                MainWindowController* s = weakSelf;
                if (s)
                {
                    [s _onRecoveryVerify];
                }
            });

        _mainAppSurface->set_on_layout(
            [weakSelf]
            {
                MainWindowController* s = weakSelf;
                if (!s || !s->_mainApp)
                {
                    return;
                }
                auto* app = s->_mainApp;
                auto* surf = s->_mainAppSurface.get();
                // Hide all native overlays while a fullscreen viewer is open —
                // the viewer is a canvas widget so it paints above the C++
                // widget tree, but native NSView overlays always sit above the
                // canvas in AppKit's view hierarchy.
                bool viewerOpen = (s->_imgViewer && s->_imgViewer->is_open()) ||
                                  (s->_vidViewer && s->_vidViewer->is_open());
                if (viewerOpen)
                {
                    s->_roomTextArea->set_visible(false);
                    s->_topicTextArea->set_visible(false);
                    s->_roomSearchField->set_visible(false);
                    s->_recoveryKeyField->set_visible(false);
                    (void)surf;
                    return;
                }
                // Compose text area.
                {
                    const tk::Rect ta = app->compose_text_area_rect();
                    s->_roomTextArea->set_visible(!ta.empty());
                    if (!ta.empty())
                        s->_roomTextArea->set_rect(ta);
                }
                // Topic-edit text area.
                {
                    const tk::Rect tr = app->room_view()->topic_edit_rect();
                    const bool wasVisible = s->_topicTextArea->visible();
                    s->_topicTextArea->set_visible(!tr.empty() && !viewerOpen);
                    if (!tr.empty() && !viewerOpen)
                    {
                        s->_topicTextArea->set_rect(tr);
                        if (!wasVisible)
                            s->_topicTextArea->set_text(
                                app->room_view()->topic_edit_initial_text());
                    }
                }
                // Room search field.
                bool searchVisible = app->room_search_field_visible();
                s->_roomSearchField->set_visible(searchVisible);
                if (searchVisible)
                {
                    s->_roomSearchField->set_rect(
                        app->room_search_field_rect());
                }
                // Recovery key field.
                bool recoveryVisible = app->recovery_key_field_visible();
                s->_recoveryKeyField->set_visible(recoveryVisible);
                if (recoveryVisible)
                {
                    s->_recoveryKeyField->set_rect(
                        app->recovery_key_field_rect());
                }
                (void)surf;
            });

        // Escape key monitor: close lightbox overlays when open.
        _escapeMonitor = [NSEvent
            addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                         handler:^(NSEvent* event) {
                                             MainWindowController* s = weakSelf;
                                             if (s && event.keyCode == 53)
                                             { // Escape
                                                 if (s->_vidViewer &&
                                                     s->_vidViewer->is_open())
                                                 {
                                                     s->_vidViewer->close();
                                                     s->_mainApp
                                                         ->show_video_viewer(
                                                             false);
                                                     s->_mainAppSurface
                                                         ->relayout();
                                                     if (!s->_mainApp
                                                              ->compose_text_area_rect()
                                                              .empty())
                                                         s->_roomTextArea
                                                             ->set_focused(
                                                                 true);
                                                     return (NSEvent*)nil;
                                                 }
                                                 if (s->_imgViewer &&
                                                     s->_imgViewer->is_open())
                                                 {
                                                     s->_imgViewer->close();
                                                     s->_mainApp
                                                         ->show_image_viewer(
                                                             false);
                                                     s->_mainAppSurface
                                                         ->relayout();
                                                     if (!s->_mainApp
                                                              ->compose_text_area_rect()
                                                              .empty())
                                                         s->_roomTextArea
                                                             ->set_focused(
                                                                 true);
                                                     return (NSEvent*)nil;
                                                 }
                                             }
                                             return event;
                                         }];
    }

    NSView* mainAppView = (__bridge NSView*)_mainAppSurface->view_handle();
    mainAppView.translatesAutoresizingMaskIntoConstraints = NO;
    mainAppView.hidden = YES;

    // ── Login overlay ─────────────────────────────────────────────────
    _loginView = [[LoginView alloc] init];
    _loginView.delegate = self;
    _loginView.translatesAutoresizingMaskIntoConstraints = NO;
    _loginView.hidden = YES;

    // ── Settings overlay ──────────────────────────────────────────────
    {
        _settingsSurface =
            std::make_unique<tk::macos::Surface>(tk::Theme::light());
        auto view = std::make_unique<tesseract::views::SettingsView>();
        _settingsView = view.get();
        __weak MainWindowController* ws = self;
        _settingsView->on_close = [ws]
        {
            MainWindowController* s = ws;
            if (!s)
            {
                return;
            }
            NSView* mainAppView =
                (__bridge NSView*)s->_mainAppSurface->view_handle();
            mainAppView.hidden = NO;
            ((__bridge NSView*)s->_settingsSurface->view_handle()).hidden = YES;
        };
        _settingsView->on_theme_changed =
            [ws](tesseract::Settings::ThemePreference pref)
        {
            MainWindowController* s = ws;
            if (s)
            {
                s->_shell->set_theme_preference_(pref);
            }
        };
        _settingsView->on_notifications_changed = [ws](bool enabled)
        {
            MainWindowController* s = ws;
            if (!s)
            {
                return;
            }
            tesseract::Settings::instance().notifications_enabled = enabled;
            tesseract::Settings::instance().save_to_disk(
                tesseract::config_dir());
        };
        _settingsView->on_image_previews_changed = [ws](bool enabled)
        {
            MainWindowController* s = ws;
            if (!s)
            {
                return;
            }
            tesseract::Settings::instance().notification_image_previews =
                enabled;
            tesseract::Settings::instance().save_to_disk(
                tesseract::config_dir());
        };
        _settingsView->on_prefetch_changed = [ws](bool enabled)
        {
            MainWindowController* s = ws;
            if (!s)
            {
                return;
            }
            tesseract::Settings::instance().prefetch_full_media = enabled;
            tesseract::Settings::instance().save_to_disk(
                tesseract::config_dir());
        };
        _settingsSurface->set_root(std::move(view));
        _settingsSurface->set_theme(_mainAppSurface->theme());
        _settingsSurface->set_on_layout(
            [ws]
            {
                MainWindowController* s = ws;
                if (!s || !s->_settingsView || !s->_settingsNameField)
                    return;
                const tk::Rect r = s->_settingsView->name_field_rect();
                s->_settingsNameField->set_visible(!r.empty());
                if (!r.empty())
                    s->_settingsNameField->set_rect(r);
            });
    }
    NSView* settingsView = (__bridge NSView*)_settingsSurface->view_handle();
    settingsView.translatesAutoresizingMaskIntoConstraints = NO;
    settingsView.hidden = YES;

    _brandingSurface = std::make_unique<tk::macos::Surface>(tk::Theme::light());
    _brandingSurface->set_root(std::make_unique<tesseract::views::BrandView>());
    NSView* brandingView =
        (__bridge NSView*)_brandingSurface->view_handle();
    brandingView.translatesAutoresizingMaskIntoConstraints = NO;

    [content addSubview:mainAppView];
    [content addSubview:_loginView];
    [content addSubview:settingsView];
    [content addSubview:brandingView];
    [NSLayoutConstraint activateConstraints:@[
        [mainAppView.topAnchor constraintEqualToAnchor:content.topAnchor],
        [mainAppView.leadingAnchor
            constraintEqualToAnchor:content.leadingAnchor],
        [mainAppView.trailingAnchor
            constraintEqualToAnchor:content.trailingAnchor],
        [mainAppView.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
        [_loginView.topAnchor constraintEqualToAnchor:content.topAnchor],
        [_loginView.leadingAnchor
            constraintEqualToAnchor:content.leadingAnchor],
        [_loginView.trailingAnchor
            constraintEqualToAnchor:content.trailingAnchor],
        [_loginView.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
        [settingsView.topAnchor constraintEqualToAnchor:content.topAnchor],
        [settingsView.leadingAnchor
            constraintEqualToAnchor:content.leadingAnchor],
        [settingsView.trailingAnchor
            constraintEqualToAnchor:content.trailingAnchor],
        [settingsView.bottomAnchor
            constraintEqualToAnchor:content.bottomAnchor],
        [brandingView.topAnchor constraintEqualToAnchor:content.topAnchor],
        [brandingView.leadingAnchor
            constraintEqualToAnchor:content.leadingAnchor],
        [brandingView.trailingAnchor
            constraintEqualToAnchor:content.trailingAnchor],
        [brandingView.bottomAnchor
            constraintEqualToAnchor:content.bottomAnchor],
    ]];
}

- (void)dealloc
{
    [NSApp removeObserver:self forKeyPath:@"effectiveAppearance"];
    [self stopSync];
    if (_escapeMonitor)
    {
        [NSEvent removeMonitor:_escapeMonitor];
        _escapeMonitor = nil;
    }
}

- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary*)change
                       context:(void*)context
{
    if ([keyPath isEqualToString:@"effectiveAppearance"] && object == NSApp)
    {
        if (_shell && tesseract::Settings::instance().theme_pref ==
                          tesseract::Settings::ThemePreference::System)
        {
            _shell->apply_current_theme_();
        }
    }
    else
    {
        [super observeValueForKeyPath:keyPath
                             ofObject:object
                               change:change
                              context:context];
    }
}

- (void)_applyTheme:(const tk::Theme&)t
{
    if (_brandingSurface)
    {
        _brandingSurface->set_theme(t);
    }
    if (_mainAppSurface)
    {
        _mainAppSurface->set_theme(t);
    }
    if (_accountPickerSurface)
    {
        _accountPickerSurface->set_theme(t);
    }
    if (_settingsSurface)
    {
        _settingsSurface->set_theme(t);
    }
    if (_shortcodePopupSurface)
    {
        _shortcodePopupSurface->set_theme(t);
    }
    if (_loginView)
    {
        [_loginView setTheme:t];
    }

    // Re-theme the singleton pickers only if they were ever shown
    // (existingPanel returns nil otherwise; messaging nil is a no-op so
    // we don't force-create a panel just to theme it).
    [[EmojiPickerPanel existingPanel] setTheme:t];
    [[StickerPickerPanel existingPanel] setTheme:t];

    NSAppearanceName name = (t.mode == tk::ThemeMode::Dark)
                                ? NSAppearanceNameDarkAqua
                                : NSAppearanceNameAqua;
    NSApp.appearance = [NSAppearance appearanceNamed:name];
}

- (void)stopSync
{
    // Drain background workers BEFORE tearing the client down.  Each
    // worker calls `client_.fetch_*` (which takes `&mut self` on the
    // Rust side); racing one against `~ClientFfi` is a data race that
    // surfaces as `panic_in_cleanup` through cxx's `prevent_unwind`.
    _shell->shutting_down_.store(true, std::memory_order_release);
    {
        std::unique_lock<std::mutex> lk(_shell->workers_mu_);
        _shell->workers_cv_.wait_for(
            lk, std::chrono::seconds(5),
            [self]
            {
                return self->_shell->workers_in_flight_ == 0;
            });
    }
    for (auto& acc : _shell->accounts_)
    {
        if (acc->sync_started)
        {
            acc->client->stop_sync();
        }
    }
}

- (void)showEmojiPicker:(id)sender
{
    if (!_mainAppSurface)
    {
        return;
    }
    EmojiPickerPanel* panel = [EmojiPickerPanel sharedPanel];
    panel.client = _shell->client_;
    __weak MainWindowController* weakSelf = self;
    [panel
        setImageProvider:[weakSelf](const std::string& cache_key,
                                    const std::string& /*source_token*/)
                             -> const tk::Image*
                         {
                             MainWindowController* s = weakSelf;
                             if (!s)
                             {
                                 return nullptr;
                             }
                             if (auto* f = s->_shell->anim_cache_.current_frame(
                                     cache_key))
                             {
                                 return f;
                             }
                             auto it = s->_shell->tk_images_.find(cache_key);
                             if (it != s->_shell->tk_images_.end())
                             {
                                 return it->second.get();
                             }
                             [s _ensureEmojiImageAsync:cache_key];
                             return nullptr;
                         }];
    __weak EmojiPickerPanel* weakPanel = panel;
    panel.onSelect = ^(NSString* glyph) {
        MainWindowController* s = weakSelf;
        if (!s || glyph.length == 0)
        {
            return;
        }
        // Reaction mode — "+" chip set _pendingReactionEventId.
        if (!s->_pendingReactionEventId.empty())
        {
            std::string ev = std::move(s->_pendingReactionEventId);
            s->_pendingReactionEventId.clear();
            if (!s->_shell->current_room_id_.empty())
            {
                s->_shell->client_->send_reaction(
                    s->_shell->current_room_id_, ev,
                    std::string(glyph.UTF8String ?: ""));
            }
            [weakPanel close];
            return;
        }
        if (!s->_roomTextArea)
        {
            return;
        }
        s->_roomTextArea->insert_at_cursor(std::string(glyph.UTF8String ?: ""));
        if (s->_roomView)
        {
            s->_roomView->set_current_text(s->_roomTextArea->text());
        }
        s->_roomTextArea->set_focused(true);
    };
    NSView* anchor = (__bridge NSView*)_mainAppSurface->view_handle();
    [panel popupAboveView:anchor];
}

// ---------------------------------------------------------------------------
// Shortcode suggestion popup
// ---------------------------------------------------------------------------

- (BOOL)shortcodePopupVisible
{
    return _shortcodePanel && _shortcodePanel.isVisible;
}

- (void)_relayoutShortcodePopupIfVisible
{
    if ([self shortcodePopupVisible] && _shortcodePopupSurface)
    {
        _shortcodePopupSurface->relayout();
    }
}

- (void)showShortcodePopupWithSuggestions:
            (const std::vector<tesseract::views::ShortcodeSuggestion>&)
                suggestions
                               cursorRect:(tk::Rect)cursor
{
    int rows = std::min((int)suggestions.size(),
                        int(tesseract::views::ShortcodePopup::kMaxRows));
    NSSize size =
        NSMakeSize(tesseract::views::ShortcodePopup::kWidth,
                   rows * tesseract::views::ShortcodePopup::kRowHeight);

    if (!_shortcodePanel)
    {
        NSRect frame = NSMakeRect(0, 0, size.width, size.height);
        _shortcodePanel = [[NSPanel alloc]
            initWithContentRect:frame
                      styleMask:NSWindowStyleMaskNonactivatingPanel |
                                NSWindowStyleMaskBorderless
                        backing:NSBackingStoreBuffered
                          defer:NO];
        _shortcodePanel.floatingPanel = YES;
        _shortcodePanel.hidesOnDeactivate = NO;
        _shortcodePanel.becomesKeyOnlyIfNeeded = YES;

        _shortcodePopupSurface =
            std::make_unique<tk::macos::Surface>(_mainAppSurface->theme());
        auto pw = std::make_unique<tesseract::views::ShortcodePopup>();
        _shortcodePopupWidget = pw.get();
        _shortcodePopupSurface->set_root(std::move(pw));

        __weak MainWindowController* weakSelf = self;
        _shortcodePopupWidget->on_accepted =
            [weakSelf](tesseract::views::ShortcodeSuggestion s)
        {
            MainWindowController* c = weakSelf;
            if (!c || !c->_roomTextArea)
            {
                return;
            }
            std::string r = s.glyph.empty() ? ":" + s.shortcode + ":" : s.glyph;
            c->_roomTextArea->replace_range(
                c->_shell->shortcode_active_match_.start,
                c->_shell->shortcode_active_match_.end, std::move(r));
            [c hideShortcodePopup];
        };
        _shortcodePopupWidget->on_dismissed = [weakSelf]
        {
            if (MainWindowController* c = weakSelf)
            {
                [c hideShortcodePopup];
            }
        };
        __weak MainWindowController* weakSelf2 = self;
        _shortcodePopupWidget->set_image_provider(
            [weakSelf2](const std::string& url) -> const tk::Image*
            {
                MainWindowController* c = weakSelf2;
                if (!c || !c->_shell)
                {
                    return nullptr;
                }
                auto it = c->_shell->tk_images_.find(url);
                return it == c->_shell->tk_images_.end() ? nullptr
                                                         : it->second.get();
            });

        NSView* popupView =
            (__bridge NSView*)_shortcodePopupSurface->view_handle();
        [_shortcodePanel setContentView:popupView];
    }

    _shortcodePopupWidget->set_suggestions(suggestions);
    [_shortcodePanel setContentSize:size];
    _shortcodePopupSurface->relayout();

    // Map cursor_local (TKSurfaceView y-down) → screen coords (y-up).
    // cursor.y is the cursor's top edge in TKSurfaceView local coordinates.
    NSView* hostView = (__bridge NSView*)_mainAppSurface->view_handle();
    NSPoint localPt = NSMakePoint(cursor.x, cursor.y);
    NSPoint windowPt = [hostView convertPoint:localPt toView:nil];
    NSPoint screenPt = [hostView.window convertPointToScreen:windowPt];
    // After the conversion screenPt.y is the cursor's top edge in screen y-up.

    NSRect screenFrame = _shortcodePanel.screen
                             ? _shortcodePanel.screen.visibleFrame
                             : [NSScreen mainScreen].visibleFrame;

    // Prefer above the cursor; fall back to below when the panel would be
    // clipped at the top of the screen (mirrors Qt6 / Win32 behaviour).
    CGFloat panelH = size.height;
    CGFloat y_above = screenPt.y + 4;
    CGFloat y_below = screenPt.y - (CGFloat)cursor.h - 4 - panelH;
    CGFloat x = screenPt.x;
    CGFloat y =
        (y_above + panelH <= screenFrame.origin.y + screenFrame.size.height)
            ? y_above
            : y_below;
    x = std::clamp(x, screenFrame.origin.x,
                   screenFrame.origin.x + screenFrame.size.width - size.width);
    y = std::clamp(y, screenFrame.origin.y,
                   screenFrame.origin.y + screenFrame.size.height -
                       size.height);

    [_shortcodePanel setFrameOrigin:NSMakePoint(x, y)];
    [_shortcodePanel orderFront:nil];
}

- (void)hideShortcodePopup
{
    [_shortcodePanel orderOut:nil];
    if (_roomTextArea)
    {
        _roomTextArea->set_on_popup_nav(nullptr);
    }
}

// ---------------------------------------------------------------------------

- (void)showEmojiPickerAtRect:(tk::Rect)anchor
{
    if (!_mainAppSurface)
    {
        return;
    }
    EmojiPickerPanel* panel = [EmojiPickerPanel sharedPanel];
    panel.client = _shell->client_;
    __weak MainWindowController* weakSelf = self;
    [panel
        setImageProvider:[weakSelf](const std::string& cache_key,
                                    const std::string& /*source_token*/)
                             -> const tk::Image*
                         {
                             MainWindowController* s = weakSelf;
                             if (!s)
                             {
                                 return nullptr;
                             }
                             if (auto* f = s->_shell->anim_cache_.current_frame(
                                     cache_key))
                             {
                                 return f;
                             }
                             auto it = s->_shell->tk_images_.find(cache_key);
                             if (it != s->_shell->tk_images_.end())
                             {
                                 return it->second.get();
                             }
                             [s _ensureEmojiImageAsync:cache_key];
                             return nullptr;
                         }];
    __weak EmojiPickerPanel* weakPanel = panel;
    panel.onSelect = ^(NSString* glyph) {
        MainWindowController* s = weakSelf;
        if (!s || glyph.length == 0)
        {
            return;
        }
        if (!s->_pendingReactionEventId.empty())
        {
            std::string ev = std::move(s->_pendingReactionEventId);
            s->_pendingReactionEventId.clear();
            if (!s->_shell->current_room_id_.empty())
            {
                s->_shell->client_->send_reaction(
                    s->_shell->current_room_id_, ev,
                    std::string(glyph.UTF8String ?: ""));
            }
            [weakPanel close];
            return;
        }
        if (!s->_roomTextArea)
        {
            return;
        }
        s->_roomTextArea->insert_at_cursor(std::string(glyph.UTF8String ?: ""));
        if (s->_roomView)
        {
            s->_roomView->set_current_text(s->_roomTextArea->text());
        }
        s->_roomTextArea->set_focused(true);
    };
    NSView* anchorView = (__bridge NSView*)_mainAppSurface->view_handle();
    [panel popupAtRect:anchor inView:anchorView];
}

- (void)_onComposeSend
{
    if (_roomView)
    {
        _roomView->compose_bar()->trigger_send();
    }
}

- (void)_sendComposedImage:(std::vector<std::uint8_t>)bytes
                      mime:(std::string)mime
                  filename:(std::string)filename
                   caption:(std::string)caption
                     width:(std::uint32_t)width
                    height:(std::uint32_t)height
                isAnimated:(bool)is_animated
              replyEventId:(std::string)reply_event_id
{
    if (_shell->current_room_id_.empty() || !_mainAppSurface)
    {
        return;
    }
    tesseract::Result res;
    if (is_animated)
    {
        // Animated GIF/WebP: send the original bytes verbatim via the
        // MSC4230 raw path. Re-encoding would flatten the animation to a
        // single frame.
        res = _shell->client_->send_image(
            _shell->current_room_id_, bytes, mime, filename, caption, width,
            height, /*is_animated=*/true, reply_event_id);
    }
    else
    {
        const bool compress = tesseract::Settings::instance().image_quality ==
                              tesseract::Settings::ImageQuality::Compressed;
        auto enc = _mainAppSurface->host().encode_for_send(
            bytes.data(), bytes.size(), compress);
        if (enc.bytes.empty())
        {
            return;
        }
        std::string out_name = filename;
        if (enc.mime == "image/jpeg")
        {
            auto dot = out_name.find_last_of('.');
            if (dot != std::string::npos)
            {
                out_name = out_name.substr(0, dot);
            }
            out_name += ".jpg";
        }
        res = _shell->client_->send_image(
            _shell->current_room_id_, enc.bytes, enc.mime, out_name, caption,
            enc.width, enc.height, /*is_animated=*/false, reply_event_id);
    }
    if (res)
    {
        if (_roomTextArea)
        {
            _roomTextArea->set_text("");
        }
        if (_roomView)
        {
            _roomView->set_current_text({});
        }
    }
}

- (void)_sendComposedVideo:(std::vector<std::uint8_t>)bytes
                      mime:(std::string)mime
                  filename:(std::string)filename
                   caption:(std::string)caption
                     width:(std::uint32_t)width
                    height:(std::uint32_t)height
                thumbBytes:(std::vector<std::uint8_t>)thumb_bytes
                thumbWidth:(std::uint32_t)thumb_width
               thumbHeight:(std::uint32_t)thumb_height
                durationMs:(std::uint64_t)duration_ms
              replyEventId:(std::string)reply_event_id
{
    if (_shell->current_room_id_.empty())
    {
        return;
    }
    auto res = _shell->client_->send_video(
        _shell->current_room_id_, bytes, mime, filename, caption, width, height,
        thumb_bytes, thumb_width, thumb_height, duration_ms, reply_event_id);
    if (res)
    {
        if (_roomTextArea)
        {
            _roomTextArea->set_text("");
        }
        if (_roomView)
        {
            _roomView->set_current_text({});
        }
    }
}

- (void)_sendComposedAudio:(std::vector<std::uint8_t>)bytes
                      mime:(std::string)mime
                  filename:(std::string)filename
                   caption:(std::string)caption
                durationMs:(std::uint64_t)duration_ms
              replyEventId:(std::string)reply_event_id
{
    if (_shell->current_room_id_.empty())
    {
        return;
    }
    auto res = _shell->client_->send_audio(_shell->current_room_id_, bytes,
                                           mime, filename, caption, duration_ms,
                                           reply_event_id);
    if (res)
    {
        if (_roomTextArea)
        {
            _roomTextArea->set_text("");
        }
        if (_roomView)
        {
            _roomView->set_current_text({});
        }
    }
}

- (void)_sendComposedFile:(std::vector<std::uint8_t>)bytes
                     mime:(std::string)mime
                 filename:(std::string)filename
                  caption:(std::string)caption
             replyEventId:(std::string)reply_event_id
{
    if (_shell->current_room_id_.empty())
    {
        return;
    }
    auto res = _shell->client_->send_file(_shell->current_room_id_, bytes, mime,
                                          filename, caption, reply_event_id);
    if (res)
    {
        if (_roomTextArea)
        {
            _roomTextArea->set_text("");
        }
        if (_roomView)
        {
            _roomView->set_current_text({});
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Login flow
// ─────────────────────────────────────────────────────────────────────────

- (void)beginLogin
{
    if (_brandingSurface)
    {
        ((__bridge NSView*)_brandingSurface->view_handle()).hidden = YES;
    }

    tesseract::SessionStore::migrate_legacy_layout();

    auto index = tesseract::SessionStore::load_index();
    for (const auto& uid : index.user_ids)
    {
        auto session_json = tesseract::SessionStore::load_account(uid);
        if (!session_json)
        {
            continue;
        }

        auto session = std::make_unique<tesseract::AccountSession>();
        session->client = std::make_unique<tesseract::Client>();
        session->client->set_data_dir(
            tesseract::SessionStore::sdk_store_dir(uid).string());
        if (!session->client->restore_session(*session_json))
        {
            continue;
        }

        auto* bridge_ptr = new tesseract::EventHandlerBase(_shell.get());
        bridge_ptr->set_user_id(uid);
        session->bridge.reset(bridge_ptr);

        session->user_id = session->client->get_user_id();
        session->display_name = session->client->get_display_name();
        session->avatar_url = session->client->get_avatar_url();
        session->last_room =
            tesseract::Prefs::parse(session->client->load_prefs_json())
                .last_room;
        session->sync_started = true;
        session->client->start_sync(session->bridge.get());

        _shell->accounts_.push_back(std::move(session));
    }

    if (_shell->accounts_.empty())
    {
        _shell->pending_login_temp_dir_ = {};
        _shell->pending_login_client_ = std::make_unique<tesseract::Client>();
        [_loginView setClient:_shell->pending_login_client_.get()];
        __weak MainWindowController* weakSelf = self;
        _loginView.onBeginOAuth = ^{
            MainWindowController* s = weakSelf;
            if (!s || !s->_shell->pending_login_temp_dir_.empty())
            {
                return;
            }
            auto ms = std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            s->_shell->pending_login_temp_dir_ =
                tesseract::SessionStore::account_dir("pending-" + ms);
            s->_shell->pending_login_client_->set_data_dir(
                (s->_shell->pending_login_temp_dir_ / "matrix-store").string());
        };
        [_loginView setMode:tesseract::views::LoginView::Mode::Initial];
        [_loginView reset];
        // _mainAppSurface is already hidden from _buildChrome; login overlay is shown.
        _loginView.hidden = NO;
        return;
    }

    int firstActive = 0;
    for (int i = 0; i < (int)_shell->accounts_.size(); ++i)
    {
        if (_shell->accounts_[i]->user_id == index.active_user_id)
        {
            firstActive = i;
            break;
        }
    }
    [self _switchActiveAccount:firstActive];
    [self _buildSettingsController];
}

- (void)_buildSettingsController
{
    __weak MainWindowController* ws = self;
    _shell->settings_controller_ =
        std::make_unique<tesseract::SettingsController>(
            _shell->client_,
            [ws](auto fn) {
                MainWindowController* s = ws;
                if (s)
                    s->_shell->post_to_ui_(std::move(fn));
            },
            [ws](auto cb) {
                MainWindowController* s = ws;
                if (!s)
                    return;
                NSOpenPanel* panel = [NSOpenPanel openPanel];
                panel.allowedContentTypes = @[ UTTypeImage ];
                panel.canChooseFiles = YES;
                panel.allowsMultipleSelection = NO;
                __weak MainWindowController* ws2 = s;
                [panel beginWithCompletionHandler:^(NSModalResponse result) {
                    if (result != NSModalResponseOK)
                        return;
                    NSURL* url = panel.URLs.firstObject;
                    if (!url)
                        return;
                    NSData* data = [NSData dataWithContentsOfURL:url];
                    if (!data || data.length == 0)
                        return;
                    std::vector<uint8_t> bytes(
                        static_cast<const uint8_t*>(data.bytes),
                        static_cast<const uint8_t*>(data.bytes) + data.length);
                    std::string mime = "image/jpeg";
                    NSString* ext = url.pathExtension.lowercaseString;
                    if ([ext isEqualToString:@"png"])
                        mime = "image/png";
                    else if ([ext isEqualToString:@"gif"])
                        mime = "image/gif";
                    else if ([ext isEqualToString:@"webp"])
                        mime = "image/webp";
                    dispatch_async(dispatch_get_main_queue(), ^{
                        MainWindowController* s2 = ws2;
                        if (!s2)
                            return;
                        auto callback = std::move(cb);
                        callback(std::move(bytes), mime);
                    });
                }];
            });

    _settingsNameField = _settingsSurface->host().make_text_field();
    _settingsNameField->set_text(_shell->my_display_name_);
    _settingsNameField->set_placeholder("Display name");
    _settingsNameField->set_visible(false);

    _settingsNameField->set_on_submit(
        [ws]
        {
            MainWindowController* s = ws;
            if (!s || !s->_shell->settings_controller_)
                return;
            s->_shell->settings_controller_->set_display_name(
                s->_settingsNameField->text());
            s->_settingsView->set_name_busy(true);
            s->_settingsSurface->relayout();
        });

    _shell->settings_controller_->on_name_changed =
        [ws](std::string name)
        {
            MainWindowController* s = ws;
            if (!s) return;
            s->_settingsView->set_display_name_text(name);
            if (s->_settingsNameField)
                s->_settingsNameField->set_text(name);
            s->_settingsSurface->relayout();
        };
    _shell->settings_controller_->on_name_result =
        [ws](bool ok, std::string error)
        {
            MainWindowController* s = ws;
            if (!s) return;
            s->_settingsView->set_name_busy(false);
            if (!ok) s->_settingsView->set_name_error(std::move(error));
            s->_settingsSurface->relayout();
        };

    if (_settingsView)
    {
        _settingsView->set_controller(
            _shell->settings_controller_.get());
        _settingsView->on_avatar_upload_requested = [ws]
        {
            MainWindowController* s = ws;
            if (s && s->_shell->settings_controller_)
                s->_shell->settings_controller_->upload_avatar();
        };
        _settingsView->on_avatar_remove_requested = [ws]
        {
            MainWindowController* s = ws;
            if (s && s->_shell->settings_controller_)
                s->_shell->settings_controller_->remove_avatar();
        };
    }
}

- (void)loginViewDidSucceed:(LoginView*)view
{
    if (!_shell->pending_login_client_)
    {
        return;
    }

    std::string newUserId = _shell->pending_login_client_->get_user_id();

    // Reject if this account is already signed in.
    for (const auto& a : _shell->accounts_)
    {
        if (a->user_id == newUserId)
        {
            _shell->pending_login_client_.reset();
            std::error_code ec;
            std::filesystem::remove_all(_shell->pending_login_temp_dir_, ec);
            _shell->pending_login_temp_dir_ = {};
            _shell->pending_login_is_add_account_ = false;
            int returnIdx = _shell->add_account_return_idx_;
            _shell->add_account_return_idx_ = -1;
            if (returnIdx >= 0 && returnIdx < (int)_shell->accounts_.size())
            {
                [self _switchActiveAccount:returnIdx];
            }
            return;
        }
    }

    std::string sessionJson = _shell->pending_login_client_->export_session();

    auto finalDir = tesseract::SessionStore::account_dir(newUserId);
    std::error_code ec;
    std::filesystem::create_directories(finalDir.parent_path(), ec);
    std::filesystem::rename(_shell->pending_login_temp_dir_, finalDir, ec);
    if (ec)
    {
        finalDir =
            _shell->pending_login_temp_dir_; // EXDEV fallback: keep as-is
    }
    _shell->pending_login_client_.reset(); // close SQLite handles before reopen
    _shell->pending_login_temp_dir_ = {};

    auto session = std::make_unique<tesseract::AccountSession>();
    session->client = std::make_unique<tesseract::Client>();
    session->client->set_data_dir(
        tesseract::SessionStore::sdk_store_dir(newUserId).string());
    if (!session->client->restore_session(sessionJson))
    {
        return;
    }

    auto* bridge_ptr = new tesseract::EventHandlerBase(_shell.get());
    bridge_ptr->set_user_id(newUserId);
    session->bridge.reset(bridge_ptr);

    session->user_id = newUserId;
    session->display_name = session->client->get_display_name();
    session->avatar_url = session->client->get_avatar_url();
    session->last_room =
        tesseract::Prefs::parse(session->client->load_prefs_json()).last_room;
    session->sync_started = true;
    session->client->start_sync(session->bridge.get());

    tesseract::SessionStore::save_account(newUserId, sessionJson);
    auto idxData = tesseract::SessionStore::load_index();
    auto& ids = idxData.user_ids;
    if (std::find(ids.begin(), ids.end(), newUserId) == ids.end())
    {
        ids.push_back(newUserId);
    }
    idxData.active_user_id = newUserId;
    tesseract::SessionStore::save_index(idxData);

    int newIdx = (int)_shell->accounts_.size();
    _shell->accounts_.push_back(std::move(session));
    _shell->pending_login_is_add_account_ = false;
    _shell->add_account_return_idx_ = -1;

    [self _switchActiveAccount:newIdx];
    [self _buildSettingsController];
}

- (void)loginViewDidCancel:(LoginView*)view
{
    _shell->pending_login_client_.reset();
    if (_shell->pending_login_temp_dir_ != std::filesystem::path())
    {
        std::error_code ec;
        std::filesystem::remove_all(_shell->pending_login_temp_dir_, ec);
        _shell->pending_login_temp_dir_ = {};
    }
    _shell->pending_login_is_add_account_ = false;
    int returnIdx = _shell->add_account_return_idx_;
    _shell->add_account_return_idx_ = -1;
    if (returnIdx >= 0 && returnIdx < (int)_shell->accounts_.size())
    {
        [self _switchActiveAccount:returnIdx];
    }
}

- (void)_switchActiveAccount:(int)idx
{
    _shell->reset_server_info_();
    _shell->active_account_index_ = idx;
    auto* session = _shell->accounts_[idx].get();
    _shell->client_ = session->client.get();
    _shell->event_handler_ = session->bridge.get();

    if (_shell->settings_controller_)
        _shell->settings_controller_->set_client(_shell->client_);

    _shell->my_user_id_ = session->user_id;
    _shell->my_display_name_ = session->display_name;
    _shell->my_avatar_url_ = session->avatar_url;
    _shell->pending_restore_room_ = session->last_room;

    auto idxData = tesseract::SessionStore::load_index();
    idxData.active_user_id = _shell->my_user_id_;
    tesseract::SessionStore::save_index(idxData);

    _shell->current_room_id_.clear();
    _shell->space_stack_.clear();
    _shell->message_cache_.clear();
    _shell->message_cache_lru_.clear();

    auto it = _shell->per_account_rooms_.find(_shell->my_user_id_);
    _shell->rooms_ = (it != _shell->per_account_rooms_.end())
                         ? it->second
                         : std::vector<tesseract::RoomInfo>{};
    [self _refreshRoomList];
    if (_roomView)
    {
        _roomView->clear_room();
        _roomView->set_messages({});
    }
    [self _relayoutChatSurface];

    NSView* mainAppView = (__bridge NSView*)_mainAppSurface->view_handle();
    mainAppView.hidden = NO;
    _loginView.hidden = YES;

    [self _populateUserStrip];
    [self _maybeShowRecoveryBanner];

    if (!_shell->pending_restore_room_.empty())
    {
        for (const auto& r : _shell->rooms_)
        {
            if (r.id == _shell->pending_restore_room_ && !r.is_space)
            {
                std::string target = std::move(_shell->pending_restore_room_);
                _shell->pending_restore_room_.clear();
                [self onRoomSelected:target];
                break;
            }
        }
    }

    if (!_tray)
    {
        __weak MainWindowController* weakSelf = self;
        _tray = std::make_unique<MacOSTrayIcon>(
            [weakSelf]
            {
                dispatch_async(dispatch_get_main_queue(), ^{
                    MainWindowController* strong = weakSelf;
                    if (!strong)
                    {
                        return;
                    }
                    [strong.window makeKeyAndOrderFront:nil];
                    [NSApp activateIgnoringOtherApps:YES];
                });
            },
            []
            {
                dispatch_async(dispatch_get_main_queue(), ^{
                    [NSApp terminate:nil];
                });
            });
    }

    UNUserNotificationCenter* center =
        [UNUserNotificationCenter currentNotificationCenter];
    center.delegate = self;
    UNAuthorizationOptions opts = UNAuthorizationOptionAlert |
                                  UNAuthorizationOptionSound |
                                  UNAuthorizationOptionBadge;
    [center requestAuthorizationWithOptions:opts
                          completionHandler:^(BOOL granted, NSError* err) {
                              (void)granted;
                              (void)err;
                          }];
}

- (void)_beginAddAccount
{
    _shell->pending_login_is_add_account_ = true;
    _shell->add_account_return_idx_ = _shell->active_account_index_;
    _shell->pending_login_temp_dir_ = {};
    _shell->pending_login_client_ = std::make_unique<tesseract::Client>();
    [_loginView setClient:_shell->pending_login_client_.get()];
    __weak MainWindowController* weakSelf = self;
    _loginView.onBeginOAuth = ^{
        MainWindowController* s = weakSelf;
        if (!s || !s->_shell->pending_login_temp_dir_.empty())
        {
            return;
        }
        auto ms = std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        s->_shell->pending_login_temp_dir_ =
            tesseract::SessionStore::account_dir("pending-" + ms);
        s->_shell->pending_login_client_->set_data_dir(
            (s->_shell->pending_login_temp_dir_ / "matrix-store").string());
    };
    [_loginView setMode:tesseract::views::LoginView::Mode::AddAccount];
    [_loginView reset];
    ((__bridge NSView*)_mainAppSurface->view_handle()).hidden = YES;
    _loginView.hidden = NO;
}

- (void)_openSettings
{
    if (!_settingsView || !_settingsSurface)
    {
        return;
    }
    __weak MainWindowController* ws = self;
    _settingsView->set_account_info(
        _shell->my_display_name_, _shell->my_user_id_, _shell->my_avatar_url_);
    _settingsView->set_image_provider(
        [ws](const std::string& mxc) -> const tk::Image*
        {
            MainWindowController* s = ws;
            if (!s)
            {
                return nullptr;
            }
            auto it = s->_shell->tk_avatars_.find(mxc);
            return it == s->_shell->tk_avatars_.end() ? nullptr
                                                      : it->second.get();
        });
    _settingsView->set_theme_pref(tesseract::Settings::instance().theme_pref);
    _settingsView->set_notifications_enabled(
        tesseract::Settings::instance().notifications_enabled);
    _settingsView->set_image_previews_enabled(
        tesseract::Settings::instance().notification_image_previews);
    _settingsView->set_prefetch_enabled(
        tesseract::Settings::instance().prefetch_full_media);
    _settingsSurface->relayout();

    NSView* mainAppView = (__bridge NSView*)_mainAppSurface->view_handle();
    mainAppView.hidden = YES;
    ((__bridge NSView*)_settingsSurface->view_handle()).hidden = NO;
}

- (void)_populateUserStrip
{
    if (!_mainApp)
    {
        return;
    }
    _mainApp->user_info()->set_display_name(_shell->my_display_name_);
    _mainApp->user_info()->set_user_id(_shell->my_user_id_);
    _mainApp->user_info()->set_avatar_url(_shell->my_avatar_url_);
    if (!_shell->my_avatar_url_.empty())
    {
        _shell->ensure_user_avatar_(_shell->my_avatar_url_);
    }
    if (_mainAppSurface)
    {
        _mainAppSurface->relayout();
    }
}

- (void)_applySearchFilter
{
    if (_roomListView)
    {
        _roomListView->set_search_text(_pendingSearchText);
    }
}

- (void)_onRoomScrollDebounce
{
    if (!_roomListView || !_shell->client_)
    {
        return;
    }
    auto ids = _roomListView->visible_room_ids();
    _shell->client_->stop_background_backfill();
    _shell->client_->start_background_backfill(ids);
}

- (void)_onSpaceBack
{
    if (!_shell->space_stack_.empty())
    {
        _shell->space_stack_.pop_back();
    }
    [self _refreshRoomList];
}

- (void)_openAccountPicker
{
    if (!_accountPickerPopover)
    {
        _accountPickerPopover = [[NSPopover alloc] init];
        _accountPickerPopover.behavior = NSPopoverBehaviorTransient;
        NSViewController* vc = [[NSViewController alloc] init];
        NSView* container =
            [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 220, 48)];
        vc.view = container;
        _accountPickerPopover.contentViewController = vc;

        _accountPickerSurface =
            std::make_unique<tk::macos::Surface>(tk::Theme::light());
        auto picker = std::make_unique<tesseract::views::AccountPicker>();
        _accountPickerShared = picker.get();
        __weak MainWindowController* weakSelf = self;
        _accountPickerShared->on_select = [weakSelf](const std::string& uid)
        {
            MainWindowController* s = weakSelf;
            if (!s)
            {
                return;
            }
            for (int i = 0; i < (int)s->_shell->accounts_.size(); ++i)
            {
                if (s->_shell->accounts_[i]->user_id == uid)
                {
                    [s->_accountPickerPopover close];
                    [s _switchActiveAccount:i];
                    break;
                }
            }
        };
        _accountPickerShared->set_image_provider(
            [weakSelf](const std::string& mxc) -> const tk::Image*
            {
                MainWindowController* s = weakSelf;
                if (!s)
                {
                    return nullptr;
                }
                auto it = s->_shell->tk_avatars_.find(mxc);
                return it == s->_shell->tk_avatars_.end() ? nullptr
                                                          : it->second.get();
            });
        _accountPickerSurface->set_root(std::move(picker));

        NSView* surfaceView =
            (__bridge NSView*)_accountPickerSurface->view_handle();
        surfaceView.translatesAutoresizingMaskIntoConstraints = NO;
        [container addSubview:surfaceView];
        [NSLayoutConstraint activateConstraints:@[
            [surfaceView.topAnchor constraintEqualToAnchor:container.topAnchor],
            [surfaceView.leadingAnchor
                constraintEqualToAnchor:container.leadingAnchor],
            [surfaceView.trailingAnchor
                constraintEqualToAnchor:container.trailingAnchor],
            [surfaceView.bottomAnchor
                constraintEqualToAnchor:container.bottomAnchor],
        ]];
    }

    std::vector<tesseract::views::AccountEntry> entries;
    for (int i = 0; i < (int)_shell->accounts_.size(); ++i)
    {
        auto& acc = *_shell->accounts_[i];
        tesseract::views::AccountEntry e;
        e.user_id = acc.user_id;
        e.display_name = acc.display_name;
        e.avatar_url = acc.avatar_url;
        e.active = (i == _shell->active_account_index_);
        entries.push_back(std::move(e));
        if (!acc.avatar_url.empty())
        {
            _shell->ensure_user_avatar_(acc.avatar_url);
        }
    }
    _accountPickerShared->set_entries(std::move(entries));

    CGFloat rowH = 48.0f;
    NSSize sz = NSMakeSize(220, rowH * (CGFloat)_shell->accounts_.size());
    _accountPickerPopover.contentSize = sz;
    _accountPickerSurface->relayout();

    // Anchor the popover above the bottom-left of the main app surface (user strip area).
    NSView* mainAppView = (__bridge NSView*)_mainAppSurface->view_handle();
    CGFloat stripH = static_cast<CGFloat>(tesseract::visual::kUserStripHeight);
    NSRect stripRect = NSMakeRect(
        0, 0, static_cast<CGFloat>(tesseract::visual::kSidebarWidth), stripH);
    [_accountPickerPopover showRelativeToRect:stripRect
                                       ofView:mainAppView
                                preferredEdge:NSRectEdgeMaxY];
}

- (void)_logoutActiveAccount
{
    if (_shell->active_account_index_ < 0 ||
        _shell->active_account_index_ >= (int)_shell->accounts_.size())
    {
        return;
    }
    auto* session = _shell->accounts_[_shell->active_account_index_].get();
    std::string uid = session->user_id;

    if (!_shell->current_room_id_.empty())
    {
        if (_shell->room_subscription_refs_.count(_shell->current_room_id_) ==
            0)
        {
            _shell->client_->unsubscribe_room(_shell->current_room_id_);
        }
        _shell->current_room_id_.clear();
    }
    session->client->logout();
    session->client->stop_sync();
    session->sync_started = false;

    tesseract::SessionStore::clear_account(uid);
    _shell->per_account_rooms_.erase(uid);
    _shell->accounts_.erase(_shell->accounts_.begin() +
                            _shell->active_account_index_);

    auto idxData = tesseract::SessionStore::load_index();
    auto& ids = idxData.user_ids;
    ids.erase(std::remove(ids.begin(), ids.end(), uid), ids.end());
    _shell->reset_server_info_();

    if (_shell->accounts_.empty())
    {
        _shell->active_account_index_ = -1;
        _shell->client_ = nullptr;
        _shell->event_handler_ = nullptr;
        idxData.active_user_id.clear();
        tesseract::SessionStore::save_index(idxData);

        _shell->rooms_.clear();
        _shell->space_stack_.clear();
        _shell->message_cache_.clear();
        _shell->message_cache_lru_.clear();
        [self _refreshRoomList];
        _shell->handle_compose_room_leaving_(_shell->current_room_id_);
        _shell->current_room_id_.clear();
        if (_roomView)
        {
            _roomView->clear_room();
            _roomView->set_messages({});
        }
        [self _relayoutChatSurface];

        _shell->pending_login_temp_dir_ = {};
        _shell->pending_login_client_ = std::make_unique<tesseract::Client>();
        [_loginView setClient:_shell->pending_login_client_.get()];
        __weak MainWindowController* weakSelf = self;
        _loginView.onBeginOAuth = ^{
            MainWindowController* s = weakSelf;
            if (!s || !s->_shell->pending_login_temp_dir_.empty())
            {
                return;
            }
            auto ms = std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            s->_shell->pending_login_temp_dir_ =
                tesseract::SessionStore::account_dir("pending-" + ms);
            s->_shell->pending_login_client_->set_data_dir(
                (s->_shell->pending_login_temp_dir_ / "matrix-store").string());
        };
        [_loginView setMode:tesseract::views::LoginView::Mode::Initial];
        [_loginView reset];
        ((__bridge NSView*)_mainAppSurface->view_handle()).hidden = YES;
        _loginView.hidden = NO;
    }
    else
    {
        int newIdx = std::min(_shell->active_account_index_,
                              (int)_shell->accounts_.size() - 1);
        idxData.active_user_id = _shell->accounts_[newIdx]->user_id;
        tesseract::SessionStore::save_index(idxData);
        [self _switchActiveAccount:newIdx];
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Notifications (UNUserNotificationCenter)
// ─────────────────────────────────────────────────────────────────────────

- (void)_requestUserAttention
{
    if (_attentionRequestToken != 0)
    {
        return;
    }
    _attentionRequestToken =
        [NSApp requestUserAttention:NSInformationalRequest];
}

- (void)windowDidBecomeKey:(NSNotification*)notification
{
    (void)notification;
    if (_attentionRequestToken != 0)
    {
        [NSApp cancelUserAttentionRequest:_attentionRequestToken];
        _attentionRequestToken = 0;
    }
}

- (void)handleNotification:(std::string)roomId
                  roomName:(std::string)roomName
                    sender:(std::string)sender
                      body:(std::string)body
                    userId:(std::string)userId
                 isMention:(BOOL)isMention
               avatarBytes:(const std::vector<std::uint8_t>&)avatarBytes
                imageBytes:(const std::vector<std::uint8_t>&)imageBytes
{
    (void)isMention;
    BOOL winVisible = self.window.isVisible && !self.window.isMiniaturized;
    BOOL winFocused = self.window.isKeyWindow;

    // Already watching this exact room — suppress silently.
    if (winFocused && _shell->active_account_index_ >= 0 &&
        (int)_shell->accounts_.size() > _shell->active_account_index_ &&
        _shell->accounts_[_shell->active_account_index_]->user_id == userId &&
        _shell->current_room_id_ == roomId)
    {
        return;
    }

    // Window on screen: no popup. Bounce dock if not focused.
    if (winVisible)
    {
        if (!winFocused)
        {
            [self _requestUserAttention];
        }
        return;
    }

    UNMutableNotificationContent* content =
        [[UNMutableNotificationContent alloc] init];
    content.title = [NSString stringWithUTF8String:sender.c_str()] ?: @"";
    if (roomName != sender)
    {
        content.subtitle =
            [NSString stringWithUTF8String:roomName.c_str()] ?: @"";
    }
    std::string preview =
        body.size() > 120 ? body.substr(0, 120) + "\xe2\x80\xa6" : body;
    content.body = [NSString stringWithUTF8String:preview.c_str()] ?: @"";
    content.sound = [UNNotificationSound defaultSound];
    content.threadIdentifier =
        [NSString stringWithUTF8String:roomId.c_str()] ?: @"";
    NSString* nsRoomId = content.threadIdentifier;
    NSString* nsUserId = [NSString stringWithUTF8String:userId.c_str()] ?: @"";
    content.userInfo = @{@"room_id" : nsRoomId, @"user_id" : nsUserId};

    // Notification picture: prefer the message image/sticker (already
    // privacy-gated upstream), fall back to the room avatar. UNNotification
    // has no app-icon override, so it shows as the banner thumbnail.
    // Written to a stable per-room temp file (bounded; macOS purges the
    // temp dir, and UN moves the file into its own store on success).
    const std::vector<std::uint8_t>& pic =
        !imageBytes.empty() ? imageBytes : avatarBytes;
    if (!pic.empty())
    {
        NSString* ext = @"png";
        if (pic.size() >= 3 && pic[0] == 0xFF && pic[1] == 0xD8 &&
            pic[2] == 0xFF)
        {
            ext = @"jpg";
        }
        else if (pic.size() >= 4 && pic[0] == 'G' && pic[1] == 'I' &&
                 pic[2] == 'F' && pic[3] == '8')
        {
            ext = @"gif";
        }
        else if (pic.size() >= 12 && pic[0] == 'R' && pic[1] == 'I' &&
                 pic[2] == 'F' && pic[3] == 'F' && pic[8] == 'W' &&
                 pic[9] == 'E' && pic[10] == 'B' && pic[11] == 'P')
        {
            ext = @"webp";
        }
        NSData* data = [NSData dataWithBytes:pic.data() length:pic.size()];
        NSString* dir = [NSTemporaryDirectory()
            stringByAppendingPathComponent:@"Tesseract/notif"];
        [[NSFileManager defaultManager] createDirectoryAtPath:dir
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nil];
        NSString* file = [NSString
            stringWithFormat:@"%zu.%@", std::hash<std::string>{}(roomId), ext];
        NSURL* url =
            [NSURL fileURLWithPath:[dir stringByAppendingPathComponent:file]];
        if ([data writeToURL:url atomically:YES])
        {
            NSError* attErr = nil;
            UNNotificationAttachment* att =
                [UNNotificationAttachment attachmentWithIdentifier:@"avatar"
                                                               URL:url
                                                           options:nil
                                                             error:&attErr];
            if (att)
            {
                content.attachments = @[ att ];
            }
        }
    }

    UNNotificationRequest* req =
        [UNNotificationRequest requestWithIdentifier:[NSUUID UUID].UUIDString
                                             content:content
                                             trigger:nil];
    [[UNUserNotificationCenter currentNotificationCenter]
        addNotificationRequest:req
         withCompletionHandler:nil];
}

// Suppress banner when the app is in the foreground and the relevant room
// is already open.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:
             (void (^)(UNNotificationPresentationOptions))completionHandler
{
    (void)center;
    NSDictionary* info = notification.request.content.userInfo;
    NSString* rid = info[@"room_id"];
    NSString* uid = info[@"user_id"];
    BOOL activeAccount =
        uid && _shell->active_account_index_ >= 0 &&
        (int)_shell->accounts_.size() > _shell->active_account_index_ &&
        _shell->accounts_[_shell->active_account_index_]->user_id ==
            std::string(uid.UTF8String ?: "");
    if (self.window.isKeyWindow && activeAccount && rid &&
        _shell->current_room_id_ == std::string(rid.UTF8String ?: ""))
    {
        completionHandler(UNNotificationPresentationOptionNone);
    }
    else
    {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
        completionHandler(UNNotificationPresentationOptionBanner |
                          UNNotificationPresentationOptionSound);
#else
        completionHandler(UNNotificationPresentationOptionAlert |
                          UNNotificationPresentationOptionSound);
#endif
    }
}

// Navigate to the room when the user taps/clicks the notification.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler
{
    (void)center;
    NSDictionary* info = response.notification.request.content.userInfo;
    NSString* rid = info[@"room_id"];
    NSString* uid = info[@"user_id"];
    // Switch to the account that owns this notification before navigating.
    if (uid)
    {
        std::string target_uid(uid.UTF8String ?: "");
        for (int i = 0; i < (int)_shell->accounts_.size(); ++i)
        {
            if (_shell->accounts_[i]->user_id == target_uid)
            {
                [self _switchActiveAccount:i];
                break;
            }
        }
    }
    if (rid)
    {
        [self _navigateToRoom:std::string(rid.UTF8String ?: "")];
    }
    completionHandler();
}

- (void)_navigateToRoom:(std::string)roomId
{
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    _shell->tab_navigate_room(roomId);
}

// ─────────────────────────────────────────────────────────────────────────
//  Event-bridge callbacks (main thread)
// ─────────────────────────────────────────────────────────────────────────

- (void)handleMessageInserted:(NSString*)roomId
                        index:(std::size_t)index
                        event:(tesseract::Event*)event
{
    std::unique_ptr<tesseract::Event> guard(event);
    if (!event)
    {
        return;
    }
    if (std::string(roomId.UTF8String ?: "") != _shell->current_room_id_)
    {
        return;
    }
    if (event->type == tesseract::EventType::Unhandled)
    {
        return;
    }
    _shell->ensure_row_media_(*event);
    if (!event->in_reply_to_id.empty())
    {
        _shell->ensure_reply_details_(event->event_id);
    }
    if (_roomView)
    {
        _roomView->insert_message(index, tesseract::views::make_row_data(
                                             *event, _shell->my_user_id_));
    }
    [self _relayoutChatSurface];
}

- (void)handleMessageUpdated:(NSString*)roomId
                       index:(std::size_t)index
                       event:(tesseract::Event*)event
{
    std::unique_ptr<tesseract::Event> guard(event);
    if (!event)
    {
        return;
    }
    if (std::string(roomId.UTF8String ?: "") != _shell->current_room_id_)
    {
        return;
    }
    if (event->type == tesseract::EventType::Unhandled)
    {
        return;
    }
    _shell->ensure_row_media_(*event);
    if (!event->in_reply_to_id.empty())
    {
        _shell->ensure_reply_details_(event->event_id);
    }
    if (_roomView)
    {
        _roomView->update_message(index, tesseract::views::make_row_data(
                                             *event, _shell->my_user_id_));
    }
    [self _relayoutChatSurface];
}

- (void)handleMessageRemoved:(NSString*)roomId index:(std::size_t)index
{
    if (std::string(roomId.UTF8String ?: "") != _shell->current_room_id_)
    {
        return;
    }
    if (_roomView)
    {
        _roomView->remove_message(index);
    }
    [self _relayoutChatSurface];
}

// updateRoomsForUserId: was the old ObjC EventBridge hook. EventHandlerBase now
// calls ShellBase::push_rooms_() directly, which invokes on_rooms_updated_()
// → _refreshRoomList + restore-room logic. No ObjC forwarding needed.

- (void)handleSyncErrorContext:(NSString*)ctx
                   description:(NSString*)desc
                    softLogout:(BOOL)soft
{
    if ([ctx isEqualToString:@"sync_auth_error"])
    {
        if (soft && _shell->client_ && _shell->active_account_index_ >= 0)
        {
            std::string uid =
                _shell->accounts_[_shell->active_account_index_]->user_id;
            if (auto saved = tesseract::SessionStore::load_account(uid))
            {
                if (_shell->client_->restore_session(*saved))
                {
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
    const bool current = roomId.UTF8String && std::string(roomId.UTF8String) ==
                                                  _shell->current_room_id_;
    if (current)
    {
        auto rows = _shell->build_rows_(snapshot);
        const std::string rid = roomId.UTF8String ? roomId.UTF8String : "";
        // A genuine switch, OR a re-population of an emptied view (e.g.
        // logout → login → same room): both warrant the display gate.
        const auto* ml = _roomView ? _roomView->message_list() : nullptr;
        const bool room_switch = _shell->view_displayed_room_id_ != rid ||
                                 (ml && ml->messages().empty());
        _shell->view_displayed_room_id_ = rid;
        if (_roomView)
        {
            _roomView->set_messages(std::move(rows), room_switch);
        }
        [self _relayoutChatSurface];
        if (_roomView && _roomView->message_list())
        {
            const auto& pstate = _shell->pagination_[rid];
            if (room_switch && pstate.is_focused)
            {
                _roomView->message_list()->begin_focused_gate(
                    pstate.focus_event_id);
            }
            _roomView->message_list()->set_historical_mode(pstate.is_focused);
            if (pstate.is_focused)
            {
                _roomView->message_list()->scroll_to_event_id(
                    pstate.focus_event_id);
            }
        }
    }
    for (auto* ev : snapshot)
    {
        delete ev;
    }
}

- (void)handleVerificationState:(BOOL)isVerified
{
    if (!_mainApp || !_mainAppSurface)
    {
        return;
    }
    if (!isVerified && !_shell->verification_banner_dismissed_)
    {
        if (!_verifShared->visible())
        {
            _verifShared->set_state(
                tesseract::views::VerificationBanner::State::Prompt);
            // Verification takes priority — hide recovery banner if it appeared
            // before the verification state callback arrived (race on first sync).
            // But if recovery is actively in progress (Verifying/Importing), let
            // it finish rather than interrupting with the verification banner.
            if (_recoveryShared && _recoveryShared->visible())
            {
                auto rs = _recoveryShared->state();
                if (rs == tesseract::views::RecoveryBanner::State::Form ||
                    rs == tesseract::views::RecoveryBanner::State::Failed)
                {
                    _mainApp->show_recovery_banner(false);
                }
                else
                {
                    return;
                }
            }
            _mainApp->show_verif_banner(true);
            _mainAppSurface->relayout();
        }
    }
    else if (_verifShared->visible())
    {
        _mainApp->show_verif_banner(false);
        _mainAppSurface->relayout();
    }
}

- (void)handleVerificationRequest:(std::string)flowId incoming:(BOOL)incoming
{
    if (!_verifShared || !_mainAppSurface)
    {
        return;
    }
    if (incoming)
    {
        _verifShared->set_state(
            tesseract::views::VerificationBanner::State::IncomingRequest);
    }
    else
    {
        _verifShared->set_state(
            tesseract::views::VerificationBanner::State::Waiting);
        if (_shell->client_)
        {
            _shell->client_->start_sas(_shell->active_verification_flow_id_);
        }
    }
    _mainApp->show_verif_banner(true);
    _mainAppSurface->relayout();
}

- (void)handleSasReady:(std::vector<tesseract::VerificationEmoji>)emojis
{
    if (!_verifShared || !_mainAppSurface)
    {
        return;
    }
    _verifShared->set_emojis(emojis);
    _mainApp->show_verif_banner(true);
    _mainAppSurface->relayout();
}

- (void)handleVerificationDone
{
    if (!_verifShared || !_mainAppSurface)
    {
        return;
    }
    _verifShared->set_state(tesseract::views::VerificationBanner::State::Done);
    _mainAppSurface->relayout();
    __weak MainWindowController* weakSelf = self;
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)),
        dispatch_get_main_queue(), ^{
            MainWindowController* s = weakSelf;
            if (s && s->_verifShared && s->_verifShared->on_done)
            {
                s->_verifShared->on_done();
            }
        });
}

- (void)handleVerificationCancelled:(std::string)reason
{
    if (!_verifShared || !_mainAppSurface)
    {
        return;
    }
    _verifShared->set_state(
        tesseract::views::VerificationBanner::State::Cancelled);
    _verifShared->set_cancel_reason(std::move(reason));
    _mainApp->show_verif_banner(true);
    _mainAppSurface->relayout();
}

- (void)handleBackupProgress:(tesseract::BackupProgress)progress
{
    [self _maybeShowRecoveryBanner];

    if (_mainApp && _recoveryShared && _recoveryShared->visible() &&
        _recoveryShared->state() ==
            tesseract::views::RecoveryBanner::State::Importing &&
        progress.state == tesseract::BackupState::Downloading &&
        progress.imported_keys > 0)
    {
        _recoveryShared->set_import_progress(progress.imported_keys);
        if (_mainAppSurface)
        {
            _mainAppSurface->relayout();
        }
    }
    if (progress.state == tesseract::BackupState::Enabled && _shell->client_ &&
        !_shell->client_->needs_recovery() && _mainApp)
    {
        _mainApp->show_recovery_banner(false);
        if (_mainAppSurface)
        {
            _mainAppSurface->relayout();
        }
    }
}

- (void)_relayoutRoomSurface
{
    if (_mainAppSurface)
    {
        _mainAppSurface->relayout();
    }
}

- (void)_relayoutChatSurface
{
    if (_mainAppSurface)
    {
        _mainAppSurface->relayout();
    }
}

- (void)_onRoomListStateChanged
{
    // Update window title with sync progress (no status bar on macOS).
    // Priority: Init/SettingUp → "Syncing rooms…",
    //           Recovering     → "Reconnecting…",
    //           keys busy      → "Downloading encryption keys (N)…",
    //           else           → clear progress suffix.
    using RLS = tesseract::RoomListState;
    using BS = tesseract::BackupState;

    const bool room_busy = (_shell->last_room_list_state_ == RLS::Init ||
                            _shell->last_room_list_state_ == RLS::SettingUp);
    const bool reconnecting =
        (_shell->last_room_list_state_ == RLS::Recovering);
    const bool keys_busy = (_shell->last_backup_state_ == BS::Downloading);

    NSString* suffix = nil;
    if (room_busy)
    {
        suffix = @" — Syncing rooms…";
    }
    else if (reconnecting)
    {
        suffix = @" — Reconnecting…";
    }
    else if (keys_busy)
    {
        suffix = [NSString
            stringWithFormat:@" — Downloading encryption keys (%llu)…",
                             (unsigned long long)_shell->last_imported_keys_];
    }

    if (suffix)
    {
        _shell->sync_progress_shown_ = true;
        self.window.title = [@"Tesseract" stringByAppendingString:suffix];
    }
    else if (_shell->sync_progress_shown_)
    {
        _shell->sync_progress_shown_ = false;
        self.window.title = @"Tesseract";
    }

    // Once Running, attempt the deferred room restore (we waited for Running
    // to avoid subscribing to a room during initial sync, which triggers the
    // imbl promote_front data race in matrix-sdk-ui).
    if (_shell->last_room_list_state_ == RLS::Running &&
        _shell->current_room_id_.empty() &&
        !_shell->pending_restore_room_.empty())
    {
        for (const auto& r : _shell->rooms_)
        {
            if (r.id == _shell->pending_restore_room_ && !r.is_space)
            {
                std::string target = std::move(_shell->pending_restore_room_);
                _shell->pending_restore_room_.clear();
                [self onRoomSelected:target];
                break;
            }
        }
    }
}

- (void)_onServerInfoReady
{
    if (_settingsView)
        _settingsView->set_server_info(_shell->server_info_);
    if (_mainApp && _mainApp->room_view())
        _mainApp->room_view()->header()->set_jump_to_date_enabled(
            _shell->server_info_.supports_msc3030);
    if (_mainAppSurface)
        _mainAppSurface->relayout();
}

- (void)_maybeShowRecoveryBanner
{
    if (_shell->recovery_banner_dismissed_)
    {
        return;
    }
    if (!_shell->client_ || !_shell->client_->needs_recovery())
    {
        return;
    }
    if (!_mainApp || !_mainAppSurface)
    {
        return;
    }
    // Verification takes priority — don't show recovery banner while the
    // verification banner is active. The "Use recovery key" link hands off.
    if (_verifShared && _verifShared->visible())
    {
        return;
    }
    if (_recoveryShared && !_recoveryShared->visible())
    {
        {
            _recoveryShared->set_state(
                tesseract::views::RecoveryBanner::State::Form);
            _recoveryShared->set_current_key("");
        }
        if (_recoveryKeyField)
        {
            _recoveryKeyField->set_text("");
            _recoveryKeyField->set_enabled(true);
        }
        _mainApp->show_recovery_banner(true);
        _mainAppSurface->relayout();
    }
}

- (void)_onRecoveryVerify
{
    std::string key;
    if (_recoveryKeyField)
    {
        key = _recoveryKeyField->text();
    }
    auto a = key.find_first_not_of(" \t\r\n");
    auto b = key.find_last_not_of(" \t\r\n");
    if (a == std::string::npos)
    {
        if (_recoveryShared)
        {
            _recoveryShared->set_state(
                tesseract::views::RecoveryBanner::State::Failed);
            _recoveryShared->set_failure_message(
                "Please enter a recovery key or passphrase.");
            if (_mainAppSurface)
            {
                _mainAppSurface->relayout();
            }
        }
        return;
    }
    key = key.substr(a, b - a + 1);

    if (_recoveryShared)
    {
        _recoveryShared->set_state(
            tesseract::views::RecoveryBanner::State::Verifying);
    }
    if (_recoveryKeyField)
    {
        _recoveryKeyField->set_enabled(false);
    }
    if (_mainAppSurface)
    {
        _mainAppSurface->relayout();
    }

    __weak MainWindowController* weakSelf = self;
    _shell->run_async_(
        [weakSelf, key, clientPtr = _shell->client_]()
        {
            auto res = clientPtr->recover(key);
            bool ok = res.ok;
            std::string msg = res.message;
            dispatch_async(dispatch_get_main_queue(), ^{
                MainWindowController* s = weakSelf;
                if (!s)
                {
                    return;
                }
                if (ok)
                {
                    if (s->_recoveryShared)
                    {
                        s->_recoveryShared->set_state(
                            tesseract::views::RecoveryBanner::State::Importing);
                    }
                }
                else
                {
                    if (s->_recoveryShared)
                    {
                        s->_recoveryShared->set_state(
                            tesseract::views::RecoveryBanner::State::Failed);
                        s->_recoveryShared->set_failure_message(msg);
                    }
                    if (s->_recoveryKeyField)
                    {
                        s->_recoveryKeyField->set_enabled(true);
                        s->_recoveryKeyField->set_focused(true);
                    }
                }
                if (s->_mainAppSurface)
                {
                    s->_mainAppSurface->relayout();
                }
            });
        });
}

- (void)_onRecoveryDismiss
{
    _shell->recovery_banner_dismissed_ = true;
    if (_mainApp)
    {
        _mainApp->show_recovery_banner(false);
        if (_mainAppSurface)
        {
            _mainAppSurface->relayout();
        }
    }
}

- (void)_refreshRoomList
{
    std::vector<tesseract::RoomInfo> filtered;
    if (_shell->space_stack_.empty())
    {
        std::unordered_set<std::string> in_space;
        if (_shell->client_)
        {
            for (const auto& r : _shell->rooms_)
            {
                if (!r.is_space)
                {
                    continue;
                }
                for (const auto& id : _shell->client_->space_children(r.id))
                {
                    in_space.insert(id);
                }
            }
        }
        filtered.reserve(_shell->rooms_.size());
        for (const auto& r : _shell->rooms_)
        {
            if (!r.is_space && (!in_space.count(r.id) || r.is_favorite))
            {
                filtered.push_back(r);
            }
        }
        for (const auto& r : _shell->rooms_)
        {
            if (r.is_space)
            {
                filtered.push_back(r);
            }
        }
        if (_mainApp)
        {
            _mainApp->set_space_nav(false);
        }
    }
    else
    {
        auto child_ids =
            _shell->client_
                ? _shell->client_->space_children(_shell->space_stack_.back())
                : std::vector<std::string>{};
        for (const auto& r : _shell->rooms_)
        {
            if (std::find(child_ids.begin(), child_ids.end(), r.id) !=
                child_ids.end())
            {
                filtered.push_back(r);
            }
        }
        std::string space_name;
        std::string space_avatar;
        for (const auto& r : _shell->rooms_)
        {
            if (r.id == _shell->space_stack_.back())
            {
                space_name = r.name;
                space_avatar = r.avatar_url;
                _shell->ensure_room_avatar_(r);
                break;
            }
        }
        if (_mainApp)
        {
            _mainApp->set_space_nav(true, space_name, space_avatar);
        }
    }
    for (const auto& r : filtered)
    {
        _shell->ensure_room_avatar_(r);
    }

    _roomListView->set_rooms(filtered);
    if (!_shell->current_room_id_.empty())
    {
        _roomListView->set_selected_room(_shell->current_room_id_);
    }
    if (_mainAppSurface)
    {
        _mainAppSurface->relayout();
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Room + message handling
// ─────────────────────────────────────────────────────────────────────────

- (void)_setComposeDraft:(const std::string&)draft
{
    if (_roomTextArea)
    {
        _roomTextArea->set_text(draft);
    }
    if (_roomView)
    {
        _roomView->set_current_text(draft);
    }
}

- (void)onRoomSelected:(std::string)roomId
{
    if (roomId.empty())
    {
        return;
    }
    for (const auto& r : _shell->rooms_)
    {
        if (r.id == roomId && r.is_space)
        {
            _shell->space_stack_.push_back(roomId);
            [self _refreshRoomList];
            return;
        }
    }
    [self hideShortcodePopup];
    _shell->handle_compose_room_leaving_(_shell->current_room_id_);
    if (!_shell->current_room_id_.empty() &&
        _shell->current_room_id_ != roomId &&
        _shell->room_subscription_refs_.count(_shell->current_room_id_) == 0)
    {
        _shell->client_->unsubscribe_room(_shell->current_room_id_);
    }
    _shell->current_room_id_ = roomId;
    _shell->clear_focused_state_(roomId);
    [_markReadTimer invalidate];
    double delayS =
        tesseract::Settings::instance().mark_as_read_delay_ms / 1000.0;
    __weak MainWindowController* weakSelf = self;
    _markReadTimer = [NSTimer
        scheduledTimerWithTimeInterval:delayS
                               repeats:NO
                                 block:^(NSTimer*) {
                                     MainWindowController* c = weakSelf;
                                     if (c)
                                     {
                                         c->_shell->mark_room_read_(
                                             c->_shell->current_room_id_);
                                     }
                                 }];
    _shell->reply_details_requested_.clear();
    {
        auto prefs =
            tesseract::Prefs::parse(_shell->client_->load_prefs_json());
        prefs.last_room = roomId;
        _shell->client_->save_prefs_json(tesseract::Prefs::serialize(prefs));
    }
    if (_roomView)
    {
        _roomView->compose_bar()->clear_reply();
        _roomView->compose_bar()->clear_editing();
    }
    if (_roomView)
    {
        _roomView->set_typing_text({});
    }
    if (_roomTextArea)
    {
        _roomTextArea->set_focused(true);
    }
    for (const auto& r : _shell->rooms_)
    {
        if (r.id == _shell->current_room_id_)
        {
            if (_roomView)
            {
                _roomView->set_room(r);
                [self _relayoutChatSurface];
            }
            break;
        }
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
        if (res)
        {
            auto pr =
                self->_shell->client_->paginate_back_with_status(subRoom, 50);
            reached = pr.ok && pr.reached_start;
            self->_shell->client_->start_background_backfill(visibleIds);
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            [self handleSubscribeResultForRoom:subRoom reached:reached];
        });
    });
}

- (void)requestMoreHistoryForRoom:(std::string)roomId
{
    if (roomId.empty())
    {
        return;
    }
    auto& state = _shell->pagination_[roomId];
    if (state.in_flight || state.reached_start)
    {
        return;
    }
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

- (void)openJumpToDateDialog
{
    if (_shell->current_room_id_.empty())
    {
        return;
    }

    // Build an NSDatePicker in graphical calendar mode.
    NSDatePicker* picker =
        [[NSDatePicker alloc] initWithFrame:NSMakeRect(0, 0, 228, 148)];
    picker.datePickerStyle = NSDatePickerStyleClockAndCalendar;
    picker.datePickerElements = NSDatePickerElementFlagYearMonthDay;
    picker.timeZone = [NSTimeZone timeZoneWithName:@"UTC"];

    // Clamp selection to [1970-01-01, today].
    NSCalendar* utcCal =
        [NSCalendar calendarWithIdentifier:NSCalendarIdentifierGregorian];
    utcCal.timeZone = [NSTimeZone timeZoneWithName:@"UTC"];
    NSDateComponents* epochComps = [[NSDateComponents alloc] init];
    epochComps.year = 1970;
    epochComps.month = 1;
    epochComps.day = 1;
    picker.minDate = [utcCal dateFromComponents:epochComps];
    picker.maxDate = [NSDate date];

    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Jump to Date";
    alert.informativeText = @"Navigate to messages from the selected day.";
    [alert addButtonWithTitle:@"Jump"];
    [alert addButtonWithTitle:@"Cancel"];
    alert.accessoryView = picker;

    [alert
        beginSheetModalForWindow:self.window
               completionHandler:^(NSModalResponse r) {
                   if (r != NSAlertFirstButtonReturn)
                   {
                       return;
                   }

                   // Extract midnight UTC from the selected date.
                   NSDate* selected = picker.dateValue;
                   NSDateComponents* comps = [utcCal
                       components:(NSCalendarUnitYear | NSCalendarUnitMonth |
                                   NSCalendarUnitDay)
                         fromDate:selected];
                   comps.hour = 0;
                   comps.minute = 0;
                   comps.second = 0;
                   NSDate* midnight = [utcCal dateFromComponents:comps];
                   const uint64_t ts_ms = static_cast<uint64_t>(
                       [midnight timeIntervalSince1970] * 1000.0);

                   const std::string room_id = self->_shell->current_room_id_;
                   if (room_id.empty())
                   {
                       return;
                   }

                   dispatch_async(
                       dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                       ^{
                           auto res = self->_shell->client_->timestamp_to_event(
                               room_id, ts_ms, "f");
                           if (!res.ok)
                           {
                               NSString* msg =
                                   [NSString
                                       stringWithUTF8String:res.message.c_str()]
                                       ?: @"Unknown error";
                               dispatch_async(dispatch_get_main_queue(), ^{
                                   NSAlert* err = [[NSAlert alloc] init];
                                   err.alertStyle = NSAlertStyleWarning;
                                   err.messageText = @"Jump to Date Failed";
                                   err.informativeText = msg;
                                   [err beginSheetModalForWindow:self.window
                                               completionHandler:nil];
                               });
                               return;
                           }
                           const std::string event_id = res.message;
                           dispatch_async(dispatch_get_main_queue(), ^{
                               self->_shell->begin_focused_subscription_(
                                   room_id, event_id);
                               dispatch_async(
                                   dispatch_get_global_queue(
                                       QOS_CLASS_USER_INITIATED, 0),
                                   ^{
                                       self->_shell->client_->subscribe_room_at(
                                           room_id, event_id);
                                   });
                           });
                       });
               }];
}

- (void)handlePaginateResultForRoom:(std::string)roomId
                      reached_start:(BOOL)reached
{
    auto it = _shell->pagination_.find(roomId);
    if (it == _shell->pagination_.end())
    {
        return;
    }
    it->second.in_flight = false;
    it->second.reached_start = reached;
    if (roomId == _shell->current_room_id_ && _roomView)
    {
        _roomView->message_list()->reset_near_top_latch();
    }
}

- (void)handleSubscribeResultForRoom:(std::string)roomId reached:(BOOL)reached
{
    if (roomId != _shell->current_room_id_)
    {
        return;
    }
    auto& state = _shell->pagination_[roomId];
    state.in_flight = false;
    state.reached_start = reached;
}

// Note: _ensureRowMedia is now handled by ShellBase::ensure_row_media_() via
// MacShell. MacShell::generate_video_thumbnail_ handles client-side first-frame
// generation for m.video when the server provides no thumbnail.

// ─────────────────────────────────────────────────────────────────────────
//  Animated sticker support
// ─────────────────────────────────────────────────────────────────────────

- (void)_decodeMediaBytes:(const std::vector<uint8_t>&)bytes
                   forKey:(const std::string&)key
{
    if (bytes.empty() || _shell->tk_images_.count(key) ||
        _shell->anim_cache_.has(key))
    {
        return;
    }
    auto d = _shell->decode_image_(bytes, 0, 0);
    if (!d.frames.empty())
    {
        const std::int64_t now = static_cast<std::int64_t>(
            [[NSDate date] timeIntervalSince1970] * 1000.0);
        _shell->anim_cache_.store(key, std::move(d.frames),
                                  std::move(d.delays_ms), now);
        [self _startAnimTickIfNeeded];
    }
    else if (d.still)
    {
        _shell->tk_images_.emplace(key, std::move(d.still));
    }
}

- (void)_startAnimTickIfNeeded
{
    if (_animTimer || _shell->anim_cache_.empty())
    {
        return;
    }
    __weak MainWindowController* weakSelf = self;
    _animTimer = [NSTimer scheduledTimerWithTimeInterval:0.016
                                                 repeats:YES
                                                   block:^(NSTimer* t) {
                                                       [weakSelf _animTick:t];
                                                   }];
    [[NSRunLoop currentRunLoop] addTimer:_animTimer
                                 forMode:NSRunLoopCommonModes];
}

- (void)_animTick:(NSTimer*)timer
{
    if (_shell->anim_cache_.empty())
    {
        [_animTimer invalidate];
        _animTimer = nil;
        return;
    }
    const std::int64_t now = static_cast<std::int64_t>(
        [[NSDate date] timeIntervalSince1970] * 1000.0);
    if (_shell->anim_cache_.advance(now))
    {
        [self _relayoutChatSurface];
        StickerPickerPanel* panel = [StickerPickerPanel sharedPanel];
        if (panel.isVisible)
        {
            [panel invalidateImageCache];
        }
    }
}

- (void)_ensureStickerImageAsync:(std::string)url
{
    _shell->ensure_picker_image_(url, /*is_sticker=*/true);
}

- (void)_ensureEmojiImageAsync:(std::string)url
{
    _shell->ensure_picker_image_(url, /*is_sticker=*/false);
}

// ─────────────────────────────────────────────────────────────────────────
//  Sticker picker
// ─────────────────────────────────────────────────────────────────────────

- (void)handleImagePacksUpdated
{
    StickerPickerPanel* panel = [StickerPickerPanel sharedPanel];
    panel.client = _shell->client_;
    [panel refreshPacks];
}

- (void)_showStickerPicker
{
    if (!_mainAppSurface || _shell->current_room_id_.empty())
    {
        return;
    }
    StickerPickerPanel* panel = [StickerPickerPanel sharedPanel];
    panel.client = _shell->client_;

    __weak MainWindowController* weakSelf = self;

    [panel
        setImageProvider:[weakSelf](const std::string& cache_key,
                                    const std::string& /*source_token*/)
                             -> const tk::Image*
                         {
                             MainWindowController* s = weakSelf;
                             if (!s)
                             {
                                 return nullptr;
                             }
                             if (auto* f = s->_shell->anim_cache_.current_frame(
                                     cache_key))
                             {
                                 return f;
                             }
                             auto it = s->_shell->tk_images_.find(cache_key);
                             if (it != s->_shell->tk_images_.end())
                             {
                                 return it->second.get();
                             }
                             [s _ensureStickerImageAsync:cache_key];
                             return nullptr;
                         }];

    __weak StickerPickerPanel* weakPanel = panel;
    panel.onSelected = ^(NSString* url, NSString* body, NSString* infoJson) {
        MainWindowController* s = weakSelf;
        if (!s || s->_shell->current_room_id_.empty())
        {
            return;
        }
        std::string u = url.UTF8String ?: "";
        std::string b = body.UTF8String ?: "";
        std::string j = infoJson.UTF8String ?: "{}";
        s->_shell->client_->send_sticker(s->_shell->current_room_id_, b, u, j);
        [weakPanel orderOut:nil];
    };

    NSView* anchor = (__bridge NSView*)_mainAppSurface->view_handle();
    [panel popupAboveView:anchor];
}

- (void)_showStickerPickerAtRect:(tk::Rect)btn
{
    if (!_mainAppSurface || _shell->current_room_id_.empty())
    {
        return;
    }
    StickerPickerPanel* panel = [StickerPickerPanel sharedPanel];
    panel.client = _shell->client_;

    __weak MainWindowController* weakSelf = self;

    [panel
        setImageProvider:[weakSelf](const std::string& cache_key,
                                    const std::string& /*source_token*/)
                             -> const tk::Image*
                         {
                             MainWindowController* s = weakSelf;
                             if (!s)
                             {
                                 return nullptr;
                             }
                             if (auto* f = s->_shell->anim_cache_.current_frame(
                                     cache_key))
                             {
                                 return f;
                             }
                             auto it = s->_shell->tk_images_.find(cache_key);
                             if (it != s->_shell->tk_images_.end())
                             {
                                 return it->second.get();
                             }
                             [s _ensureStickerImageAsync:cache_key];
                             return nullptr;
                         }];

    __weak StickerPickerPanel* weakPanel = panel;
    panel.onSelected = ^(NSString* url, NSString* body, NSString* infoJson) {
        MainWindowController* s = weakSelf;
        if (!s || s->_shell->current_room_id_.empty())
        {
            return;
        }
        std::string u = url.UTF8String ?: "";
        std::string b = body.UTF8String ?: "";
        std::string j = infoJson.UTF8String ?: "{}";
        s->_shell->client_->send_sticker(s->_shell->current_room_id_, b, u, j);
        [weakPanel orderOut:nil];
    };

    NSView* anchor = (__bridge NSView*)_mainAppSurface->view_handle();
    [panel popupAtRect:btn inView:anchor];
}

- (void)_showStickerContextMenuAt:(NSPoint)screenPt
{
    if (_ctxStickerMxcUrl.empty())
    {
        return;
    }
    BOOL alreadySaved =
        _shell->client_->user_pack_has_sticker(_ctxStickerMxcUrl,
                                               _ctxStickerInfoJson);
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Sticker"];
    NSMenuItem* item = [[NSMenuItem alloc]
        initWithTitle:alreadySaved ? @"Already in Saved Stickers"
                                   : @"Add to Saved Stickers"
               action:alreadySaved ? nil : @selector(_onStickerSave:)
        keyEquivalent:@""];
    item.target = self;
    [menu addItem:item];
    [menu popUpMenuPositioningItem:nil atLocation:screenPt inView:nil];
}

- (void)_onStickerSave:(id)sender
{
    if (_ctxStickerMxcUrl.empty())
    {
        return;
    }
    auto res = _shell->client_->save_sticker_to_user_pack(
        _ctxStickerBody, _ctxStickerBody, _ctxStickerMxcUrl,
        _ctxStickerInfoJson);
    _ctxStickerEventId.clear();
    _ctxStickerMxcUrl.clear();
    _ctxStickerBody.clear();
    _ctxStickerInfoJson.clear();
    if (!res.ok)
    {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.alertStyle = NSAlertStyleWarning;
        alert.messageText = @"Could Not Save Sticker";
        alert.informativeText =
            [NSString stringWithUTF8String:res.message.c_str()] ?: @"";
        [alert beginSheetModalForWindow:self.window completionHandler:nil];
    }
}

// ─── Cmd+C for message-list text selection ────────────────────────────────

- (BOOL)validateUserInterfaceItem:(id<NSValidatedUserInterfaceItem>)item
{
    if (item.action == @selector(copy:))
    {
        auto* ml = _mainApp ? _mainApp->room_view()->message_list() : nullptr;
        return ml && ml->has_selection();
    }
    return YES;
}

- (void)copy:(id)sender
{
    auto* ml = _mainApp ? _mainApp->room_view()->message_list() : nullptr;
    if (ml)
        ml->copy_selection();
}

@end
