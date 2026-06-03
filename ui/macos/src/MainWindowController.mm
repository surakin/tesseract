#import "MainWindowController.h"
#import "tk_locale.h"
#import "LoginView.h"
#import "EmojiPicker.h"
#import "StickerPicker.h"
#import "MacOSTrayIcon.h"
#import "MacScreenLock.h"
#import "RoomWindowController.h"

#include <tesseract/client.h>
#include <tesseract/event_handler.h>
#include <tesseract/image_pack.h>
#include <tesseract/session_store.h>
#include <tesseract/prefs.h>
#include <tesseract/paths.h>
#include <tesseract/settings.h>
#include <tesseract/visual.h>

#include "app/SlashCommands.h"
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
#include "views/MentionController.h"
#include "views/MentionPopup.h"
#include "views/SlashCommandEngine.h"
#include "views/SlashCommandPopup.h"
#include <tesseract/mentions.h>

#include <ImageIO/ImageIO.h>
#import <AVFoundation/AVFoundation.h>
#import <UserNotifications/UserNotifications.h>
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#endif

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

public:
    void post_to_ui_(std::function<void()> fn) override;
    void post_to_ui_after_(int ms, std::function<void()> fn) override;
    void request_relayout_() override;
    void request_repaint_() override;
    void on_invites_updated_() override;
    void handle_verification_state_ui_(bool is_verified) override;

protected:
    void on_rooms_updated_() override;
    void on_space_children_cache_ready_ui_() override;
    void show_encryption_setup_overlay_(
        tesseract::views::EncryptionSetupOverlay::Mode mode) override;
    void on_tray_unread_changed_(bool has_unread,
                                 bool has_highlight) override;
    void on_media_bytes_ready_(const std::string& key,
                               ShellBase::MediaKind kind,
                               std::vector<uint8_t> bytes) override;
    void generate_video_thumbnail_(const std::string& event_id,
                                   const std::string& video_url) override;
    void cache_rgba_image_(const std::string& key, int w, int h,
                           std::vector<uint8_t> rgba) override;

    std::int64_t monotonic_ms_() override;
    void start_anim_tick_() override;
    void stop_anim_tick_() override;
    void repaint_anim_frame_() override;
    void repaint_pickers_() override;

    void handle_timeline_reset_ui_(
        std::string room_id,
        tesseract::EventList snapshot) override;
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
    void handle_notification_ui_(std::string user_id, std::string room_id,
                                 std::string room_name, std::string sender,
                                 std::string body, bool is_mention,
                                 std::vector<uint8_t> avatar_bytes,
                                 std::vector<uint8_t> image_bytes) override;
    void on_room_list_state_ui_() override;
    void on_inflight_ui_() override;
    void on_server_info_ready_ui_() override;
    void update_typing_bar_(const std::string& text, bool visible) override;

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
    void navigate_to_room_(const std::string& room_id) override
    {
        tab_navigate_room(room_id);
    }

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
    using ShellBase::begin_crypto_identity_reset_;
    using ShellBase::begin_focused_subscription_;
    using ShellBase::build_rows_;
    using ShellBase::cached_emoticons_;
    using ShellBase::cancel_debounce_;
    using ShellBase::clear_all_caches_;
    using ShellBase::clear_focused_state_;
    using ShellBase::client_;
    using ShellBase::compose_typing_active_;
    using ShellBase::compute_cache_sizes_;
    using ShellBase::current_room_id_;
    using ShellBase::current_thread_root_;
    using ShellBase::debounce_;
    using ShellBase::DebounceSlot;
    DecodedImage decode_image_(const std::vector<uint8_t>& bytes, int max_w,
                               int max_h) override;
    void pick_image_file_(
        std::function<void(std::vector<uint8_t>, std::string)> cb) override;
    using ShellBase::emoji_fetches_in_flight_;
    using ShellBase::ensure_media_image_;
    using ShellBase::ensure_picker_image_;
    using ShellBase::ensure_reply_details_;
    using ShellBase::ensure_room_avatar_;
    using ShellBase::ensure_row_media_;
    using ShellBase::ensure_tile_async;
    using ShellBase::ensure_user_avatar_;
    using ShellBase::event_handler_;
    using ShellBase::find_existing_dm_;
    using ShellBase::handle_compose_room_leaving_;
    using ShellBase::handle_compose_text_changed_;
    using ShellBase::handle_send_presence_toggle_;
    using ShellBase::apply_media_preview_config_;
    using ShellBase::inflight_dot_color_;
    using ShellBase::inflight_total_;
    using ShellBase::last_backup_state_;
    using ShellBase::last_imported_keys_;
    using ShellBase::last_inflight_;
    using ShellBase::last_room_list_state_;
    using ShellBase::last_tray_highlight_;
    using ShellBase::last_tray_unread_;
    using ShellBase::mark_room_read_;
    using ShellBase::navigate_tray_unread_;
    using ShellBase::maybe_send_read_receipt_;
    using ShellBase::media_fetches_in_flight_;
    using ShellBase::my_avatar_url_;
    using ShellBase::my_display_name_;
    using ShellBase::my_user_id_;
    using ShellBase::notify_presence_logout_;
    using ShellBase::notify_presence_tick_;
    using ShellBase::pagination_;
    using ShellBase::pending_login_client_;
    using ShellBase::pending_login_is_add_account_;
    using ShellBase::pending_login_temp_dir_;
    using ShellBase::pending_restore_rooms_;
    using ShellBase::try_restore_tab_session_;
    using ShellBase::per_account_rooms_;
    using ShellBase::per_account_invites_;
    using ShellBase::current_invite_;
    using ShellBase::pick_and_set_room_avatar_;
    using ShellBase::push_paginate_result_;
    using ShellBase::push_room_list_state_;
    using ShellBase::notify_tray_unread_;
    using ShellBase::notify_user_activity_;
    using ShellBase::notify_window_active_;
    using ShellBase::push_rooms_;
    using ShellBase::encryption_setup_shown_;
    using ShellBase::encryption_setup_dismissed_;
    using ShellBase::reply_details_requested_;
    using ShellBase::request_forward_history_;
    using ShellBase::return_to_live_;
    using ShellBase::room_subscription_refs_;
    using ShellBase::invites_;
    using ShellBase::rooms_;
    using ShellBase::run_async_;
    using ShellBase::set_screen_lock_;
    using ShellBase::set_theme_preference_;
    using ShellBase::shortcode_for_mxc_;
    using ShellBase::pool_;
    using ShellBase::mut_pool_;
    using ShellBase::init_pool_callbacks_;
    using ShellBase::pool_pending_count_;
    using ShellBase::mut_pool_pending_count_;
    using ShellBase::pending_media_count_;
    using ShellBase::run_async_mut_;
    using ShellBase::apply_space_child_counts_;
    using ShellBase::space_children_cache_;
    using ShellBase::space_stack_;
    using ShellBase::sticker_fetches_in_flight_;
    using ShellBase::sync_progress_shown_;
    using ShellBase::tab_close;
    using ShellBase::tab_navigate_room;
    using ShellBase::tab_open_room;
    using ShellBase::tab_select_room;
    using ShellBase::tabs_;
    using ShellBase::ThreadPanel;
    using ShellBase::thread_panel_;
    using ShellBase::tick_anim_;
    using ShellBase::thumbnail_cache_;
    using ShellBase::image_cache_;
    using ShellBase::url_preview_data_;
    using ShellBase::verification_banner_dismissed_;
    using ShellBase::video_thumb_in_flight_;
    using ShellBase::view_displayed_room_id_;
    using ShellBase::voice_bytes_or_fetch_;
    using ShellBase::voice_prefetched_;
    using ShellBase::capture_;
    using ShellBase::wire_main_app_viewers_;
    using ShellBase::wire_main_app_widget_;
    using ShellBase::wire_voice_capture_;
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
    // main_app_ / room_view_ now live in ShellBase; re-export them so the ObjC
    // side can keep reaching them through _shell. app_surface_ is the macOS
    // native surface and stays here.
    using ShellBase::main_app_;
    using ShellBase::room_view_;
    tk::macos::Surface* app_surface_ = nullptr;

    // Native text fields for the encryption-setup overlay. Owned here (not in
    // ObjC ivars) so show_encryption_setup_overlay_() can reach them from C++.
    std::unique_ptr<tk::NativeTextField> enc_passphrase_field_;
    std::unique_ptr<tk::NativeTextField> enc_key_field_;

    // Public forwarder for the protected ShellBase virtual so ObjC++ code can
    // call it through _shell without a friend declaration.
    void show_encryption_setup(
        tesseract::views::EncryptionSetupOverlay::Mode mode)
    {
        show_encryption_setup_overlay_(mode);
    }

    // SettingsController — created at login, reset on account switch.
    std::unique_ptr<tesseract::SettingsController> settings_controller_;

    // Shortcode engine + transient state (owned here, accessed via _shell->).
    tesseract::views::ShortcodeEngine shortcode_engine_;
    tesseract::views::ShortcodeMatch shortcode_active_match_{};
    std::vector<tesseract::views::ShortcodeSuggestion>
        shortcode_current_suggestions_;

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
- (void)showShortcodePopupWithSuggestions:
            (const std::vector<tesseract::views::ShortcodeSuggestion>&)
                suggestions
                               cursorRect:(tk::Rect)cursor;
- (void)hideShortcodePopup;
- (BOOL)shortcodePopupVisible;
- (void)showSlashPopupWithSuggestions:
            (const std::vector<tesseract::views::SlashCommandSuggestion>&)
                suggestions
                           cursorRect:(tk::Rect)cursor;
- (void)hideSlashPopup;
- (BOOL)slashPopupVisible;
- (void)showMentionPopupAtCursor:(tk::Rect)cursor rows:(int)rows;
- (void)hideMentionPopup;
- (BOOL)mentionPopupVisible;
- (void)_relayoutMentionPopupIfVisible;
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
- (void)_refreshInviteList;
- (void)_relayoutRoomSurface;
- (void)_relayoutChatSurface;
- (void)_onRoomListStateChanged;
- (void)_onServerInfoReady;
- (void)_buildStatusBar:(NSView*)content;
- (void)_refreshSyncStatus;
- (void)_onInflightChanged;
- (void)_updateTrayUnread:(bool)hasUnread highlight:(bool)hasHighlight;

// Sticker picker + animated stickers.
- (void)handleImagePacksUpdated;
- (void)_showStickerPicker;
- (void)_showStickerPickerAtRect:(tk::Rect)btn;
- (void)_showStickerContextMenuAt:(NSPoint)screenPt;
- (void)_onStickerSave:(id)sender;
- (void)_startAnimTickIfNeeded;
- (void)_stopAnimTick;
- (void)_animTick:(NSTimer*)timer;
- (void)_ensureStickerImageAsync:(std::string)url;
- (void)_ensureEmojiImageAsync:(std::string)url;
- (void)_applyTheme:(const tk::Theme&)t;
- (void)_decodeMediaBytes:(const std::vector<uint8_t>&)bytes
                   forKey:(const std::string&)key
                thumbnail:(BOOL)thumb;
- (void)_onSpaceBack;
- (void)_onComposeSend;
- (void)_relayoutShortcodePopupIfVisible;
- (void)_relayoutSlashPopupIfVisible;
- (void)_relayoutAccountPickerIfVisible;
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

void MacShell::post_to_ui_after_(int ms, std::function<void()> fn)
{
    auto* heap = new std::function<void()>(std::move(fn));
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms * NSEC_PER_MSEC),
        dispatch_get_main_queue(), ^{
            (*heap)();
            delete heap;
        });
}

void MacShell::request_relayout_()
{
    if (ctrl_)
    {
        [ctrl_ _relayoutChatSurface];
    }
}

void MacShell::request_repaint_()
{
    if (app_surface_)
    {
        app_surface_->host().request_repaint();
    }
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
    else if (!pending_restore_rooms_.empty() &&
             last_room_list_state_ == tesseract::RoomListState::Running)
    {
        if (try_restore_tab_session_(pending_restore_rooms_,
                                     pending_restore_rooms_[0]))
            pending_restore_rooms_.clear();
    }

    update_secondary_room_infos_();
}

void MacShell::on_invites_updated_()
{
    MainWindowController* c = ctrl_;
    if (!c)
        return;
    [c _refreshInviteList];
}

void MacShell::on_space_children_cache_ready_ui_()
{
    if (ctrl_)
    {
        [ctrl_ _refreshRoomList];
    }
}

void MacShell::show_encryption_setup_overlay_(
    tesseract::views::EncryptionSetupOverlay::Mode mode)
{
    MainWindowController* c = ctrl_;
    if (!c || !main_app_)
        return;
    auto* ov = main_app_->encryption_setup();
    if (!ov)
        return;

    // Reconfigure the overlay (clears prior callbacks) before re-creating the
    // native fields, then wire the shared callbacks via ShellBase.
    ov->reset(mode);

    enc_passphrase_field_ = app_surface_->host().make_text_field();
    enc_passphrase_field_->set_password(true);
    enc_key_field_ = app_surface_->host().make_text_field();
    enc_key_field_->set_password(false);

    wire_encryption_setup_callbacks_(*ov, app_surface_->host(),
                                     enc_passphrase_field_.get(),
                                     enc_key_field_.get());

    main_app_->show_encryption_setup(true);
    if (app_surface_)
        app_surface_->relayout();
}

void MacShell::on_tray_unread_changed_(bool has_unread, bool has_highlight)
{
    MainWindowController* c = ctrl_;
    if (!c)
    {
        return;
    }
    [c _updateTrayUnread:has_unread highlight:has_highlight];
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
        [c _decodeMediaBytes:bytes forKey:key thumbnail:NO];
        [c _relayoutChatSurface];
        [c _relayoutShortcodePopupIfVisible];
        return;
    }
    if (kind == MediaKind::MediaThumbnail)
    {
        [c _decodeMediaBytes:bytes forKey:key thumbnail:YES];
        [c _relayoutChatSurface];
        [c _relayoutShortcodePopupIfVisible];
        return;
    }
    if (kind == MediaKind::Tile)
    {
        if (image_cache_.contains(key))
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
        image_cache_.store(key, tk::cg::make_image(img));
        CGImageRelease(img);
        if (room_view_)
        {
            room_view_->message_list()->invalidate_data();
        }
        [c _relayoutChatSurface];
        return;
    }
    if (bytes.empty() || thumbnail_cache_.contains(key))
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
    thumbnail_cache_.store(key, tk::cg::make_image(img));
    CGImageRelease(img);
    if (kind == MediaKind::RoomAvatar)
    {
        [c _relayoutRoomSurface];
    }
    else if (kind == MediaKind::UserAvatar)
    {
        [c _relayoutChatSurface];
        [c _relayoutAccountPickerIfVisible];
        [c _relayoutMentionPopupIfVisible];
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
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            CGImageRef frame = [gen copyCGImageAtTime:t
                                           actualTime:nil
                                                error:&err];
#pragma clang diagnostic pop
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
                    if (image_cache_.contains(key))
                    {
                        return;
                    }
                    image_cache_.store(key, std::move(*img_holder));
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
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            NSArray<AVAssetTrack*>* vTracks =
                [asset tracksWithMediaType:AVMediaTypeVideo];
#pragma clang diagnostic pop
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
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            CGImageRef frame = [gen copyCGImageAtTime:kCMTimeZero
                                           actualTime:nil
                                                error:&err];
#pragma clang diagnostic pop
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
    if (image_cache_.contains(key))
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
    image_cache_.store(key, tk::cg::make_image(img));
    CGImageRelease(img);
    MainWindowController* c = ctrl_;
    if (c)
    {
        [c _relayoutChatSurface];
    }
}

void MacShell::pick_image_file_(
    std::function<void(std::vector<uint8_t>, std::string)> cb)
{
    NSOpenPanel* panel = [NSOpenPanel openPanel];
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
    panel.allowedContentTypes = @[ UTTypeImage ];
#else
    panel.allowedFileTypes = @[ @"public.image" ];
#endif
    panel.canChooseFiles         = YES;
    panel.allowsMultipleSelection = NO;
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
            auto callback = std::move(cb);
            callback(std::move(bytes), mime);
        });
    }];
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

void MacShell::stop_anim_tick_()
{
    if (ctrl_)
    {
        [ctrl_ _stopAnimTick];
    }
}

void MacShell::repaint_anim_frame_()
{
    // Repaint only the animated-image rects, not the whole chat surface (and
    // no per-frame relayout — frame swaps never change layout).
    if (app_surface_)
    {
        app_surface_->update_anim_regions();
    }
    StickerPickerPanel* panel = [StickerPickerPanel sharedPanel];
    if (panel.isVisible)
    {
        [panel invalidateImageCache];
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
    tesseract::EventList snapshot)
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
    apply_notification_redaction_(sender, room_name, body, avatar_bytes,
                                  image_bytes);
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

void MacShell::on_room_list_state_ui_()
{
    MainWindowController* c = ctrl_;
    if (c)
    {
        [c _onRoomListStateChanged];
    }
}

void MacShell::on_inflight_ui_()
{
    if (ctrl_)
        [ctrl_ _onInflightChanged];
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

tesseract::RoomWindowBase*
MacShell::create_secondary_room_window_(const std::string& room_id)
{
    return tesseract::make_mac_room_window(this, room_id);
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
                const std::string& av_mxc = r.effective_avatar_url();
                if (!av_mxc.empty())
                {
                    avatar = thumbnail_cache_.peek(av_mxc);
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

    // Settings name field — positioned via _settingsSurface->set_on_layout().
    std::unique_ptr<tk::NativeTextField> _settingsNameField;

    // Borrowed sub-view aliases (set after building _mainAppSurface).
    tesseract::views::RoomListView* _roomListView;      // via _mainApp
    tesseract::views::RoomView* _roomView;              // via _mainApp
    tesseract::views::VerificationBanner* _verifShared; // via _mainApp
    tesseract::views::ImageViewerOverlay* _imgViewer;   // via _mainApp
    tesseract::views::VideoViewerOverlay* _vidViewer;   // via _mainApp

    // Shortcode suggestion popup — NSPanel hosting a tk::macos::Surface.
    NSPanel* _shortcodePanel;
    std::unique_ptr<tk::macos::Surface> _shortcodePopupSurface;
    tesseract::views::ShortcodePopup*
        _shortcodePopupWidget; // borrowed from root

    // @mention autocomplete popup — NSPanel hosting a tk::macos::Surface,
    // driven by the shared MentionController.
    NSPanel* _mentionPanel;
    std::unique_ptr<tk::macos::Surface> _mentionPopupSurface;
    tesseract::views::MentionPopup* _mentionPopupWidget; // borrowed from root
    std::unique_ptr<tesseract::views::MentionController> _mentionController;

    // Slash-command autocomplete popup — NSPanel hosting a tk::macos::Surface.
    NSPanel* _slashPanel;
    std::unique_ptr<tk::macos::Surface> _slashPopupSurface;
    tesseract::views::SlashCommandPopup*
        _slashPopupWidget; // borrowed from root
    tesseract::views::SlashCommandEngine _slashEngine;

    // AppKit chrome.
    LoginView* _loginView;

    // Status bar: container view, sync-state label, in-flight dot.
    NSView*      _statusBarView;
    NSTextField* _statusLabel;
    NSTextField* _inflightDotLabel;

    NSTimer* _animTimer;
    NSTimer* _markReadTimer;
    NSTimer* _presenceTickTimer;

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

    // Floating tooltip popover for cache hit/miss rows in Settings > About.
    NSPopover* _cacheTooltipPopover;

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
    // Load saved settings before _buildChrome wires the main app widget.
    tesseract::Settings::instance().load_from_disk(tesseract::config_dir());
    [self _buildChrome];
    // Apply the loaded theme to all surfaces created by _buildChrome.
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
    }
    else
    {
        [NSApp terminate:nil];
    }
    return NO;
}

- (void)_buildChrome
{
    NSView* content = self.window.contentView;
    content.wantsLayer = YES;

    // ── Single surface hosting the full main-app widget tree ──────────
    _mainAppSurface = std::make_unique<tk::macos::Surface>(tk::Theme::light());
    // Let the animation timer repaint only the rects where animated images are
    // drawn (see _animTick) instead of the whole surface.
    _mainAppSurface->set_anim_cache(&_shell->anim_cache_);
    // Feed pointer / wheel events into the PresenceTracker.
    _mainAppSurface->host().set_on_user_activity(
        [shell = _shell.get()] { if (shell) shell->notify_user_activity_(); });

    // 30 s periodic tick for the idle-decay check.
    __weak MainWindowController* weakSelf = self;
    _presenceTickTimer =
        [NSTimer scheduledTimerWithTimeInterval:30.0
                                        repeats:YES
                                          block:^(NSTimer*) {
                                              MainWindowController* s = weakSelf;
                                              if (s && s->_shell)
                                              {
                                                  s->_shell->notify_presence_tick_();
                                              }
                                          }];
    [[NSRunLoop currentRunLoop] addTimer:_presenceTickTimer
                                 forMode:NSRunLoopCommonModes];
    {
        auto main_app_owner =
            std::make_unique<tesseract::views::MainAppWidget>();
        _mainApp = main_app_owner.get();

        // Wire borrowed sub-view aliases.
        _roomListView = _mainApp->room_list_view();
        _roomView = _mainApp->room_view();
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
        // (image_provider is wired in wire_main_app_widget_ below.)
        __weak MainWindowController* weakSelf = self;
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
            const std::string& _name_ref = s->_shell->my_display_name_.empty()
                ? s->_shell->my_user_id_
                : s->_shell->my_display_name_;
            NSString* logoutTitle = [NSString
                stringWithUTF8String:tk::trf(tk::tr("Log Out {0}"),
                                             {_name_ref})
                                         .c_str()];
            NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
            [menu addItemWithTitle:TkTr("Settings\xe2\x80\xa6")
                            action:@selector(_openSettings)
                     keyEquivalent:@""];
            [menu addItemWithTitle:TkTr("Add Account\xe2\x80\xa6")
                            action:@selector(_beginAddAccount)
                     keyEquivalent:@""];
            [menu addItemWithTitle:logoutTitle
                            action:@selector(_logoutActiveAccount)
                     keyEquivalent:@""];
            [menu addItem:[NSMenuItem separatorItem]];
            [menu addItemWithTitle:TkTr("Quit")
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
            if (!s)
            {
                return;
            }
            // ⌘+click pops the room out into its own window (and closes the
            // tab); a plain click just switches to it. Matches the room-list
            // new-tab gesture, which also uses Command on macOS.
            if ([NSEvent modifierFlags] & NSEventModifierFlagCommand)
            {
                s->_shell->tab_popout_room(room_id);
            }
            else
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
        // Provider wiring (avatar/image/sticker/preview/user-info).
        _shell->wire_main_app_widget_(_mainApp);

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
            // The recovery-key entry path now lives in the encryption-setup
            // overlay (Recover mode); the old inline RecoveryBanner was removed.
            s->_shell->show_encryption_setup(
                tesseract::views::EncryptionSetupOverlay::Mode::Recover);
        };

        // Image + video viewers — providers / repaint / on_close.
        // The shell-passed on_*_close callable restores compose focus when
        // the overlay closes (matches Qt6 behavior).
        _shell->wire_main_app_viewers_(
            _mainApp, _mainAppSurface->host(),
            [weakSelf]
            {
                MainWindowController* s = weakSelf;
                if (s && s->_mainAppSurface)
                {
                    s->_mainAppSurface->relayout();
                }
            },
            [weakSelf]
            {
                MainWindowController* s = weakSelf;
                if (!s)
                {
                    return;
                }
                if (!s->_mainApp->compose_text_area_rect().empty())
                {
                    s->_roomTextArea->set_focused(true);
                }
            },
            [weakSelf]
            {
                MainWindowController* s = weakSelf;
                if (!s)
                {
                    return;
                }
                if (!s->_mainApp->compose_text_area_rect().empty())
                {
                    s->_roomTextArea->set_focused(true);
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

        // RoomView shortcode lookup (avatar/image/preview wired via
        // wire_main_app_widget_).
        _mainApp->room_view()->set_shortcode_provider(
            [weakSelf](const std::string& mxc) -> std::string
            {
                MainWindowController* s = weakSelf;
                return s ? s->_shell->shortcode_for_mxc_(mxc) : std::string();
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
                // Non-blocking: warmed bytes or empty + async fetch (repaint on
                // arrival) so playback never freezes the UI thread.
                MacShell* shell = s->_shell.get();
                return shell->voice_bytes_or_fetch_(
                    source_json, [shell] { shell->request_relayout_(); });
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
            if (tesseract::is_slash_command_no_arg(body, "myroomavatar"))
            {
                s->_shell->pick_and_set_room_avatar_(s->_shell->current_room_id_);
                if (s->_roomTextArea) s->_roomTextArea->set_text("");
                if (s->_roomView)    s->_roomView->set_current_text({});
                return;
            }
            // Build from the composer's mention draft so inline pills become
            // matrix.to links + m.mentions; fall back to the plain body.
            std::vector<tesseract::MentionSeg> draft =
                s->_roomTextArea ? s->_roomTextArea->mention_draft()
                                 : std::vector<tesseract::MentionSeg>{};
            bool has_mention = false;
            for (const auto& seg : draft)
            {
                if (seg.kind == tesseract::MentionSeg::Kind::Mention)
                    has_mention = true;
            }
            tesseract::MarkdownResult msg =
                draft.empty() ? tesseract::MarkdownResult{body, ""}
                              : tesseract::build_mention_message(draft);
            std::string trimmed = trim(msg.body);
            if (trimmed.empty() && !has_mention)
            {
                return;
            }
            auto res = tesseract::dispatch_compose_send(
                *s->_shell->client_, s->_shell->current_room_id_,
                msg.body, msg.formatted_body);
            if (res)
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
            [weakSelf](const std::string& event_id, const std::string& key,
                       const std::string& source_mxc)
        {
            MainWindowController* s = weakSelf;
            if (!s || s->_shell->current_room_id_.empty())
            {
                return;
            }
            if (!source_mxc.empty())
            {
                // For MSC4027 chips matrix-sdk aggregates by the mxc:// key
                // (so `key` IS the mxc URI). Look up the shortcode locally
                // so the outgoing event carries `:shortcode:` rather than
                // re-broadcasting the URI as its own shortcode.
                std::string sc =
                    s->_shell->shortcode_for_mxc_(source_mxc);
                std::string shortcode =
                    sc.empty() ? std::string() : ":" + sc + ":";
                s->_shell->client_->send_reaction_custom(
                    s->_shell->current_room_id_, event_id, source_mxc,
                    shortcode);
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

        // Avatar click → open the lightbox with the original avatar mxc.
        // Overrides the thumbnail-only wiring from
        // ShellBase::wire_main_app_widget_ so ensure_media_image_ fetches
        // the native-resolution bytes into tk_images_; the viewer's
        // image_provider prefers that over the resized tk_avatars_ entry.
        _mainApp->room_view()->on_avatar_clicked =
            [weakSelf](std::string url, std::string name)
        {
            MainWindowController* s = weakSelf;
            if (!s || !s->_mainApp || !s->_mainAppSurface || url.empty())
            {
                return;
            }
            s->_imgViewer->open(url, url, name, 0, 0);
            s->_mainApp->show_image_viewer(true);
            s->_mainAppSurface->relayout();
            s->_shell->ensure_media_image_(url,
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
                                hit.natural_h, hit.loop, hit.no_audio,
                                hit.hide_controls);
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
                pop.behavior = NSPopoverBehaviorSemitransient;
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
            // TKSurfaceView has isFlipped=YES so widget coords map directly.
            NSRect anchorRect =
                NSMakeRect(anchor.x, anchor.y, anchor.w, anchor.h);
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
            std::string ev = event_id;
            s->_shell->begin_focused_subscription_(room, ev);
            dispatch_async(
                dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                    MainWindowController* s2 = weakSelf;
                    if (s2)
                    {
                        s2->_shell->client_->subscribe_room_at(room, ev);
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
        _mainApp->room_view()->on_threads_button_clicked = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (s)
            {
                s->_shell->on_threads_button_clicked();
            }
        };
        _mainApp->room_view()->on_pin_requested =
            [weakSelf](const std::string& ev)
        {
            MainWindowController* s = weakSelf;
            if (s)
            {
                s->_shell->on_pin_requested(ev);
            }
        };
        _mainApp->room_view()->on_unpin_requested =
            [weakSelf](const std::string& ev)
        {
            MainWindowController* s = weakSelf;
            if (s)
            {
                s->_shell->on_unpin_requested(ev);
            }
        };
        _mainApp->room_view()->on_thread_open_requested =
            [weakSelf](const std::string& root)
        {
            MainWindowController* s = weakSelf;
            if (s)
            {
                s->_shell->on_thread_open_requested(root);
            }
        };
        _mainApp->room_view()->on_thread_close_requested = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (s)
            {
                s->_shell->on_thread_close_requested();
            }
        };
        _mainApp->room_view()->on_thread_send =
            [weakSelf](const std::string& body, const std::string& formatted)
        {
            MainWindowController* s = weakSelf;
            if (s)
            {
                s->_shell->on_thread_send_requested(body, formatted);
                if (s->_roomTextArea)
                    s->_roomTextArea->set_text("");
                s->_roomView->set_current_text({});
            }
        };
        _mainApp->room_view()->on_thread_send_reply =
            [weakSelf](const std::string& reply_id,
                       const std::string& body,
                       const std::string& formatted)
        {
            MainWindowController* s = weakSelf;
            if (s)
            {
                s->_shell->on_thread_send_reply_requested(reply_id, body,
                                                          formatted);
                if (s->_roomTextArea)
                    s->_roomTextArea->set_text("");
                s->_roomView->set_current_text({});
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
        _mainApp->room_view()->on_focus_input = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (s && s->_roomTextArea)
                s->_roomTextArea->set_focused(true);
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
            s->_shell->run_async_mut_(
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
            s->_shell->run_async_mut_(
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
        _shell->setup_dm_callbacks();
        _mainApp->room_view()->on_ignore_user =
            [weakSelf](std::string user_id)
        {
            MainWindowController* s = weakSelf;
            if (!s || !s->_shell->client_)
                return;
            auto* c = s->_shell->client_;
            s->_shell->run_async_mut_(
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
        _roomTextArea->set_mention_colors(
            _mainAppSurface->theme().palette.accent,
            _mainAppSurface->theme().palette.text_on_accent);
        {
            // Eagerly build the mention popup panel + controller so the
            // controller has its borrowed popup widget from the start.
            NSRect mf = NSMakeRect(0, 0,
                                   tesseract::views::MentionPopup::kWidth,
                                   tesseract::views::MentionPopup::kRowHeight);
            _mentionPanel = [[NSPanel alloc]
                initWithContentRect:mf
                          styleMask:NSWindowStyleMaskNonactivatingPanel |
                                    NSWindowStyleMaskBorderless
                            backing:NSBackingStoreBuffered
                              defer:NO];
            _mentionPanel.floatingPanel = YES;
            _mentionPanel.hidesOnDeactivate = NO;
            _mentionPanel.becomesKeyOnlyIfNeeded = YES;
            _mentionPopupSurface =
                std::make_unique<tk::macos::Surface>(_mainAppSurface->theme());
            auto mw = std::make_unique<tesseract::views::MentionPopup>();
            _mentionPopupWidget = mw.get();
            _mentionPopupSurface->set_root(std::move(mw));
            [_mentionPanel setContentView:(__bridge NSView*)
                                              _mentionPopupSurface->view_handle()];

            __weak MainWindowController* mc = self;
            tesseract::views::MentionController::Hooks hooks;
            hooks.show = [mc](tk::Rect cursor, int rows)
            {
                if (MainWindowController* c = mc)
                    [c showMentionPopupAtCursor:cursor rows:rows];
            };
            hooks.hide = [mc]
            {
                if (MainWindowController* c = mc)
                    [c hideMentionPopup];
            };
            hooks.repaint = [mc]
            {
                if (MainWindowController* c = mc)
                    [c _relayoutMentionPopupIfVisible];
            };
            hooks.room_id = [mc]() -> std::string
            {
                MainWindowController* c = mc;
                return c ? c->_shell->current_room_id_ : std::string{};
            };
            hooks.run_async = [mc](std::function<void()> fn)
            {
                if (MainWindowController* c = mc)
                    c->_shell->run_async_(std::move(fn));
            };
            hooks.post_to_ui = [mc](std::function<void()> fn)
            {
                if (MainWindowController* c = mc)
                    c->_shell->post_to_ui_(std::move(fn));
            };
            // Live client getter: this controller is built before a session is
            // restored (_shell->client_ is null here), so a snapshot would stay
            // null. Reading it on each fetch also tracks account switches.
            hooks.client = [mc]() -> tesseract::Client*
            {
                MainWindowController* c = mc;
                return c ? c->_shell->client_ : nullptr;
            };
            hooks.fetch_avatar = [mc](const std::string& mxc)
            {
                if (MainWindowController* c = mc)
                    c->_shell->ensure_user_avatar_(mxc);
            };
            // Resolve candidate avatars from the shared avatar cache.
            _mentionPopupWidget->set_image_provider(
                [mc](const std::string& mxc) -> const tk::Image*
                {
                    MainWindowController* c = mc;
                    if (!c || !c->_shell)
                        return nullptr;
                    return c->_shell->thumbnail_cache_.peek(mxc);
                });
            _mentionController =
                std::make_unique<tesseract::views::MentionController>(
                    _roomTextArea.get(), _shell->client_, _mentionPopupWidget,
                    std::move(hooks));
        }
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

                // ── Slash-command detection ────────────────────────────────────
                int cursor = c->_roomTextArea->cursor_byte_pos();
                {
                    auto m = c->_slashEngine.find_prefix(s, cursor);
                    if (m.has_value())
                    {
                        auto items = c->_slashEngine.lookup(m->prefix);
                        if (items.empty())
                        {
                            [c hideSlashPopup];
                        }
                        else
                        {
                            // Guarded hides — don't wipe set_on_popup_nav unnecessarily.
                            if ([c shortcodePopupVisible])
                            {
                                [c hideShortcodePopup];
                            }
                            if ([c mentionPopupVisible])
                            {
                                [c hideMentionPopup];
                            }
                            [c showSlashPopupWithSuggestions:items
                                                 cursorRect:c->_roomTextArea
                                                                ->cursor_rect()];
                            // Install nav handler unconditionally on every matching tick.
                            c->_roomTextArea->set_on_popup_nav(
                                [weakSelf](
                                    tk::NativeTextArea::NavKey nk) -> bool
                                {
                                    MainWindowController* c2 = weakSelf;
                                    if (!c2 || ![c2 slashPopupVisible])
                                    {
                                        return false;
                                    }
                                    int cur =
                                        c2->_slashPopupWidget->selected_index();
                                    int n = c2->_slashPopupWidget->visible_rows();
                                    if (n <= 0)
                                    {
                                        return true;
                                    }
                                    switch (nk)
                                    {
                                    case tk::NativeTextArea::NavKey::Up:
                                        c2->_slashPopupWidget->set_selected_index(
                                            std::max(0, cur - 1));
                                        c2->_slashPopupSurface->host()
                                            .request_repaint();
                                        return true;
                                    case tk::NativeTextArea::NavKey::Down:
                                        c2->_slashPopupWidget->set_selected_index(
                                            std::min(n - 1, cur + 1));
                                        c2->_slashPopupSurface->host()
                                            .request_repaint();
                                        return true;
                                    case tk::NativeTextArea::NavKey::Tab:
                                    {
                                        int sel = std::max(
                                            0,
                                            c2->_slashPopupWidget
                                                ->selected_index());
                                        if (sel < n &&
                                            c2->_slashPopupWidget->on_accepted)
                                        {
                                            c2->_slashPopupWidget->on_accepted(
                                                c2->_slashPopupWidget
                                                    ->suggestion_at(sel));
                                        }
                                        return true;
                                    }
                                    case tk::NativeTextArea::NavKey::ShiftTab:
                                        return false;
                                    case tk::NativeTextArea::NavKey::Escape:
                                        [c2 hideSlashPopup];
                                        return true;
                                    }
                                    return false;
                                });
                        }
                        return; // skip shortcode/mention handling
                    }
                    if ([c slashPopupVisible])
                    {
                        [c hideSlashPopup];
                    }
                }
                // ── End slash-command detection ─────────────────────────────────

                // ── Shortcode detection ─────────────────────────────────────────

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
                    [c hideSlashPopup];
                    [c hideShortcodePopup];
                    [c hideMentionPopup];
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
                        [c hideMentionPopup];
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
                        [c showShortcodePopupWithSuggestions:
                                c->_shell->shortcode_current_suggestions_
                                                  cursorRect:
                                                      c->_roomTextArea
                                                          ->cursor_rect()];
                        // Reinstall the nav handler unconditionally on every
                        // tick: the hideMentionPopup call above wipes it via
                        // set_on_popup_nav(nullptr), so guarding the install
                        // behind "first show only" leaves nav dead after the
                        // second keystroke (Up/Down stop working).
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
                // ── @mention popup ──────────────────────────────────────────
                if (c->_mentionController &&
                    c->_mentionController->on_text_changed(s, cursor))
                {
                    return;
                }
                [c hideSlashPopup];
                [c hideShortcodePopup];
                [c hideMentionPopup];
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
                if (c->_mentionController && c->_mentionController->on_submit())
                {
                    return;
                }
                if ([c slashPopupVisible])
                {
                    int i = c->_slashPopupWidget->selected_index();
                    if (i >= 0 && i < c->_slashPopupWidget->visible_rows() &&
                        c->_slashPopupWidget->on_accepted)
                    {
                        c->_slashPopupWidget->on_accepted(
                            c->_slashPopupWidget->suggestion_at(i));
                    }
                    else
                    {
                        [c hideSlashPopup];
                    }
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
                s->_shell->debounce_(
                    MacShell::DebounceSlot::RoomSearch,
                    tesseract::views::RoomListView::kSearchDebounceMs,
                    [s] { [s _applySearchFilter]; });
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
                    if (s->_shell->enc_passphrase_field_)
                        s->_shell->enc_passphrase_field_->set_visible(false);
                    if (s->_shell->enc_key_field_)
                        s->_shell->enc_key_field_->set_visible(false);
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
                // Encryption setup passphrase field.
                if (s->_shell->enc_passphrase_field_ && app->encryption_setup())
                {
                    auto* ov = app->encryption_setup();
                    bool ppVisible = ov->passphrase_field_rect_visible();
                    s->_shell->enc_passphrase_field_->set_visible(ppVisible);
                    if (ppVisible)
                        s->_shell->enc_passphrase_field_->set_rect(
                            ov->passphrase_field_rect_value());
                }
                // Encryption setup recovery-key field.
                if (s->_shell->enc_key_field_ && app->encryption_setup())
                {
                    auto* ov = app->encryption_setup();
                    bool kfVisible = ov->key_field_rect_visible();
                    s->_shell->enc_key_field_->set_visible(kfVisible);
                    if (kfVisible)
                        s->_shell->enc_key_field_->set_rect(
                            ov->key_field_rect_value());
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
    // Route the homeserver-discovery debounce through MacShell's drain so a
    // blocked discover_homeserver call can't outlive ~LoginView and corrupt
    // the heap (mirrors the SettingsController wiring elsewhere).
    {
        __weak MainWindowController* ws = self;
        [_loginView setRunAsync:^(void (^body)(void)) {
            MainWindowController* s = ws;
            if (!s || !s->_shell)
            {
                return;
            }
            s->_shell->run_async_([body] { body(); });
        }];
    }

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
        _settingsView->on_reset_identity = [ws]
        {
            MainWindowController* s = ws;
            if (!s || !s->_shell)
            {
                return;
            }
            // The reset overlay lives on the main window — leave settings
            // first, then start the reset flow.
            NSView* mainAppView =
                (__bridge NSView*)s->_mainAppSurface->view_handle();
            mainAppView.hidden = NO;
            ((__bridge NSView*)s->_settingsSurface->view_handle()).hidden = YES;
            s->_shell->begin_crypto_identity_reset_();
        };
        _settingsView->on_logout = [ws]
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
            [s _logoutActiveAccount];
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
            if (!s || !s->_shell->settings_controller_)
            {
                return;
            }
            s->_shell->settings_controller_->set_notifications_enabled(enabled);
        };
        _settingsView->on_hide_content_changed = [ws](bool enabled)
        {
            MainWindowController* s = ws;
            if (!s)
            {
                return;
            }
            tesseract::Settings::instance().notification_hide_content = enabled;
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
        _settingsView->on_group_inactive_changed = [ws](bool enabled)
        {
            MainWindowController* s = ws;
            if (!s)
            {
                return;
            }
            tesseract::Settings::instance().group_inactive_rooms = enabled;
            tesseract::Settings::instance().save_to_disk(
                tesseract::config_dir());
            if (s->_roomListView) s->_roomListView->refresh();
        };
        _settingsView->on_inactive_period_changed = [ws](int days)
        {
            MainWindowController* s = ws;
            if (!s)
            {
                return;
            }
            tesseract::Settings::instance().inactive_room_threshold_days = days;
            tesseract::Settings::instance().save_to_disk(
                tesseract::config_dir());
            if (s->_roomListView) s->_roomListView->refresh();
        };
        _settingsView->on_send_presence_changed = [ws](bool enabled)
        {
            MainWindowController* s = ws;
            if (s) s->_shell->handle_send_presence_toggle_(enabled);
        };
        _settingsView->on_media_previews_changed =
            [ws](tesseract::Settings::MediaPreviews mode)
        {
            MainWindowController* s = ws;
            if (s)
                s->_shell->apply_media_preview_config_(
                    mode, tesseract::Settings::instance().invite_avatars);
        };
        _settingsView->on_invite_avatars_changed = [ws](bool enabled)
        {
            MainWindowController* s = ws;
            if (s)
                s->_shell->apply_media_preview_config_(
                    tesseract::Settings::instance().media_previews, enabled);
        };
        _settingsView->on_tab_changed = [ws] {
            MainWindowController* s = ws;
            if (s) s->_settingsSurface->relayout();
        };
        _settingsView->on_clear_caches = [ws]
        {
            MainWindowController* s = ws;
            if (!s || !s->_shell) return;
            s->_shell->clear_all_caches_(
                [ws](uint64_t local, uint64_t sdk, uint64_t memory,
                     uint64_t mh, uint64_t mm, uint64_t dh, uint64_t dm)
            {
                MainWindowController* s2 = ws;
                if (s2 && s2->_settingsView)
                    s2->_settingsView->set_cache_sizes(local, sdk, memory,
                                                       mh, mm, dh, dm);
            });
        };
        _settingsView->on_show_tooltip =
            [ws](std::string text, tk::Rect anchor)
        {
            MainWindowController* s = ws;
            if (!s || !s->_settingsSurface)
                return;
            if (!s->_cacheTooltipPopover)
            {
                NSTextField* lbl = [NSTextField wrappingLabelWithString:@""];
                lbl.translatesAutoresizingMaskIntoConstraints = NO;
                NSView* cv = [[NSView alloc] init];
                cv.translatesAutoresizingMaskIntoConstraints = NO;
                [cv addSubview:lbl];
                [NSLayoutConstraint activateConstraints:@[
                    [lbl.leadingAnchor constraintEqualToAnchor:cv.leadingAnchor
                                                      constant:8],
                    [lbl.trailingAnchor constraintEqualToAnchor:cv.trailingAnchor
                                                       constant:-8],
                    [lbl.topAnchor constraintEqualToAnchor:cv.topAnchor
                                                  constant:6],
                    [lbl.bottomAnchor constraintEqualToAnchor:cv.bottomAnchor
                                                     constant:-6],
                    [cv.widthAnchor constraintLessThanOrEqualToConstant:480],
                ]];
                NSViewController* vc = [[NSViewController alloc] init];
                vc.view = cv;
                NSPopover* pop = [[NSPopover alloc] init];
                pop.contentViewController = vc;
                pop.behavior = NSPopoverBehaviorSemitransient;
                pop.animates = NO;
                s->_cacheTooltipPopover = pop;
            }
            NSTextField* lbl =
                (NSTextField*)s->_cacheTooltipPopover.contentViewController
                    .view.subviews.firstObject;
            lbl.stringValue = [NSString stringWithUTF8String:text.c_str()];
            [s->_cacheTooltipPopover.contentViewController.view
                layoutSubtreeIfNeeded];
            s->_cacheTooltipPopover.contentSize =
                [s->_cacheTooltipPopover.contentViewController.view fittingSize];
            NSView* view =
                (__bridge NSView*)s->_settingsSurface->view_handle();
            NSRect anchorRect =
                NSMakeRect(anchor.x, anchor.y, anchor.w, anchor.h);
            [s->_cacheTooltipPopover showRelativeToRect:anchorRect
                                                 ofView:view
                                          preferredEdge:NSRectEdgeMinY];
        };
        _settingsView->on_hide_tooltip = [ws]
        {
            MainWindowController* s = ws;
            if (s && s->_cacheTooltipPopover && s->_cacheTooltipPopover.shown)
                [s->_cacheTooltipPopover close];
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

    // Status bar is added last so it is always on top of other views and
    // cannot be covered by a full-height sibling.
    [self _buildStatusBar:content];

    [NSLayoutConstraint activateConstraints:@[
        // All content views stop above the status bar.
        [mainAppView.topAnchor constraintEqualToAnchor:content.topAnchor],
        [mainAppView.leadingAnchor
            constraintEqualToAnchor:content.leadingAnchor],
        [mainAppView.trailingAnchor
            constraintEqualToAnchor:content.trailingAnchor],
        [mainAppView.bottomAnchor
            constraintEqualToAnchor:_statusBarView.topAnchor],
        [_loginView.topAnchor constraintEqualToAnchor:content.topAnchor],
        [_loginView.leadingAnchor
            constraintEqualToAnchor:content.leadingAnchor],
        [_loginView.trailingAnchor
            constraintEqualToAnchor:content.trailingAnchor],
        [_loginView.bottomAnchor
            constraintEqualToAnchor:_statusBarView.topAnchor],
        [settingsView.topAnchor constraintEqualToAnchor:content.topAnchor],
        [settingsView.leadingAnchor
            constraintEqualToAnchor:content.leadingAnchor],
        [settingsView.trailingAnchor
            constraintEqualToAnchor:content.trailingAnchor],
        [settingsView.bottomAnchor
            constraintEqualToAnchor:_statusBarView.topAnchor],
        [brandingView.topAnchor constraintEqualToAnchor:content.topAnchor],
        [brandingView.leadingAnchor
            constraintEqualToAnchor:content.leadingAnchor],
        [brandingView.trailingAnchor
            constraintEqualToAnchor:content.trailingAnchor],
        [brandingView.bottomAnchor
            constraintEqualToAnchor:_statusBarView.topAnchor],
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
    if (_presenceTickTimer)
    {
        [_presenceTickTimer invalidate];
        _presenceTickTimer = nil;
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
    if (_slashPopupSurface)
    {
        _slashPopupSurface->set_theme(t);
    }
    if (_mentionPopupSurface)
    {
        _mentionPopupSurface->set_theme(t);
    }
    if (_roomTextArea)
    {
        _roomTextArea->set_mention_colors(t.palette.accent,
                                          t.palette.text_on_accent);
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
    // Signal Rust's cancellation channel first so any worker thread
    // currently blocked inside a `block_on(tokio::select! { stop_rx })`
    // FFI call returns immediately.  drain() can then join all threads
    // without blocking.  The invariant "no worker is calling client_->*
    // when the client is destroyed" is still satisfied because drain()
    // runs before the client destructor.
    for (auto& acc : _shell->accounts_)
    {
        if (acc->sync_started)
            acc->client->stop_sync();
    }
    _shell->pool_.drain();
    _shell->mut_pool_.drain();
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
                                 [s _startAnimTickIfNeeded];
                                 return f;
                             }
                             {
                                 if (const auto* img =
                                         s->_shell->image_cache_.peek(cache_key))
                                     return img;
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
    panel.onEmoticonSelect = ^(const tesseract::ImagePackImage& img) {
        MainWindowController* s = weakSelf;
        if (!s || img.url.empty())
        {
            return;
        }
        // Reaction mode (parallel to onSelect above): send an MSC4027
        // custom-image reaction.
        if (!s->_pendingReactionEventId.empty())
        {
            std::string ev = std::move(s->_pendingReactionEventId);
            s->_pendingReactionEventId.clear();
            if (!s->_shell->current_room_id_.empty())
            {
                s->_shell->client_->send_reaction_custom(
                    s->_shell->current_room_id_, ev, img.url,
                    ":" + img.shortcode + ":");
            }
            [weakPanel close];
            return;
        }
        // Compose mode: insert `:shortcode:` text.
        if (!s->_roomTextArea)
        {
            return;
        }
        s->_roomTextArea->insert_at_cursor(":" + img.shortcode + ":");
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

- (void)_relayoutAccountPickerIfVisible
{
    if (_accountPickerPopover && _accountPickerPopover.isShown &&
        _accountPickerSurface)
    {
        _accountPickerSurface->relayout();
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
                return c->_shell->image_cache_.peek(url);
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
// Slash-command suggestion popup
// ---------------------------------------------------------------------------

- (BOOL)slashPopupVisible
{
    return _slashPanel && _slashPanel.isVisible;
}

- (void)_relayoutSlashPopupIfVisible
{
    if ([self slashPopupVisible] && _slashPopupSurface)
    {
        _slashPopupSurface->relayout();
    }
}

- (void)showSlashPopupWithSuggestions:
            (const std::vector<tesseract::views::SlashCommandSuggestion>&)
                suggestions
                           cursorRect:(tk::Rect)cursor
{
    int rows = std::min((int)suggestions.size(),
                        int(tesseract::views::SlashCommandPopup::kMaxRows));
    NSSize size =
        NSMakeSize(tesseract::views::SlashCommandPopup::kWidth,
                   rows * tesseract::views::SlashCommandPopup::kRowHeight);

    if (!_slashPanel)
    {
        NSRect frame = NSMakeRect(0, 0, size.width, size.height);
        _slashPanel = [[NSPanel alloc]
            initWithContentRect:frame
                      styleMask:NSWindowStyleMaskNonactivatingPanel |
                                NSWindowStyleMaskBorderless
                        backing:NSBackingStoreBuffered
                          defer:NO];
        _slashPanel.floatingPanel = YES;
        _slashPanel.hidesOnDeactivate = NO;
        _slashPanel.becomesKeyOnlyIfNeeded = YES;

        _slashPopupSurface =
            std::make_unique<tk::macos::Surface>(_mainAppSurface->theme());
        auto pw = std::make_unique<tesseract::views::SlashCommandPopup>();
        _slashPopupWidget = pw.get();
        _slashPopupSurface->set_root(std::move(pw));

        __weak MainWindowController* weakSelf = self;
        _slashPopupWidget->on_accepted =
            [weakSelf](tesseract::views::SlashCommandSuggestion s)
        {
            MainWindowController* c = weakSelf;
            if (!c)
            {
                return;
            }
            [c hideSlashPopup];
            if (!c->_roomTextArea)
            {
                return;
            }
            if (!c->_shell->client_ || c->_shell->current_room_id_.empty())
            {
                return;
            }
            if (s.args_hint.empty())
            {
                std::string body = "/" + s.name;
                (void)tesseract::dispatch_compose_send(
                    *c->_shell->client_, c->_shell->current_room_id_,
                    body, std::string{});
                c->_roomTextArea->set_text("");
                if (c->_roomView)
                {
                    c->_roomView->set_current_text({});
                }
            }
            else
            {
                // Use replace_range (not set_text) so the caret lands after
                // the trailing space and shared composer state stays in sync.
                std::string body = "/" + s.name + " ";
                c->_roomTextArea->replace_range(
                    0, static_cast<int>(c->_roomTextArea->text().size()), body);
            }
        };
        _slashPopupWidget->on_dismissed = [weakSelf]
        {
            if (MainWindowController* c = weakSelf)
            {
                [c hideSlashPopup];
            }
        };

        NSView* popupView =
            (__bridge NSView*)_slashPopupSurface->view_handle();
        [_slashPanel setContentView:popupView];
    }

    _slashPopupWidget->set_suggestions(suggestions);
    _slashPopupWidget->set_selected_index(0);
    [_slashPanel setContentSize:size];
    _slashPopupSurface->relayout();

    // Map cursor_local (TKSurfaceView y-down) → screen coords (y-up).
    NSView* hostView = (__bridge NSView*)_mainAppSurface->view_handle();
    NSPoint localPt = NSMakePoint(cursor.x, cursor.y);
    NSPoint windowPt = [hostView convertPoint:localPt toView:nil];
    NSPoint screenPt = [hostView.window convertPointToScreen:windowPt];

    NSRect screenFrame = _slashPanel.screen
                             ? _slashPanel.screen.visibleFrame
                             : [NSScreen mainScreen].visibleFrame;

    // Prefer above the cursor; fall back to below when the panel would be
    // clipped at the top of the screen (mirrors shortcode popup behaviour).
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

    [_slashPanel setFrameOrigin:NSMakePoint(x, y)];
    [_slashPanel orderFront:nil];
}

- (void)hideSlashPopup
{
    [_slashPanel orderOut:nil];
    if (_roomTextArea)
    {
        _roomTextArea->set_on_popup_nav(nullptr);
    }
}

// ---------------------------------------------------------------------------

- (BOOL)mentionPopupVisible
{
    return _mentionPanel && _mentionPanel.isVisible;
}

- (void)_relayoutMentionPopupIfVisible
{
    if ([self mentionPopupVisible] && _mentionPopupSurface)
    {
        _mentionPopupSurface->relayout();
    }
}

- (void)hideMentionPopup
{
    [_mentionPanel orderOut:nil];
    if (_roomTextArea)
    {
        _roomTextArea->set_on_popup_nav(nullptr);
    }
}

- (void)showMentionPopupAtCursor:(tk::Rect)cursor rows:(int)rows
{
    if (!_mentionPanel || !_mainAppSurface)
    {
        return;
    }
    NSSize size = NSMakeSize(tesseract::views::MentionPopup::kWidth,
                             rows * tesseract::views::MentionPopup::kRowHeight);
    [_mentionPanel setContentSize:size];
    if (_mentionPopupSurface)
    {
        _mentionPopupSurface->relayout();
    }

    NSView* hostView = (__bridge NSView*)_mainAppSurface->view_handle();
    NSPoint localPt = NSMakePoint(cursor.x, cursor.y);
    NSPoint windowPt = [hostView convertPoint:localPt toView:nil];
    NSPoint screenPt = [hostView.window convertPointToScreen:windowPt];

    NSRect screenFrame = _mentionPanel.screen
                             ? _mentionPanel.screen.visibleFrame
                             : [NSScreen mainScreen].visibleFrame;
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
                   screenFrame.origin.y + screenFrame.size.height - size.height);
    [_mentionPanel setFrameOrigin:NSMakePoint(x, y)];
    [_mentionPanel orderFront:nil];

    // Route keyboard nav to the controller while the popup is up (mirrors the
    // shortcode popup; mutually exclusive so re-installing on each show is ok).
    __weak MainWindowController* weakSelf = self;
    _roomTextArea->set_on_popup_nav(
        [weakSelf](tk::NativeTextArea::NavKey nk) -> bool
        {
            MainWindowController* c = weakSelf;
            return c && c->_mentionController &&
                   c->_mentionController->on_nav(nk);
        });
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
                                 [s _startAnimTickIfNeeded];
                                 return f;
                             }
                             {
                                 if (const auto* img =
                                         s->_shell->image_cache_.peek(cache_key))
                                     return img;
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
    panel.onEmoticonSelect = ^(const tesseract::ImagePackImage& img) {
        MainWindowController* s = weakSelf;
        if (!s || img.url.empty())
        {
            return;
        }
        if (!s->_pendingReactionEventId.empty())
        {
            std::string ev = std::move(s->_pendingReactionEventId);
            s->_pendingReactionEventId.clear();
            if (!s->_shell->current_room_id_.empty())
            {
                s->_shell->client_->send_reaction_custom(
                    s->_shell->current_room_id_, ev, img.url,
                    ":" + img.shortcode + ":");
            }
            [weakPanel close];
            return;
        }
        if (!s->_roomTextArea)
        {
            return;
        }
        s->_roomTextArea->insert_at_cursor(":" + img.shortcode + ":");
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
        {
            auto prefs = tesseract::Prefs::parse(session->client->load_prefs_json());
            session->last_room  = prefs.last_room;
            session->open_rooms = prefs.open_rooms;
        }
        session->sync_started = true;
        session->client->start_sync(session->bridge.get());

        _shell->accounts_.push_back(std::move(session));
    }

    if (_shell->accounts_.empty())
    {
        _shell->pending_login_temp_dir_.clear();
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
            [ws](auto fn) {
                MainWindowController* s = ws;
                if (s)
                    s->_shell->run_async_(std::move(fn));
            },
            [ws](auto cb) {
                MainWindowController* s = ws;
                if (s)
                    s->_shell->pick_image_file_(std::move(cb));
            });

    // Key export/import dialog hooks.
    _shell->settings_controller_->show_passphrase_prompt =
        [ws](std::string title, std::function<void(std::string)> cb)
    {
        MainWindowController* s = ws;
        if (!s) return;
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = [NSString stringWithUTF8String:title.c_str()];
        alert.alertStyle = NSAlertStyleInformational;
        NSSecureTextField* field = [[NSSecureTextField alloc]
            initWithFrame:NSMakeRect(0, 0, 260, 24)];
        field.placeholderString = @"Passphrase";
        alert.accessoryView = field;
        [alert addButtonWithTitle:@"OK"];
        [alert addButtonWithTitle:@"Cancel"];
        [alert beginSheetModalForWindow:s.window
                      completionHandler:^(NSModalResponse resp) {
            if (resp == NSAlertFirstButtonReturn) {
                std::string pass = field.stringValue.UTF8String ?: "";
                if (!pass.empty()) cb(std::move(pass));
            }
        }];
    };

    _shell->settings_controller_->show_save_file_dialog =
        [ws](std::string suggested_name, std::function<void(std::string)> cb)
    {
        MainWindowController* s = ws;
        if (!s) return;
        NSSavePanel* panel = [NSSavePanel savePanel];
        panel.nameFieldStringValue =
            [NSString stringWithUTF8String:suggested_name.c_str()];
        [panel beginSheetModalForWindow:s.window
                      completionHandler:^(NSModalResponse result) {
            if (result == NSModalResponseOK && panel.URL)
                cb(panel.URL.path.UTF8String ?: "");
        }];
    };

    _shell->settings_controller_->show_open_file_dialog =
        [ws](std::function<void(std::string)> cb)
    {
        MainWindowController* s = ws;
        if (!s) return;
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.allowsMultipleSelection = NO;
        [panel beginSheetModalForWindow:s.window
                      completionHandler:^(NSModalResponse result) {
            if (result == NSModalResponseOK && panel.URL)
                cb(panel.URL.path.UTF8String ?: "");
        }];
    };

    _shell->settings_controller_->on_export_keys_result =
        [ws](bool ok, std::string error)
    {
        MainWindowController* s = ws;
        if (!s) return;
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = ok ? @"Export complete" : @"Export failed";
        alert.informativeText = ok
            ? @"Room keys exported successfully."
            : [NSString stringWithUTF8String:error.c_str()];
        alert.alertStyle = ok ? NSAlertStyleInformational : NSAlertStyleWarning;
        [alert beginSheetModalForWindow:s.window completionHandler:nil];
    };

    _shell->settings_controller_->on_import_keys_result =
        [ws](bool ok, std::string error)
    {
        MainWindowController* s = ws;
        if (!s) return;
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = ok ? @"Import complete" : @"Import failed";
        alert.informativeText = ok
            ? @"Room keys imported successfully."
            : [NSString stringWithUTF8String:error.c_str()];
        alert.alertStyle = ok ? NSAlertStyleInformational : NSAlertStyleWarning;
        [alert beginSheetModalForWindow:s.window completionHandler:nil];
    };

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
        _settingsView->set_request_repaint([ws]
        {
            MainWindowController* s = ws;
            if (s && s->_settingsSurface)
                s->_settingsSurface->relayout();
        });
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
        // Override the shared SettingsView's on_avatar_changed (which only
        // updates the in-settings AccountSection chip) so the sidebar
        // UserInfo strip also refreshes after a self-avatar change.
        _shell->settings_controller_->on_avatar_changed =
            [ws](std::string mxc)
        {
            MainWindowController* s = ws;
            if (!s) return;
            s->_shell->my_avatar_url_ = mxc;
            if (s->_shell->active_account_index_ >= 0 &&
                s->_shell->active_account_index_ <
                    static_cast<int>(s->_shell->accounts_.size()))
            {
                s->_shell->accounts_[s->_shell->active_account_index_]
                    ->avatar_url = s->_shell->my_avatar_url_;
            }
            s->_settingsView->set_avatar_url(mxc);
            if (s->_settingsSurface) s->_settingsSurface->relayout();
            [s _populateUserStrip];
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
            [_loginView setClient:nullptr];
            _shell->pending_login_client_.reset();
            std::error_code ec;
            std::filesystem::remove_all(_shell->pending_login_temp_dir_, ec);
            _shell->pending_login_temp_dir_.clear();
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
    [_loginView setClient:nullptr];
    _shell->pending_login_client_.reset(); // close SQLite handles before reopen
    _shell->pending_login_temp_dir_.clear();

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
    {
        auto prefs = tesseract::Prefs::parse(session->client->load_prefs_json());
        session->last_room  = prefs.last_room;
        session->open_rooms = prefs.open_rooms;
    }
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
    [_loginView setClient:nullptr];
    _shell->pending_login_client_.reset();
    if (_shell->pending_login_temp_dir_ != std::filesystem::path())
    {
        std::error_code ec;
        std::filesystem::remove_all(_shell->pending_login_temp_dir_, ec);
        _shell->pending_login_temp_dir_.clear();
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
    const int old_idx = _shell->active_account_index_;
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
    _shell->pending_restore_rooms_ = session->open_rooms.empty()
        ? (session->last_room.empty() ? std::vector<std::string>{}
                                      : std::vector<std::string>{session->last_room})
        : session->open_rooms;
    if (!session->last_room.empty() && !_shell->pending_restore_rooms_.empty() &&
        _shell->pending_restore_rooms_[0] != session->last_room)
    {
        auto it = std::find(_shell->pending_restore_rooms_.begin(),
                            _shell->pending_restore_rooms_.end(),
                            session->last_room);
        if (it != _shell->pending_restore_rooms_.end())
            std::rotate(_shell->pending_restore_rooms_.begin(), it, it + 1);
    }

    auto idxData = tesseract::SessionStore::load_index();
    idxData.active_user_id = _shell->my_user_id_;
    tesseract::SessionStore::save_index(idxData);

    _shell->current_room_id_.clear();
    _shell->tabs_.clear();
    _shell->active_tab_idx_ = 0;
    _shell->space_stack_.clear();

    auto it = _shell->per_account_rooms_.find(_shell->my_user_id_);
    _shell->rooms_ = (it != _shell->per_account_rooms_.end())
                         ? it->second
                         : std::vector<tesseract::RoomInfo>{};
    [self _refreshRoomList];

    // Restore the invite snapshot for the incoming account (parallel to rooms_).
    auto inv_it = _shell->per_account_invites_.find(_shell->my_user_id_);
    _shell->invites_ = (inv_it != _shell->per_account_invites_.end())
                           ? inv_it->second
                           : std::vector<tesseract::InviteInfo>{};
    _shell->on_invites_updated_();

    // Dismiss any stale InviteCard from the previous account.
    _shell->current_invite_.reset();
    if (_shell->main_app_)
        _shell->main_app_->show_room();
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

    // Save banner state for the outgoing account, then load for the incoming.
    if (old_idx >= 0 && old_idx < static_cast<int>(_shell->accounts_.size()))
    {
        _shell->accounts_[old_idx]->verification_banner_dismissed =
            _shell->verification_banner_dismissed_;
    }
    if (_mainApp)
    {
        _mainApp->show_verif_banner(false);
        _mainAppSurface->relayout();
    }
    _shell->verification_banner_dismissed_ = session->verification_banner_dismissed;

    _shell->handle_verification_state_ui_(!session->unverified);

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
                    NSWindow* win = strong.window;
                    if (win.isVisible && !win.isMiniaturized && [NSApp isActive])
                    {
                        [win orderOut:nil];
                    }
                    else
                    {
                        [win makeKeyAndOrderFront:nil];
                        [NSApp activateIgnoringOtherApps:YES];
                        strong->_shell->navigate_tray_unread_();
                    }
                });
            },
            []
            {
                dispatch_async(dispatch_get_main_queue(), ^{
                    [NSApp terminate:nil];
                });
            });
        if (_tray && _tray->is_available())
        {
            // Seed the new tray with the current aggregate so an already-
            // unread state shows immediately rather than waiting for the next
            // sync tick to flip on_tray_unread_changed_.
            _tray->set_unread(_shell->last_tray_unread_,
                              _shell->last_tray_highlight_);
        }
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
    _shell->pending_login_temp_dir_.clear();
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
            return s->_shell->thumbnail_cache_.peek(mxc);
        });
    _settingsView->set_theme_pref(tesseract::Settings::instance().theme_pref);
    _settingsView->set_notifications_enabled(
        tesseract::Settings::instance().notifications_enabled);
    _settingsView->set_hide_content_enabled(
        tesseract::Settings::instance().notification_hide_content);
    _settingsView->set_image_previews_enabled(
        tesseract::Settings::instance().notification_image_previews);
    _settingsView->set_prefetch_enabled(
        tesseract::Settings::instance().prefetch_full_media);
    _settingsView->set_group_inactive_pref(
        tesseract::Settings::instance().group_inactive_rooms);
    _settingsView->set_inactive_period_pref(
        tesseract::Settings::instance().inactive_room_threshold_days);
    _settingsView->set_send_presence_pref(
        tesseract::Settings::instance().send_presence);
    _settingsView->set_media_previews_pref(
        tesseract::Settings::instance().media_previews);
    _settingsView->set_invite_avatars_pref(
        tesseract::Settings::instance().invite_avatars);
    _settingsSurface->relayout();

    _shell->compute_cache_sizes_(
        [ws](uint64_t local, uint64_t sdk, uint64_t memory,
             uint64_t mh, uint64_t mm, uint64_t dh, uint64_t dm)
    {
        MainWindowController* s = ws;
        if (s && s->_settingsView)
            s->_settingsView->set_cache_sizes(local, sdk, memory,
                                              mh, mm, dh, dm);
    });

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
                return s->_shell->thumbnail_cache_.peek(mxc);
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
    // TKSurfaceView has isFlipped=YES so y=0 is the top; the strip sits at viewH-stripH.
    NSView* mainAppView = (__bridge NSView*)_mainAppSurface->view_handle();
    CGFloat stripH = static_cast<CGFloat>(tesseract::visual::kUserStripHeight);
    CGFloat viewH  = mainAppView.bounds.size.height;
    NSRect stripRect = NSMakeRect(
        0, viewH - stripH,
        static_cast<CGFloat>(tesseract::visual::kSidebarWidth), stripH);
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
    _shell->notify_presence_logout_();
    session->client->logout();
    session->client->stop_sync();
    session->sync_started = false;

    tesseract::SessionStore::clear_account(uid);
    _shell->per_account_rooms_.erase(uid);
    _shell->per_account_invites_.erase(uid);
    // Recompute the tray aggregate so the dot clears (or rolls over to the
    // surviving accounts) immediately; without this the indicator can stick
    // when the only account with unreads was the one we just signed out.
    _shell->notify_tray_unread_();
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
        _shell->invites_.clear();
        _shell->current_invite_.reset();
        _shell->space_stack_.clear();
        [self _refreshRoomList];
        [self _refreshInviteList];
        _shell->handle_compose_room_leaving_(_shell->current_room_id_);
        _shell->current_room_id_.clear();
        if (_roomView)
        {
            _roomView->clear_room();
            _roomView->set_messages({});
        }
        if (_shell->main_app_)
            _shell->main_app_->clear_content();
        [self _relayoutChatSurface];

        _shell->pending_login_temp_dir_.clear();
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
    if (_shell)
    {
        _shell->notify_window_active_(true);
    }
}

- (void)windowDidResignKey:(NSNotification*)notification
{
    (void)notification;
    if (_shell)
    {
        _shell->notify_window_active_(false);
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

    // Bounce dock if window is visible but not focused.
    if (winVisible && !winFocused)
    {
        [self _requestUserAttention];
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
        [_loginView setStatusMessage:TkTr("Session expired; please log in again.")];
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
    // The old inline RecoveryBanner was removed; recovery now lives in the
    // encryption-setup overlay (Recover mode). Backup progress no longer drives
    // any banner state here.
    (void)progress;
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

- (void)_buildStatusBar:(NSView*)content
{
    _statusBarView = [[NSView alloc] init];
    _statusBarView.translatesAutoresizingMaskIntoConstraints = NO;
    // No explicit wantsLayer / layer.backgroundColor here — content already
    // has wantsLayer = YES; setting it again with an explicit backgroundColor
    // was preventing the NSTextField subviews from rendering.

    NSBox* sep = [[NSBox alloc] init];
    sep.translatesAutoresizingMaskIntoConstraints = NO;
    sep.boxType = NSBoxSeparator;
    [_statusBarView addSubview:sep];

    _statusLabel = [[NSTextField alloc] initWithFrame:NSZeroRect];
    _statusLabel.editable = NO;
    _statusLabel.selectable = NO;
    _statusLabel.bordered = NO;
    _statusLabel.bezeled = NO;
    _statusLabel.drawsBackground = NO;
    _statusLabel.stringValue = @"Not logged in";
    _statusLabel.font = [NSFont systemFontOfSize:11];
    _statusLabel.textColor = NSColor.secondaryLabelColor;
    _statusLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    _statusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    [_statusLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                         forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_statusBarView addSubview:_statusLabel];

    _inflightDotLabel = [[NSTextField alloc] initWithFrame:NSZeroRect];
    _inflightDotLabel.editable = NO;
    _inflightDotLabel.selectable = NO;
    _inflightDotLabel.bordered = NO;
    _inflightDotLabel.bezeled = NO;
    _inflightDotLabel.drawsBackground = NO;
    _inflightDotLabel.stringValue = @"●";
    _inflightDotLabel.font = [NSFont systemFontOfSize:10];
    _inflightDotLabel.textColor = NSColor.tertiaryLabelColor;
    _inflightDotLabel.translatesAutoresizingMaskIntoConstraints = NO;
    [_inflightDotLabel setContentHuggingPriority:NSLayoutPriorityRequired
                                forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_inflightDotLabel setContentCompressionResistancePriority:NSLayoutPriorityRequired
                                              forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_statusBarView addSubview:_inflightDotLabel];

    [content addSubview:_statusBarView];

    [NSLayoutConstraint activateConstraints:@[
        [_statusBarView.leadingAnchor
            constraintEqualToAnchor:content.leadingAnchor],
        [_statusBarView.trailingAnchor
            constraintEqualToAnchor:content.trailingAnchor],
        [_statusBarView.bottomAnchor
            constraintEqualToAnchor:content.bottomAnchor],
        [_statusBarView.heightAnchor constraintEqualToConstant:22],

        [sep.topAnchor constraintEqualToAnchor:_statusBarView.topAnchor],
        [sep.leadingAnchor constraintEqualToAnchor:_statusBarView.leadingAnchor],
        [sep.trailingAnchor
            constraintEqualToAnchor:_statusBarView.trailingAnchor],
        [sep.heightAnchor constraintEqualToConstant:1],

        [_inflightDotLabel.trailingAnchor
            constraintEqualToAnchor:_statusBarView.trailingAnchor constant:-8],
        [_inflightDotLabel.centerYAnchor
            constraintEqualToAnchor:_statusBarView.centerYAnchor],

        [_statusLabel.leadingAnchor
            constraintEqualToAnchor:_statusBarView.leadingAnchor constant:8],
        [_statusLabel.centerYAnchor
            constraintEqualToAnchor:_statusBarView.centerYAnchor],
        [_statusLabel.trailingAnchor
            constraintEqualToAnchor:_inflightDotLabel.leadingAnchor
                           constant:-8],
    ]];

    if (_shell)
        _shell->init_pool_callbacks_();
    [self _onInflightChanged];
}

- (void)_refreshSyncStatus
{
    if (!_statusLabel || !_shell)
        return;
    using RLS = tesseract::RoomListState;
    using BS = tesseract::BackupState;

    const bool room_busy = (_shell->last_room_list_state_ == RLS::Init ||
                             _shell->last_room_list_state_ == RLS::SettingUp);
    const bool reconnecting =
        (_shell->last_room_list_state_ == RLS::Recovering);
    const bool keys_busy = (_shell->last_backup_state_ == BS::Downloading);

    NSString* text;
    if (room_busy)
        text = @"Syncing rooms…";
    else if (reconnecting)
        text = @"Reconnecting…";
    else if (keys_busy)
        text = [NSString
            stringWithFormat:@"Downloading encryption keys (%llu)…",
                             (unsigned long long)_shell->last_imported_keys_];
    else
        text = @"Connected";

    _statusLabel.stringValue = text;
    _shell->sync_progress_shown_ = room_busy || reconnecting || keys_busy;
}

- (void)_onInflightChanged
{
    if (!_inflightDotLabel || !_shell)
        return;
    const auto   c  = _shell->inflight_dot_color_();
    const uint32_t n  = _shell->inflight_total_();
    const size_t fp = _shell->pool_pending_count_();
    const size_t sp = _shell->mut_pool_pending_count_();
    const size_t mp = _shell->pending_media_count_();
    _inflightDotLabel.textColor =
        [NSColor colorWithRed:c.r / 255.0
                        green:c.g / 255.0
                         blue:c.b / 255.0
                        alpha:1.0];
    NSString* first = (n == 1)
                          ? @"1 request in flight"
                          : [NSString stringWithFormat:@"%u requests in flight", n];
    _inflightDotLabel.toolTip =
        [NSString stringWithFormat:
                      @"%@\nmedia: %zu loading · fetch: %zu queued · send: %zu queued",
                      first, mp, fp, sp];
}

- (void)_onRoomListStateChanged
{
    [self _refreshSyncStatus];
    [self _onInflightChanged];

    // Once Running, attempt the deferred room restore (we waited for Running
    // to avoid subscribing to a room during initial sync, which triggers the
    // imbl promote_front data race in matrix-sdk-ui).
    using RLS = tesseract::RoomListState;
    if (_shell->last_room_list_state_ == RLS::Running &&
        _shell->current_room_id_.empty() &&
        !_shell->pending_restore_rooms_.empty())
    {
        if (_shell->try_restore_tab_session_(_shell->pending_restore_rooms_,
                                              _shell->pending_restore_rooms_[0]))
            _shell->pending_restore_rooms_.clear();
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

- (void)_updateTrayUnread:(bool)hasUnread highlight:(bool)hasHighlight
{
    if (_tray)
    {
        _tray->set_unread(hasUnread, hasHighlight);
    }
}

- (void)_refreshRoomList
{
    std::vector<tesseract::RoomInfo> filtered;
    if (_shell->space_stack_.empty())
    {
        std::unordered_set<std::string> in_space;
        for (const auto& r : _shell->rooms_)
        {
            if (!r.is_space)
            {
                continue;
            }
            auto sc_it = _shell->space_children_cache_.find(r.id);
            if (sc_it != _shell->space_children_cache_.end())
            {
                for (const auto& id : sc_it->second)
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
        _shell->apply_space_child_counts_(filtered);
        if (_mainApp)
        {
            _mainApp->set_space_nav(false);
        }
    }
    else
    {
        static const std::vector<std::string> kNoChildren;
        const auto sc_it =
            _shell->space_children_cache_.find(_shell->space_stack_.back());
        const auto& child_ids =
            sc_it != _shell->space_children_cache_.end() ? sc_it->second
                                                         : kNoChildren;
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
    // Avatars load lazily as rows are painted (RoomListView's
    // on_room_avatar_needed), so collapsed / off-screen rooms aren't requested.
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

- (void)_refreshInviteList
{
    if (_roomListView)
    {
        _roomListView->set_invites(&_shell->invites_);
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
    [self hideSlashPopup];
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
        prefs.open_rooms.clear();
        for (const auto& t : _shell->tabs_)
            prefs.open_rooms.push_back(t.room_id);
        if (prefs.open_rooms.empty())
            prefs.open_rooms.push_back(roomId);
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
    {
        auto& state = _shell->pagination_[subRoom];
        if (state.in_flight)
            return;
        state.in_flight = true;
    }
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
                thumbnail:(BOOL)thumb
{
    // Inline thumbnails land in thumbnail_cache_; full-size media in
    // image_cache_. Animated frames always go to anim_cache_.
    tk::PixmapCache& still_cache =
        thumb ? _shell->thumbnail_cache_ : _shell->image_cache_;
    if (bytes.empty() || still_cache.contains(key) ||
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
        still_cache.store(key, std::move(d.still));
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
    if (_shell)
    {
        _shell->tick_anim_();
    }
}

- (void)_stopAnimTick
{
    [_animTimer invalidate];
    _animTimer = nil;
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
                                 [s _startAnimTickIfNeeded];
                                 return f;
                             }
                             {
                                 if (const auto* img =
                                         s->_shell->image_cache_.peek(cache_key))
                                     return img;
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
        if (s->_shell->thread_panel_ == tesseract::ShellBase::ThreadPanel::Open &&
            !s->_shell->current_thread_root_.empty())
        {
            s->_shell->client_->send_thread_sticker(
                s->_shell->current_room_id_, s->_shell->current_thread_root_,
                b, u, j);
        }
        else
        {
            s->_shell->client_->send_sticker(s->_shell->current_room_id_, b, u, j);
        }
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
                                 [s _startAnimTickIfNeeded];
                                 return f;
                             }
                             {
                                 if (const auto* img =
                                         s->_shell->image_cache_.peek(cache_key))
                                     return img;
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
        if (s->_shell->thread_panel_ == tesseract::ShellBase::ThreadPanel::Open &&
            !s->_shell->current_thread_root_.empty())
        {
            s->_shell->client_->send_thread_sticker(
                s->_shell->current_room_id_, s->_shell->current_thread_root_,
                b, u, j);
        }
        else
        {
            s->_shell->client_->send_sticker(s->_shell->current_room_id_, b, u, j);
        }
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
