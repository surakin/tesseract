#import "MainWindowController.h"
#import "tk_locale.h"
#import "LoginView.h"
#import "EmojiPicker.h"
#import "StickerPicker.h"
#import "MacOSTrayIcon.h"
#import "MacScreenLock.h"
#import "RoomWindowController.h"
#import "CallWindowController.h"

#include <tesseract/client.h>
#include <tesseract/event_handler.h>
#include <tesseract/image_pack.h>
#include <tesseract/session_store.h>
#include <tesseract/prefs.h>
#include <tesseract/paths.h>
#include <tesseract/settings.h>
#include <tesseract/visual.h>

#include "app/SlashCommands.h"
#include "app/status_links.h"
#include "tk/anim_image_cache.h"
#include "tk/canvas_cg.h"
#include "tk/inflight_dot.h"
#include "tk/video_decode.h"
#include "tk/host.h"
#include "tk/host_macos.h"
#include "tk/theme.h"
#include "util.h"
#include "views/BrandView.h"
#include "views/MainAppWidget.h"
#include "views/media_drop.h"
#include "views/JoinRoomView.h"
#include "views/SettingsView.h"
#include "views/ShortcodeEngine.h"
#include "views/ShortcodePopup.h"
#include "views/MentionController.h"
#include "views/GifController.h"
#include "views/GifPopup.h"
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

#include "app/AccountManager.h"
#include "app/ShellBase.h"
#include "app/EventHandlerBase.h"
#include "app/SettingsController.h"

#include "views/AccountPicker.h"

#include <algorithm>
#include <cstdlib>
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
//  _TkMenuAction — thin ObjC object that bridges a C++ callback to an
//  NSMenuItem action. Created per-item and kept alive in an NSArray for the
//  synchronous duration of popUpMenuPositioningItem:atLocation:inView:.
// ─────────────────────────────────────────────────────────────────────────
@interface _TkMenuAction : NSObject
- (instancetype)initWithCallback:(std::function<void()>)cb;
- (void)fire:(id)sender;
@end
@implementation _TkMenuAction
{
    std::function<void()> _cb;
}
- (instancetype)initWithCallback:(std::function<void()>)cb
{
    self = [super init];
    if (self)
        _cb = std::move(cb);
    return self;
}
- (void)fire:(id)sender
{
    if (_cb)
        _cb();
}
@end

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
    explicit MacShell(tesseract::AccountManager& account_manager,
                      MainWindowController* ctrl)
        : ShellBase(account_manager), ctrl_(ctrl)
    {
        account_manager_.register_window(this);
        broadcast_rebuild_tray_();
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
    void on_space_unjoined_summaries_ready_ui_(const std::string&) override;
    void on_join_room_outcome_ui_(bool ok, const std::string& room_id) override;
    void show_encryption_setup_overlay_(
        tesseract::views::EncryptionSetupOverlay::Mode mode) override;
    void show_qr_grant_overlay_() override;
    void hide_qr_grant_overlay_() override;
    void on_tray_unread_changed_(bool has_unread,
                                 bool has_highlight) override;
    void on_dock_badge_changed_(uint64_t count) override;
    void on_media_bytes_ready_(const std::string& key,
                               ShellBase::MediaKind kind,
                               std::vector<uint8_t> bytes) override;
    void generate_video_thumbnail_(const std::string& event_id,
                                   const std::string& video_url) override;
    void cache_rgba_image_(const std::string& key, int w, int h,
                           std::vector<uint8_t> rgba) override;

    bool is_main_window_visible_() const override
    {
        if (!ctrl_) return false;
        NSWindow* w = ctrl_.window;
        return w && !w.isMiniaturized &&
               (w.occlusionState & NSWindowOcclusionStateVisible);
    }

    std::int64_t monotonic_ms_() override;
    void start_anim_tick_() override;
    void stop_anim_tick_() override;
    void repaint_anim_frame_() override;
    void start_inflight_tick_() override;
    void stop_inflight_tick_() override;
    void repaint_inflight_spinner_() override;
    void repaint_pickers_() override;

    // handle_timeline_reset_ui_ and handle_message_{inserted,updated,removed}_ui_
    // are NOT overridden here: the shared ShellBase implementations drive the
    // same room_view_ this shell owns (via request_relayout_ → _relayoutChatSurface)
    // and dispatch to secondary windows. They also carry guards this shell used
    // to drop (in-thread reply exclusion, scroll/focus restore on reset), so the
    // base path is strictly more correct.
    void handle_sync_error_ui_(std::string context, std::string user_id,
                               std::string description,
                               bool soft_logout) override;
    void refresh_user_strip_() override;
    void request_relogin_(const std::string& user_id) override;
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
    void on_own_extended_profile_ready_ui_() override;
    void on_profile_field_result_ui_(const std::string& key, bool ok,
                                     const std::string& error) override;
    void update_typing_bar_(const std::string& text, bool visible) override;
    void on_show_status_message_ui_(const std::string& msg) override;
    void on_restore_status_ui_() override;

    tk::ThemeMode os_color_scheme_() const override;
    void apply_theme_ui_(const tk::Theme& t) override;
    tesseract::RoomWindowBase*
    create_secondary_room_window_(const std::string& room_id) override;
    void raise_and_activate_() override;
    void rebuild_tray_() override;
    bool is_ctrl_held_() const override;
    void focus_forward_picker_field_() override;
    void hide_forward_picker_field_() override;
    void switch_active_account_(const std::string& user_id) override;
    void refresh_account_ui_after_switch_() override;
    void spawn_main_window_(
        std::shared_ptr<tesseract::AccountSession> account) override;
    std::unique_ptr<tesseract::IEventHandler>
    make_account_bridge_(const std::string& uid) override;
    // macOS has no in-app per-account notifier; the restore loop's call is a
    // no-op (install_account_up_connector_ likewise relies on the base no-op).
    void install_account_notifier_(
        tesseract::AccountSession& /*session*/) override
    {
    }
#ifdef TESSERACT_CALLS_ENABLED
    std::unique_ptr<tk::AudioPlayback> make_call_audio_output_() override
    {
        return tk::make_audio_playback_macos();
    }
    tesseract::CallWindowBase* create_call_window_() override;
#endif

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
    void open_join_room_dialog_ui_(const std::string& prefill) override;
    bool is_room_search_active_() const override
    {
        return !pending_search_text_.empty();
    }

    // Public C++ API for ObjC++ code — add new features here rather than
    // extending the legacy using-block below.
public:
    // Room messaging
    bool send_room_message(std::string body, std::string formatted);
    void send_reply(std::string reply_event_id, std::string body);
    void send_edit(std::string event_id, std::string new_body);
    void send_reaction(std::string event_id, std::string key,
                       std::string source_mxc);
    void redact_event(std::string event_id);
    void send_sticker(std::string body, std::string url, std::string info_json);
    void send_read_receipt(std::string event_id);
    std::string shortcode_for_mxc(const std::string& mxc) const;

    // Window / presence
    void on_window_closing();
    void notify_window_active(bool active);
    void notify_user_activity();
    void notify_presence_tick();
    void handle_send_presence_toggle(bool enabled);
    void handle_index_messages_toggle(bool enabled);
    void handle_show_membership_events_toggle(bool enabled);
#ifdef TESSERACT_GITHUB_REPO
    void handle_check_for_updates_toggle(bool enabled);
#endif

    // Current-room actions (operate on current_room_id_ internally)
    void handle_compose_text_changed(const std::string& text);
    void handle_compose_room_leaving();
    void mark_room_read();
    void request_forward_history();
    void return_to_live();
    void handle_date_jump(std::uint64_t ts_ms);

    // Account / crypto
    void begin_crypto_identity_reset();
    void on_account_picker_select(const std::string& uid);

    // Room list / space
    void join_room_command(const std::string& room_id);
    void cancel_unjoined_summaries();
    const std::vector<tesseract::RoomSummary>& cached_unjoined_summaries();

    // Tray
    bool focus_tray_unread_popout();
    void navigate_tray_unread();

    // Media / asset fetching
    void ensure_user_avatar(const std::string& mxc,
                            std::uint64_t group_id = 0);
    std::uint64_t media_group_for_room(const std::string& room_id);
    void ensure_room_avatar(const tesseract::RoomInfo& r);
    void ensure_media_image(const std::string& url, int max_w, int max_h,
                            std::uint64_t group_id = 0);
    void ensure_media_thumbnail(const std::string& url, int w, int h,
                                bool animated, std::uint64_t group_id = 0);
    void ensure_viewer_fullres(const std::string& url);
    void ensure_picker_image(const std::string& url, bool is_sticker);
    void ensure_tile(int z, int x, int y);
    std::vector<std::uint8_t> voice_bytes_or_fetch(const std::string& token,
                                                    std::function<void()> on_ready);

    // Theme / settings init (called during setup or on preference change)
    void apply_current_theme();
    void save_settings_debounced();
    void set_theme_preference(tesseract::Settings::ThemePreference pref);
    void set_screen_lock(std::unique_ptr<tesseract::IScreenLock> lock);
    void apply_space_child_counts(std::vector<tesseract::RoomInfo>& rooms);
    void handle_profile_field_change(const std::string& key,
                                     const std::string& value_json);
    void clear_focused_state(const std::string& room_id);
    void persist_room_layout_pref();

    // User profile (read; my_avatar_url_ also settable)
    const std::string& display_name()  const { return my_display_name_; }
    const std::string& user_id()       const { return my_user_id_; }
    const std::string& avatar_url()    const { return my_avatar_url_; }
    void set_avatar_url(std::string u) { my_avatar_url_ = std::move(u); }

    // Misc state accessors
    bool foreign_cross_signing_identity() const
        { return foreign_cross_signing_identity_(); }
    const tesseract::ExtendedProfile& own_extended_profile() const
        { return own_extended_profile_; }
    void set_stats_settings_view(tesseract::views::SettingsView* v)
        { stats_settings_view_ = v; }

    // Status bar / inflight dot snapshots
    struct SyncStatusInfo {
        tesseract::RoomListState room_list_state;
        tesseract::BackupState   backup_state;
        std::uint64_t            imported_keys;
        bool                     has_override;
    };
    SyncStatusInfo sync_status_info() const;
    void           set_sync_progress_shown(bool b);
    struct InflightInfo {
        tk::Color     dot_color;
        std::uint32_t count;
        float         spin_phase;
        std::size_t   pool_pending;
        std::size_t   mut_pool_pending;
        std::size_t   media_pending;
        std::string   urls; // debug; empty in release
    };
    InflightInfo inflight_info() const;
    bool inflight_needs_anim() const;
    std::vector<tesseract::StatusSegment> parse_status(const std::string& raw) const;
    bool tray_unread()    const { return last_tray_unread_; }
    bool tray_highlight() const { return last_tray_highlight_; }

    // Account management
    tesseract::ShellBase::FinalizeLoginResult finalize_login();
    tesseract::ShellBase::LogoutResult        logout_active_account();
    bool switch_account(const std::string& user_id);
    tesseract::ShellBase::RestoreResult       restore_all_accounts();
    bool try_restore_tab_session(const std::vector<std::string>& rooms,
                                 const std::string& preferred);
    // Run the deferred room-restore after sync reaches Running; returns true when
    // a session was restored and the pending list was cleared.
    bool maybe_restore_rooms();

    // Room list
    void start_room_subscription(const std::string& room_id,
                                 std::vector<std::string> visible_ids);
    void request_more_history(std::string room_id);
    void handle_paginate_result(const std::string& room_id, bool reached_start);
    void refresh_room_list() { refresh_room_list_(); }

    // Settings / session
    void ensure_settings_controller();
    void show_status_message(std::string msg);
    void start_search_stats_poll();
    void stop_search_stats_poll();
    bool verification_banner_dismissed() const;
    bool is_secondary_window_startup() const;
    bool is_pinned_window() const;
    void set_verification_banner_dismissed(bool b);

    // Misc one-liners
    void init_pool_callbacks();
    bool tick_anim();
    bool tick_inflight();
    tesseract::Settings::WindowGeometry clamp_to_screens(
        const tesseract::Settings::WindowGeometry& saved,
        int default_w, int default_h);
    void begin_focused_subscription(const std::string& room_id,
                                    const std::string& event_id);
    void apply_media_preview_config(tesseract::Settings::MediaPreviews mode,
                                    bool invite_avatars);
    void commit_room_media_preview_override(
        const std::string& room_id, bool has_override,
        tesseract::MediaPreviewConfig::Mode mode);
    void seed_room_media_section(const std::string& room_id);
    void fetch_room_security_state(const std::string& room_id);
    void seed_image_pack_tab(const std::string& room_id,
                            tesseract::views::RoomSettingsView* target);
    void handle_image_pack_images_needed(const std::string& pack_id,
                                         tesseract::views::RoomSettingsView* target);
    void handle_image_pack_pending_image_added(std::uint64_t local_id,
                                               std::vector<uint8_t> bytes,
                                               std::string mime,
                                               tesseract::views::RoomSettingsView* target);
    void handle_user_pack_pending_image_added(std::uint64_t local_id,
                                              std::vector<uint8_t> bytes,
                                              std::string mime,
                                              tesseract::views::UserPackEditor* target);
    void wire_main_app_widget(tesseract::views::MainAppWidget* app);
    void wire_main_app_viewers(tesseract::views::MainAppWidget* app,
                               tk::Host& host,
                               std::function<void()> request_relayout,
                               std::function<void()> on_image_close = {},
                               std::function<void()> on_video_close = {});
    void wire_voice_capture(tesseract::views::RoomView* rv,
                            std::function<void()> request_repaint,
                            std::function<std::string()> get_room_id,
                            std::function<void()> clear_text_fn);
    void schedule_relayout();
    const std::vector<tesseract::ImagePackImage>& cached_emoticons() const;
    std::string gif_src_disk_key(const std::string& url) const;
    const tesseract::ServerInfo& server_info_ref() const;
    using CacheSizeCallback =
        std::function<void(uint64_t, uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t)>;
    void compute_cache_sizes(CacheSizeCallback cb);
    void clear_all_caches(CacheSizeCallback cb);
    void start_qr_grant_overlay_public();

    // Space navigation
    // push_space captures nav frame + pushes room onto space_stack_.
    // Caller must call SpaceNavFrame::enter() (or refresh) after if needed.
    void push_space(const std::string& room_id, tesseract::views::RoomListView* rlv);
    void pop_space(tesseract::views::RoomListView* rlv);
    bool space_stack_empty() const;
    const std::string& current_space() const;
    const std::vector<std::string>* space_children(const std::string& id) const;

    // Verification
    const std::string& verification_flow_id() const;

    // Misc one-shot state
    const std::vector<tesseract::InviteInfo>* invites_ptr() const;
    void clear_reply_details();
    void drain_pools();
    void set_capture(std::unique_ptr<tk::AudioCapture> c);
    const tesseract::RoomInfo* room_by_id(const std::string& id) const;

    // LEGACY: do not add new entries here. Add a public C++ method above instead.
public:
    using ShellBase::account_manager_;
    using ShellBase::active_account_;
    using ShellBase::add_account_return_idx_;
    using ShellBase::client_;
    using ShellBase::current_room_id_;
    using ShellBase::DecodedImage;
    DecodedImage decode_image_(const std::vector<uint8_t>& bytes, int max_w,
                               int max_h) override;
    void pick_image_file_(
        std::function<void(std::vector<uint8_t>, std::string)> cb) override;
    void bind_settings_controller_() override;
#ifndef NDEBUG
#endif
    // media_disk_cache_ is accessed via account_manager_.media_disk_cache()
    using ShellBase::pending_login_client_;
    using ShellBase::pending_login_is_add_account_;
    using ShellBase::pending_login_temp_dir_;
    using ShellBase::settings_controller_;
    using ShellBase::rooms_;
    using ShellBase::run_async_;
    using ShellBase::run_async_mut_;
    using ShellBase::begin_media_req_;
    using ShellBase::handle_media_ready_ui_;

    // Public method to call the protected update_typing_bar_ method
    void update_typing_bar(const std::string& text, bool visible)
    {
        update_typing_bar_(text, visible);
    }

    // Public methods to call the protected Room Settings helpers.
    void stage_room_settings_avatar_upload(const std::string& room_id,
                                           tesseract::views::RoomSettingsView* target)
    {
        stage_room_settings_avatar_upload_(room_id, target);
    }
    static ShellBase::RoomSettingsCommitOutcome apply_room_settings(
        tesseract::Client* client, const std::string& room_id,
        const tesseract::views::RoomSettingsChanges& changes)
    {
        return ShellBase::apply_room_settings_(client, room_id, changes);
    }

    std::vector<tk::Rect> get_screen_work_areas_() const override
    {
        std::vector<tk::Rect> result;
        NSArray<NSScreen*>* screens = [NSScreen screens];
        if (!screens.count) return result;
        // y-flip reference: top of screen 0 in Cocoa coords = origin.y + height.
        // We store geometry in top-left-origin coords (y=0 at top of primary).
        const CGFloat primaryTop =
            [[screens firstObject] frame].origin.y +
            [[screens firstObject] frame].size.height;
        for (NSScreen* s in screens)
        {
            NSRect vf = s.visibleFrame; // excludes menu bar + dock
            tk::Rect r;
            r.x = static_cast<float>(vf.origin.x);
            r.y = static_cast<float>(primaryTop - vf.origin.y - vf.size.height);
            r.w = static_cast<float>(vf.size.width);
            r.h = static_cast<float>(vf.size.height);
            result.push_back(r);
        }
        return result;
    }

    // Extract thumbnail, dimensions, and duration from a dropped file on a
    // background thread; posts result back via post_to_ui_. When `target` is
    // non-null the result is posted to that compose bar (a pop-out window's),
    // guarded by `target_alive`; otherwise to the main compose bar. Overrides
    // the ShellBase drag-and-drop probe hook.
    void extract_drop_media_(std::uint32_t pending_gen,
                             std::vector<std::uint8_t> bytes,
                             std::string mime,
                             tesseract::views::ComposeBar* target = nullptr,
                             std::shared_ptr<bool> target_alive = nullptr)
        override;

    // GIF search results arrive on a MacShell (ShellBase) callback but the
    // GifController lives on the ObjC MainWindowController; the controller sets
    // these to forward into itself.
    std::function<void(std::uint64_t, std::vector<tesseract::GifResult>)>
        gif_on_results_;
    std::function<void(std::uint64_t, std::string)> gif_on_failed_;
    void handle_gif_results_ui_(std::uint64_t request_id,
                                std::vector<tesseract::GifResult> results)
        override
    {
        if (gif_on_results_)
            gif_on_results_(request_id, std::move(results));
    }
    void handle_gif_search_failed_ui_(std::uint64_t request_id,
                                      std::string message) override
    {
        if (gif_on_failed_)
            gif_on_failed_(request_id, std::move(message));
    }

    // Borrowed pointers set by ObjC side after building the chat surface.
    // main_app_ / room_view_ now live in ShellBase; re-export them so the ObjC
    // side can keep reaching them through _shell. app_surface_ is the macOS
    // native surface and stays here.
    using ShellBase::main_app_;
    using ShellBase::room_view_;
    tk::macos::Surface* app_surface_ = nullptr;

    // Current room-list search query (empty when search is inactive).
    std::string pending_search_text_;

    // Native text fields for the encryption-setup overlay. Owned here (not in
    // ObjC ivars) so show_encryption_setup_overlay_() can reach them from C++.
    std::unique_ptr<tk::NativeTextField> enc_passphrase_field_;
    std::unique_ptr<tk::NativeTextField> enc_key_field_;

    // Native text field for the QR grant check-code input.
    std::unique_ptr<tk::NativeTextField> qr_check_code_field_;

    // Public forwarder for the protected ShellBase virtual so ObjC++ code can
    // call it through _shell without a friend declaration.
    void show_encryption_setup(
        tesseract::views::EncryptionSetupOverlay::Mode mode)
    {
        show_encryption_setup_overlay_(mode);
    }

    // settings_controller_ now lives in ShellBase (created/reset via
    // ensure_settings_controller_); exposed above via a using-declaration.

    // Shortcode engine + transient state (owned here, accessed via _shell->).
    tesseract::views::ShortcodeEngine shortcode_engine_;
    tesseract::views::ShortcodeMatch shortcode_active_match_{};
    std::vector<tesseract::views::ShortcodeSuggestion>
        shortcode_current_suggestions_;

    // Room-switch member-list cache backing the received-mention-pill avatar
    // provider (set_mention_avatar_provider) — names/avatar_urls only; no
    // avatar bytes are fetched until a pill actually paints. The
    // MentionController fetches its own member list independently for
    // autocomplete.
    std::vector<tesseract::RoomMember> cached_room_members_;
    std::string cached_members_room_;

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
//  Inflight status-bar indicator: center dot + optional spinning ring.
// ─────────────────────────────────────────────────────────────────────────

@interface InflightDotView : NSView
@property (nonatomic) uint32_t  inflightCount;
@property (nonatomic) tk::Color dotColor;
@property (nonatomic) float     spinPhase;
@end

@implementation InflightDotView
- (instancetype)initWithFrame:(NSRect)frame
{
    if ((self = [super initWithFrame:frame]))
    {
        _dotColor = tk::Color::rgb(0x40BF4D);
    }
    return self;
}
- (void)drawRect:(NSRect)dirtyRect
{
    (void)dirtyRect;
    CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
    auto cv = tk::cg::make_canvas(ctx);
    const CGSize sz  = self.bounds.size;
    const float  c   = static_cast<float>(sz.width) / 2.0f;
    constexpr tk::Color kRingColor = tk::Color::rgb(0xA0A0A6);
    tk::draw_inflight_indicator(*cv, {c, c},
                                tk::kInflightDotR, tk::kInflightOrbitR,
                                tk::kInflightRingDotR,
                                _dotColor, kRingColor,
                                _spinPhase,
                                _inflightCount >= 2);
}
- (BOOL)isFlipped { return YES; }
@end

// ─────────────────────────────────────────────────────────────────────────
//  Internal IBO that the C++ EventBridge calls back into.
// ─────────────────────────────────────────────────────────────────────────

@interface MainWindowController () <
    LoginViewDelegate, UNUserNotificationCenterDelegate, NSWindowDelegate>
// Provides C++ access to the MacShell from spawn_main_window_() (same .mm file).
@property (readonly, nonatomic) MacShell* shell;
- (void)handlePaginateResultForRoom:(std::string)roomId
                      reached_start:(BOOL)reached;
- (void)requestMoreHistoryForRoom:(std::string)roomId;
- (void)_switchActiveAccount:(const std::string&)user_id;
- (void)_refreshAccountUIAfterSwitch;
- (void)_populateUserStrip;
- (void)_bindSettingsControllerNative;
- (void)_beginAddAccount;
- (void)_logoutActiveAccount;
- (void)_openSettings;
- (void)_showQRGrant;
- (void)_openQuickSwitch;
- (void)_closeQuickSwitch;
- (void)_openMessageSearch;
- (void)_closeMessageSearch;
- (void)_closeForwardPicker;
- (void)_focusForwardPickerField;
- (void)_hideForwardPickerField;
- (void)_openFindInRoom;
- (void)_closeFindInRoom;
- (void)_navigateHistoryBack;
- (void)_navigateHistoryForward;
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
- (void)showGifPopup;
- (void)hideGifPopup;
- (BOOL)gifPopupVisible;
- (void)repaintGifPopupAnimRegions;
- (void)repaintSettingsAnimRegions;
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
// Whichever RoomSettingsView instance currently has a tab open —
// _mainApp->room_view()'s (a normal room) or _mainApp->space_root()'s (a
// space root, since the wrench icon there opens its own separate
// RoomSettingsView instance). nullptr if neither is open.
- (tesseract::views::RoomSettingsView*)_activeRoomSettingsView;
- (void)_onRoomListStateChanged;
- (void)_onServerInfoReady;
- (void)_onOwnExtendedProfileReady;
- (void)_onProfileFieldResult:(const std::string&)key
                           ok:(bool)ok
                        error:(const std::string&)error;
- (void)_buildStatusBar:(NSView*)content;
- (void)_refreshSyncStatus;
- (void)_setStatusLabelText:(NSString*)text;
- (void)_onInflightChanged;
- (void)_updateTrayUnread:(bool)hasUnread highlight:(bool)hasHighlight;
- (void)_rebuildTrayMenu;

// Sticker picker + animated stickers.
- (void)handleImagePacksUpdated;
- (void)_showStickerPicker;
- (void)_showStickerPickerAtRect:(tk::Rect)btn;
- (void)_showStickerContextMenuAt:(NSPoint)screenPt;
- (void)_onStickerSave:(id)sender;
- (void)_startAnimTickIfNeeded;
- (void)_stopAnimTick;
- (void)_animTick:(NSTimer*)timer;
- (void)_startInflightTickIfNeeded;
- (void)_stopInflightTick;
- (void)_inflightTick:(NSTimer*)timer;
- (void)_repaintInflightSpinner;
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
- (void)_openJoinRoomSheetWithPrefill:(NSString*)prefill;
// Designated init for spawned (secondary) windows that share an existing
// AccountManager instead of owning their own.
- (instancetype)_initWithSharedAccountManager:(tesseract::AccountManager*)mgr;
@end

namespace
{

// Minimal JSON string escaper for profile field values.
std::string json_quote(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s)
    {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else out += c;
    }
    out += '"';
    return out;
}

// ── MacShell method implementations ──────────────────────────────────────

std::unique_ptr<tesseract::IEventHandler>
MacShell::make_account_bridge_(const std::string& uid)
{
    auto bridge = std::make_unique<tesseract::EventHandlerBase>(this);
    bridge->set_user_id(uid);
    return bridge;
}

#ifdef TESSERACT_CALLS_ENABLED
tesseract::CallWindowBase* MacShell::create_call_window_()
{
    return tesseract::make_mac_call_window(this);
}
#endif

void MacShell::open_join_room_dialog_ui_(const std::string& prefill)
{
    NSString* ns = prefill.empty() ? nil
                                   : [NSString stringWithUTF8String:prefill.c_str()];
    [ctrl_ _openJoinRoomSheetWithPrefill:ns];
}

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

void MacShell::focus_forward_picker_field_()
{
    if (ctrl_)
        [ctrl_ _focusForwardPickerField];
}

void MacShell::hide_forward_picker_field_()
{
    if (ctrl_)
        [ctrl_ _hideForwardPickerField];
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
        if (const auto* r = room_by_id_(current_room_id_))
        {
            if (room_view_)
            {
                room_view_->set_room(*r);
                [c _relayoutChatSurface];
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

void MacShell::on_space_unjoined_summaries_ready_ui_(const std::string&)
{
    if (ctrl_)
    {
        [ctrl_ _refreshRoomList];
    }
}

void MacShell::on_join_room_outcome_ui_(bool ok, const std::string&)
{
    if (!ok && main_app_ && main_app_->room_preview())
        main_app_->room_preview()->set_state(
            tesseract::views::RoomPreviewView::State::Idle);
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

void MacShell::show_qr_grant_overlay_()
{
    if (!main_app_ || !app_surface_) return;
    auto* view = main_app_->qr_grant_view();
    if (!view) return;
    qr_check_code_field_ = app_surface_->host().make_text_field();
    qr_check_code_field_->set_on_changed([view](const std::string& t) {
        view->set_check_code_text(t);
    });
    qr_check_code_field_->set_visible(false);
}

void MacShell::hide_qr_grant_overlay_()
{
    qr_check_code_field_.reset();
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

void MacShell::on_dock_badge_changed_(uint64_t count)
{
    NSString* label = count > 0
        ? [NSString stringWithFormat:@"%llu", (unsigned long long)count]
        : @"";
    [NSApp.dockTile setBadgeLabel:label];
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
    if (kind == MediaKind::MediaImage || kind == MediaKind::MediaThumbnail ||
        kind == MediaKind::Sticker || kind == MediaKind::Reaction)
    {
        // Decode off the UI thread — CGImageSource is thread-safe. Store and
        // repaint on the UI thread once the decode completes.
        const bool is_thumb = (kind == MediaKind::MediaThumbnail);
        tk::PixmapCache& still_cache =
            is_thumb ? account_manager_.thumbnail_cache() : account_manager_.image_cache();
        if (still_cache.contains(key) || account_manager_.anim_cache().has(key))
            return;
        run_async_(
            [this, key, kind, is_thumb,
             bytes = std::move(bytes)]() mutable
            {
                auto d = std::make_shared<DecodedImage>(
                    decode_image_(bytes, 0, 0));
                post_to_ui_(
                    [this, key, kind, is_thumb, d]() mutable
                    {
                        MainWindowController* c = ctrl_;
                        if (!c) return;
                        tk::PixmapCache& sc =
                            is_thumb ? account_manager_.thumbnail_cache()
                                     : account_manager_.image_cache();
                        if (sc.contains(key) || account_manager_.anim_cache().has(key))
                            return;
                        if (!d->frames.empty())
                        {
                            const std::int64_t now =
                                static_cast<std::int64_t>(
                                    [[NSDate date] timeIntervalSince1970] *
                                    1000.0);
                            account_manager_.anim_cache().store(
                                key, std::move(d->frames),
                                std::move(d->delays_ms), now);
                            start_anim_tick_();
                        }
                        else if (d->still)
                        {
                            sc.store(key, std::move(d->still));
                        }
                        else
                        {
                            return;
                        }
                        if (room_view_)
                            room_view_->notify_image_ready(key);
                        // Coalescing, not [c _relayoutChatSurface] directly:
                        // a dense grid (the room media gallery) can land
                        // dozens of these completions in a tight burst, and
                        // an uncoalesced relayout() here does a full
                        // app-wide arrange() per completion — including a
                        // full re-measure of the main chat timeline's rows
                        // — which has nothing to do with a thumbnail
                        // arriving. schedule_relayout_() folds a burst of
                        // these into one deferred pass (mirrors GTK4/Qt6,
                        // which already use this here).
                        schedule_relayout_();
                        [c _relayoutShortcodePopupIfVisible];
                        notify_secondary_media_ready_(key, kind);
                    });
            });
        return;
    }
    if (kind == MediaKind::Tile)
    {
        if (account_manager_.image_cache().contains(key))
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
        account_manager_.image_cache().store(key, tk::cg::make_image(img));
        CGImageRelease(img);
        // A map tile fills a fixed-size map card and isn't a tracked row media
        // source, so it needs only a repaint, not a re-measure — the chat
        // surface relayout below re-draws it (LocationMapPanner::paint re-reads
        // the tile from the cache). The old invalidate_data() forced a full
        // O(timeline) re-measure just to repaint a fixed card.
        [c _relayoutChatSurface];
        notify_secondary_media_ready_(key, kind);
        return;
    }
    if (bytes.empty() || account_manager_.thumbnail_cache().contains(key))
    {
        return;
    }
    // Decode off the UI thread (CGImageSource is thread-safe — same basis as
    // the MediaImage path above). A burst of avatar fetches (e.g. after a room
    // switch) would otherwise decode synchronously here and stall the UI event
    // queue that a just-sent message's local echo waits in. Store + relayout
    // hop back to the UI thread.
    run_async_(
        [this, key, kind, bytes = std::move(bytes)]() mutable
        {
            auto d = std::make_shared<DecodedImage>(decode_image_(bytes, 0, 0));
            post_to_ui_(
                [this, key, kind, d]() mutable
                {
                    MainWindowController* c = ctrl_;
                    if (!c) return;
                    if (account_manager_.thumbnail_cache().contains(key))
                        return;
                    // Avatars render static: use the still, or the first frame
                    // of an animated source.
                    std::unique_ptr<tk::Image> img;
                    if (d->still)
                        img = std::move(d->still);
                    else if (!d->frames.empty())
                        img = std::move(d->frames.front());
                    if (!img)
                        return;
                    account_manager_.thumbnail_cache().store(key,
                                                             std::move(img));
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
                    notify_secondary_media_ready_(key, kind);
                });
        });
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
    if (!client_) return;
    auto req_id = begin_media_req_(0,
        [this, eid](std::vector<uint8_t> bytes) mutable
        {
            if (bytes.empty()) return;
            // Callback is on the UI thread — do the AVFoundation work directly.
            NSString* tmpDir = NSTemporaryDirectory();
            NSString* eidNS = [NSString stringWithUTF8String:eid.c_str()];
            NSString* tmpPath =
                [tmpDir stringByAppendingPathComponent:
                            [NSString stringWithFormat:@"vtmp_%@.mp4", eidNS]];
            NSData* data = [NSData dataWithBytes:bytes.data()
                                          length:bytes.size()];
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
            if (!frame) return;
            std::string key = "thumb::" + eid;
            if (account_manager_.image_cache().contains(key))
            {
                CGImageRelease(frame);
                return;
            }
            account_manager_.image_cache().store(
                key, tk::cg::make_image(frame));
            CGImageRelease(frame);
            MainWindowController* c2 = ctrl_;
            if (c2)
                [c2 _relayoutChatSurface];
        });
    client_->fetch_source_bytes_async(req_id, src);
}

void MacShell::extract_drop_media_(std::uint32_t pending_gen,
                                   std::vector<std::uint8_t> bytes,
                                   std::string mime,
                                   tesseract::views::ComposeBar* target,
                                   std::shared_ptr<bool> target_alive)
{
    run_async_([this, pending_gen, target, target_alive = std::move(target_alive),
                bytes = std::move(bytes), mime = std::move(mime)]() mutable
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

        // Post result back to the UI thread. A pop-out target (guarded by its
        // liveness token) takes precedence; otherwise resolve the main
        // compose_bar() at call time to avoid a dangling pointer.
        post_to_ui_([this, info = std::move(info), target,
                     target_alive = std::move(target_alive)]() mutable
        {
            if (target)
            {
                if (target_alive && *target_alive)
                    target->update_pending_attachment(info);
            }
            else if (room_view_)
            {
                room_view_->compose_bar()->update_pending_attachment(info);
            }
        });
    });
}

void MacShell::cache_rgba_image_(const std::string& key, int w, int h,
                                 std::vector<uint8_t> rgba)
{
    if (account_manager_.image_cache().contains(key))
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
    account_manager_.image_cache().store(key, tk::cg::make_image(img));
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

void MacShell::bind_settings_controller_()
{
    // settings_controller_ is freshly constructed by
    // ShellBase::ensure_settings_controller_(); the native AppKit binding
    // (NSAlert / NSSavePanel / NSOpenPanel dialog hooks + settings view/name
    // field wiring) lives in the ObjC++ controller method below.
    [ctrl_ _bindSettingsControllerNative];
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
    if (ctrl_)
    {
        [ctrl_ repaintGifPopupAnimRegions];
        [ctrl_ repaintSettingsAnimRegions];
    }
}

void MacShell::start_inflight_tick_()
{
    if (ctrl_)
        [ctrl_ _startInflightTickIfNeeded];
}

void MacShell::stop_inflight_tick_()
{
    if (ctrl_)
        [ctrl_ _stopInflightTick];
}

void MacShell::repaint_inflight_spinner_()
{
    if (ctrl_)
        [ctrl_ _repaintInflightSpinner];
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

// handle_timeline_reset_ui_ and handle_message_{inserted,updated,removed}_ui_
// are inherited from ShellBase: it drives the same room_view_ this shell owns
// (relayout via request_relayout_ → _relayoutChatSurface) and dispatches to
// secondary windows. See the MacShell class declaration for the rationale.

void MacShell::handle_sync_error_ui_(std::string context, std::string user_id,
                                     std::string description, bool soft_logout)
{
    // Agnostic state machine lives in ShellBase; this shell only supplies the
    // native restart timer (post_to_ui_after_), status label, user strip
    // (refresh_user_strip_) and relogin (request_relogin_).
    handle_sync_error_impl_(std::move(context), std::move(user_id),
                            std::move(description), soft_logout);
}

void MacShell::refresh_user_strip_()
{
    if (ctrl_)
    {
        [ctrl_ _populateUserStrip];
    }
}

void MacShell::request_relogin_(const std::string& user_id)
{
    const bool is_active =
        active_account_ && active_account_->user_id == user_id;
    if (is_active)
    {
        // ShellBase already showed "Session expired…" and cleared/stopped the
        // account; drop to the login flow.
        if (ctrl_)
            [ctrl_ _logoutActiveAccount];
        return;
    }
    // A non-active account expired: forget it and drop it from the persisted
    // index so it doesn't reappear on next launch, without disturbing the
    // foregrounded account. (ShellBase already cleared its stored session and
    // stopped its sync.) Mirrors the Windows request_relogin_ behavior.
    account_manager_.remove_account(user_id);
    auto index = tesseract::SessionStore::load_index();
    index.user_ids.erase(
        std::remove(index.user_ids.begin(), index.user_ids.end(), user_id),
        index.user_ids.end());
    if (index.active_user_id == user_id && active_account_)
    {
        index.active_user_id = active_account_->user_id;
    }
    tesseract::SessionStore::save_index(index);
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
    dismiss_encryption_setup_after_verification_();
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

void MacShell::on_own_extended_profile_ready_ui_()
{
    MainWindowController* c = ctrl_;
    if (c)
        [c _onOwnExtendedProfileReady];
}

void MacShell::on_profile_field_result_ui_(const std::string& key,
                                            bool ok,
                                            const std::string& error)
{
    MainWindowController* c = ctrl_;
    if (c)
        [c _onProfileFieldResult:key ok:ok error:error];
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

void MacShell::on_show_status_message_ui_(const std::string& msg)
{
    [ctrl_ _setStatusLabelText:@(msg.c_str())];
}

void MacShell::on_restore_status_ui_()
{
    [ctrl_ _refreshSyncStatus];
}

tesseract::RoomWindowBase*
MacShell::create_secondary_room_window_(const std::string& room_id)
{
    return tesseract::make_mac_room_window(this, room_id);
}

void MacShell::raise_and_activate_()
{
    if (ctrl_ && ctrl_.window)
    {
        [ctrl_.window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }
}

void MacShell::rebuild_tray_()
{
    if (ctrl_)
        [ctrl_ _rebuildTrayMenu];
}

bool MacShell::is_ctrl_held_() const
{
    return ([NSEvent modifierFlags] & NSEventModifierFlagCommand) != 0;
}

void MacShell::switch_active_account_(const std::string& user_id)
{
    if (ctrl_)
    {
        [ctrl_ _switchActiveAccount:user_id];
    }
}

void MacShell::refresh_account_ui_after_switch_()
{
    if (ctrl_)
    {
        [ctrl_ _refreshAccountUIAfterSwitch];
    }
}

void MacShell::spawn_main_window_(
    std::shared_ptr<tesseract::AccountSession> account)
{
    MainWindowController* ctrl =
        [[MainWindowController alloc]
            _initWithSharedAccountManager:&account_manager_];
    ctrl.shell->set_initial_account(account);
    // Shared hand-off: re-point bridge at the new window, seed caches, pin, and
    // register dedicated — before the new window's deferred doLogin().
    hand_account_to_spawned_window_(ctrl.shell, account);
    [ctrl showWindow:nil];
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
#ifdef TESSERACT_CALLS_ENABLED
    if (call_window_)
        call_window_->apply_theme(t);
#endif
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
            if (const auto* r = room_by_id_(t.room_id))
            {
                name = r->name;
                const std::string& av_mxc = r->effective_avatar_url();
                if (!av_mxc.empty())
                {
                    avatar = account_manager_.thumbnail_cache().peek(av_mxc);
                }
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
// MacShell public messaging API
// ─────────────────────────────────────────────────────────────────────────────

bool MacShell::send_room_message(std::string body, std::string formatted)
{
    if (current_room_id_.empty())
        return false;
    auto outcome = dispatch_room_send_(current_room_id_, std::move(body),
                                       std::move(formatted));
    return outcome.handled_as_command || bool(outcome.send_result);
}

void MacShell::send_reply(std::string reply_event_id, std::string body)
{
    if (current_room_id_.empty() || !client_)
        return;
    client_->send_reply(current_room_id_, std::move(reply_event_id),
                        std::move(body));
}

void MacShell::send_edit(std::string event_id, std::string new_body)
{
    if (current_room_id_.empty() || !client_)
        return;
    client_->send_edit(current_room_id_, std::move(event_id),
                       std::move(new_body));
}

void MacShell::send_reaction(std::string event_id, std::string key,
                              std::string source_mxc)
{
    if (current_room_id_.empty() || !client_)
        return;
    if (!source_mxc.empty())
    {
        const std::string sc = shortcode_for_mxc_(source_mxc);
        const std::string shortcode = sc.empty() ? std::string() : ":" + sc + ":";
        client_->send_reaction_custom(current_room_id_, event_id, source_mxc,
                                      shortcode);
        return;
    }
    client_->send_reaction(current_room_id_, std::move(event_id), std::move(key));
}

void MacShell::redact_event(std::string event_id)
{
    if (current_room_id_.empty() || !client_)
        return;
    client_->redact_event(current_room_id_, std::move(event_id));
}

void MacShell::send_sticker(std::string body, std::string url,
                             std::string info_json)
{
    if (current_room_id_.empty() || !client_)
        return;
    if (thread_panel_ == ShellBase::ThreadPanel::Open &&
        !current_thread_root_.empty())
    {
        client_->send_thread_sticker(current_room_id_, current_thread_root_,
                                     body, url, info_json);
    }
    else
    {
        client_->send_sticker(current_room_id_, body, url, info_json);
    }
}

void MacShell::send_read_receipt(std::string event_id)
{
    if (current_room_id_.empty())
        return;
    maybe_send_read_receipt_(current_room_id_, std::move(event_id));
}

std::string MacShell::shortcode_for_mxc(const std::string& mxc) const
{
    return shortcode_for_mxc_(mxc);
}

// ─────────────────────────────────────────────────────────────────────────────
// MacShell public event / action API
// ─────────────────────────────────────────────────────────────────────────────

void MacShell::on_window_closing()        { on_window_closing_(); }
void MacShell::notify_window_active(bool active) { notify_window_active_(active); }
void MacShell::notify_user_activity()     { notify_user_activity_(); }
void MacShell::notify_presence_tick()     { notify_presence_tick_(); }
void MacShell::handle_send_presence_toggle(bool enabled)
    { handle_send_presence_toggle_(enabled); }
void MacShell::handle_index_messages_toggle(bool enabled)
    { handle_index_messages_toggle_(enabled); }
void MacShell::handle_show_membership_events_toggle(bool enabled)
    { handle_show_membership_events_toggle_(enabled); }
#ifdef TESSERACT_GITHUB_REPO
void MacShell::handle_check_for_updates_toggle(bool enabled)
    { handle_check_for_updates_toggle_(enabled); }
#endif
void MacShell::begin_crypto_identity_reset() { begin_crypto_identity_reset_(); }
void MacShell::on_account_picker_select(const std::string& uid)
    { on_account_picker_select_(uid); }
void MacShell::join_room_command(const std::string& room_id)
    { join_room_command_(room_id); }
void MacShell::handle_date_jump(std::uint64_t ts_ms) { handle_date_jump_(ts_ms); }
void MacShell::handle_compose_text_changed(const std::string& text)
    { handle_compose_text_changed_(text); }
void MacShell::handle_compose_room_leaving()
    { handle_compose_room_leaving_(current_room_id_); }
void MacShell::mark_room_read()     { mark_room_read_(current_room_id_); }
void MacShell::request_forward_history() { request_forward_history_(current_room_id_); }
void MacShell::return_to_live()          { return_to_live_(current_room_id_); }
void MacShell::cancel_unjoined_summaries() { cancel_unjoined_summaries_(); }
bool MacShell::focus_tray_unread_popout() { return focus_tray_unread_popout_(); }
void MacShell::navigate_tray_unread()    { navigate_tray_unread_(); }
const std::vector<tesseract::RoomSummary>& MacShell::cached_unjoined_summaries()
{
    return get_cached_unjoined_summaries_(space_stack_.back());
}

// ─────────────────────────────────────────────────────────────────────────────
// MacShell public media / asset API
// ─────────────────────────────────────────────────────────────────────────────

void MacShell::ensure_user_avatar(const std::string& mxc,
                                  std::uint64_t group_id)
    { ensure_user_avatar_(mxc, group_id); }
std::uint64_t MacShell::media_group_for_room(const std::string& room_id)
    { return media_group_for_room_(room_id); }
void MacShell::ensure_room_avatar(const tesseract::RoomInfo& r)
    { ensure_room_avatar_(r); }
void MacShell::ensure_media_image(const std::string& url, int max_w, int max_h,
                                   std::uint64_t group_id)
    { ensure_media_image_(url, max_w, max_h, group_id); }
void MacShell::ensure_media_thumbnail(const std::string& url, int w, int h,
                                       bool animated, std::uint64_t group_id)
    { ensure_media_thumbnail_(url, w, h, animated, group_id); }
void MacShell::ensure_viewer_fullres(const std::string& url)
    { ensure_viewer_fullres_(url); }
void MacShell::ensure_picker_image(const std::string& url, bool is_sticker)
    { ensure_picker_image_(url, is_sticker); }
void MacShell::ensure_tile(int z, int x, int y)
    { ensure_tile_async(z, x, y); }
std::vector<std::uint8_t> MacShell::voice_bytes_or_fetch(
    const std::string& token, std::function<void()> on_ready)
{
    return voice_bytes_or_fetch_(token, std::move(on_ready));
}

// ─────────────────────────────────────────────────────────────────────────────
// MacShell public settings / theme / layout API
// ─────────────────────────────────────────────────────────────────────────────

void MacShell::apply_current_theme()    { apply_current_theme_(); }
void MacShell::save_settings_debounced() { save_settings_debounced_(); }
void MacShell::set_theme_preference(tesseract::Settings::ThemePreference pref)
    { set_theme_preference_(pref); }
void MacShell::set_screen_lock(std::unique_ptr<tesseract::IScreenLock> lock)
    { set_screen_lock_(std::move(lock)); }
void MacShell::apply_space_child_counts(std::vector<tesseract::RoomInfo>& rooms)
    { apply_space_child_counts_(rooms); }
void MacShell::handle_profile_field_change(const std::string& key,
                                            const std::string& value_json)
    { handle_profile_field_change_(key, value_json); }
void MacShell::clear_focused_state(const std::string& room_id)
    { clear_focused_state_(room_id); }
void MacShell::persist_room_layout_pref() { persist_room_layout_pref_(); }

// ─────────────────────────────────────────────────────────────────────────────
// MacShell public status-bar / inflight snapshot API
// ─────────────────────────────────────────────────────────────────────────────

MacShell::SyncStatusInfo MacShell::sync_status_info() const
{
    return {last_room_list_state_, last_backup_state_,
            last_imported_keys_,   has_status_override_()};
}
void MacShell::set_sync_progress_shown(bool b) { sync_progress_shown_ = b; }

MacShell::InflightInfo MacShell::inflight_info() const
{
    InflightInfo i;
    i.dot_color        = inflight_dot_color_();
    i.count            = inflight_total_();
    i.spin_phase       = inflight_spin_phase_();
    i.pool_pending     = pool_pending_count_();
    i.mut_pool_pending = mut_pool_pending_count_();
    i.media_pending    = pending_media_count_();
#ifndef NDEBUG
    i.urls             = last_inflight_urls_;
#endif
    return i;
}
bool MacShell::inflight_needs_anim() const { return inflight_needs_anim_(); }
std::vector<tesseract::StatusSegment>
MacShell::parse_status(const std::string& raw) const
{
    return parse_status_message_(raw);
}

// ─────────────────────────────────────────────────────────────────────────────
// MacShell public account-management API
// ─────────────────────────────────────────────────────────────────────────────

tesseract::ShellBase::FinalizeLoginResult MacShell::finalize_login()
    { return finalize_login_(); }
tesseract::ShellBase::LogoutResult MacShell::logout_active_account()
    { return logout_active_account_impl_(); }
bool MacShell::switch_account(const std::string& user_id)
    { return switch_active_account_impl_(user_id); }
tesseract::ShellBase::RestoreResult MacShell::restore_all_accounts()
    { return restore_all_accounts_(); }
bool MacShell::try_restore_tab_session(const std::vector<std::string>& rooms,
                                        const std::string& preferred)
    { return try_restore_tab_session_(rooms, preferred); }
bool MacShell::maybe_restore_rooms()
{
    if (pending_restore_rooms_.empty())
        return false;
    if (!try_restore_tab_session_(pending_restore_rooms_, pending_restore_rooms_[0]))
        return false;
    pending_restore_rooms_.clear();
    return true;
}

void MacShell::start_room_subscription(const std::string& room_id,
                                        std::vector<std::string> visible_ids)
    { start_room_subscription_(room_id, std::move(visible_ids)); }

void MacShell::request_more_history(std::string room_id)
{
    if (room_id.empty())
        return;
    auto& state = pagination_[room_id];
    if (state.in_flight || state.reached_start)
        return;
    state.in_flight = true;
    if (room_view_)
        room_view_->set_paginating(true);
    start_anim_tick_();

    __weak MainWindowController* wc = ctrl_;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto pr = client_->paginate_back_with_status(room_id, 50);
        BOOL reached = pr.ok && pr.reached_start;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (MainWindowController* c = wc)
                [c handlePaginateResultForRoom:room_id reached_start:reached];
        });
    });
}

void MacShell::handle_paginate_result(const std::string& room_id,
                                       bool reached_start)
{
    auto it = pagination_.find(room_id);
    if (it == pagination_.end())
        return;
    it->second.in_flight    = false;
    it->second.reached_start = reached_start;
    if (room_id == current_room_id_ && room_view_)
    {
        room_view_->message_list()->set_paginating(false);
        schedule_relayout_();
        room_view_->message_list()->reset_near_top_latch();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MacShell public settings / misc API
// ─────────────────────────────────────────────────────────────────────────────

void MacShell::ensure_settings_controller() { ensure_settings_controller_(); }
void MacShell::show_status_message(std::string msg)
    { show_status_message_(std::move(msg)); }
void MacShell::start_search_stats_poll() { start_search_index_stats_poll_(); }
void MacShell::stop_search_stats_poll()  { stop_search_index_stats_poll_(); }
bool MacShell::verification_banner_dismissed() const
    { return verification_banner_dismissed_; }
bool MacShell::is_secondary_window_startup() const
    { return is_secondary_window_startup_(); }
bool MacShell::is_pinned_window() const    { return is_pinned_window_; }
void MacShell::set_verification_banner_dismissed(bool b)
    { verification_banner_dismissed_ = b; }
void MacShell::init_pool_callbacks() { init_pool_callbacks_(); }
bool MacShell::tick_anim()           { return tick_anim_(); }
bool MacShell::tick_inflight()        { return inflight_tick_(); }
tesseract::Settings::WindowGeometry MacShell::clamp_to_screens(
    const tesseract::Settings::WindowGeometry& saved, int default_w, int default_h)
{
    return clamp_to_screens_(saved, default_w, default_h, get_screen_work_areas_());
}
void MacShell::begin_focused_subscription(const std::string& room_id,
                                           const std::string& event_id)
    { begin_focused_subscription_(room_id, event_id); }
void MacShell::apply_media_preview_config(tesseract::Settings::MediaPreviews mode,
                                           bool invite_avatars)
    { apply_media_preview_config_(mode, invite_avatars); }
void MacShell::commit_room_media_preview_override(
    const std::string& room_id, bool has_override,
    tesseract::MediaPreviewConfig::Mode mode)
    { commit_room_media_preview_override_(room_id, has_override, mode); }
void MacShell::seed_room_media_section(const std::string& room_id)
    { seed_room_media_section_(room_id); }
void MacShell::fetch_room_security_state(const std::string& room_id)
    { fetch_room_security_state_(room_id); }
void MacShell::seed_image_pack_tab(const std::string& room_id,
                                   tesseract::views::RoomSettingsView* target)
    { seed_image_pack_tab_(room_id, target); }
void MacShell::handle_image_pack_images_needed(const std::string& pack_id,
                                               tesseract::views::RoomSettingsView* target)
    { handle_image_pack_images_needed_(pack_id, target); }
void MacShell::handle_image_pack_pending_image_added(
    std::uint64_t local_id, std::vector<uint8_t> bytes, std::string mime,
    tesseract::views::RoomSettingsView* target)
    { handle_image_pack_pending_image_added_(local_id, std::move(bytes), std::move(mime), target); }
void MacShell::handle_user_pack_pending_image_added(
    std::uint64_t local_id, std::vector<uint8_t> bytes, std::string mime,
    tesseract::views::UserPackEditor* target)
    { handle_user_pack_pending_image_added_(local_id, std::move(bytes), std::move(mime), target); }
void MacShell::wire_main_app_widget(tesseract::views::MainAppWidget* app)
    { wire_main_app_widget_(app); }
void MacShell::wire_main_app_viewers(tesseract::views::MainAppWidget* app,
                                      tk::Host& host,
                                      std::function<void()> request_relayout,
                                      std::function<void()> on_image_close,
                                      std::function<void()> on_video_close)
{
    wire_main_app_viewers_(app, host, std::move(request_relayout),
                           std::move(on_image_close), std::move(on_video_close));
}
void MacShell::wire_voice_capture(tesseract::views::RoomView* rv,
                                   std::function<void()> request_repaint,
                                   std::function<std::string()> get_room_id,
                                   std::function<void()> clear_text_fn)
{
    wire_voice_capture_(rv, std::move(request_repaint), std::move(get_room_id),
                        std::move(clear_text_fn));
}
void MacShell::schedule_relayout() { schedule_relayout_(); }
const std::vector<tesseract::ImagePackImage>& MacShell::cached_emoticons() const
    { return cached_emoticons_; }
std::string MacShell::gif_src_disk_key(const std::string& url) const
    { return gif_src_disk_key_(url); }
const tesseract::ServerInfo& MacShell::server_info_ref() const
    { return server_info_; }
void MacShell::compute_cache_sizes(CacheSizeCallback cb)
    { compute_cache_sizes_(std::move(cb)); }
void MacShell::clear_all_caches(CacheSizeCallback cb)
    { clear_all_caches_(std::move(cb)); }
void MacShell::start_qr_grant_overlay_public()
    { start_qr_grant_overlay(); }
void MacShell::push_space(const std::string& room_id,
                           tesseract::views::RoomListView* rlv)
{
    space_nav_frames_.push_back(SpaceNavFrame::capture(rlv));
    space_stack_.push_back(room_id);
}
void MacShell::pop_space(tesseract::views::RoomListView* rlv)
{
    if (!space_stack_.empty())
        space_stack_.pop_back();
    if (!space_nav_frames_.empty())
    {
        space_nav_frames_.back().restore(rlv);
        space_nav_frames_.pop_back();
    }
}
const std::string& MacShell::verification_flow_id() const
    { return active_verification_flow_id_; }
const std::vector<tesseract::InviteInfo>* MacShell::invites_ptr() const
    { return &invites_; }
bool MacShell::space_stack_empty() const { return space_stack_.empty(); }
const std::string& MacShell::current_space() const
{
    static const std::string empty;
    return space_stack_.empty() ? empty : space_stack_.back();
}
const std::vector<std::string>* MacShell::space_children(const std::string& id) const
{
    auto it = space_children_cache_.find(id);
    return it != space_children_cache_.end() ? &it->second : nullptr;
}
void MacShell::clear_reply_details()
    { reply_details_requested_.clear(); }
void MacShell::drain_pools()
{
    pool_.drain();
    mut_pool_.drain();
}
void MacShell::set_capture(std::unique_ptr<tk::AudioCapture> c)
    { capture_ = std::move(c); }
const tesseract::RoomInfo* MacShell::room_by_id(const std::string& id) const
    { return room_by_id_(id); }

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

    // Settings surface — full-window sibling of mainAppView and _loginView.
    std::unique_ptr<tk::macos::Surface> _settingsSurface;
    tesseract::views::SettingsView*
        _settingsView; // borrowed from _settingsSurface root

    // Native overlay fields positioned via _mainAppSurface->set_on_layout().
    std::unique_ptr<tk::NativeTextField> _roomSearchField;
    std::unique_ptr<tk::NativeTextField> _quickSwitchField;
    std::unique_ptr<tk::NativeTextField> _messageSearchField;
    std::unique_ptr<tk::NativeTextField> _forwardPickerField;
    std::unique_ptr<tk::NativeTextField> _findInRoomField;
    std::unique_ptr<tk::NativeTextArea> _roomTextArea;
    std::unique_ptr<tk::NativeTextArea> _topicTextArea;
    std::unique_ptr<tk::NativeTextField> _roomSettingsNameField;
    bool _roomSettingsNameFieldVisible;
    std::unique_ptr<tk::NativeTextArea> _roomSettingsTopicArea;

    // Emojis & Stickers tab (ImagePackEditorView) — initial-testing wiring.
    std::unique_ptr<tk::NativeTextField> _imagePackNameField;
    bool _imagePackNameFieldVisible;
    std::unique_ptr<tk::NativeTextField> _imagePackShortcodeField;
    bool _imagePackShortcodeFieldVisible;
    std::unique_ptr<tk::NativeTextField> _imagePackRenameField;
    bool _imagePackRenameFieldVisible;
    std::unique_ptr<tk::NativeTextArea> _imagePackPasteCatcher;
    bool _imagePackPasteCatcherVisible;
    std::uint64_t _imagePackNameResetGenSeen;

    // Settings name field — positioned via _settingsSurface->set_on_layout().
    std::unique_ptr<tk::NativeTextField> _settingsNameField;
    // Extended-profile NativeTextField overlays (MSC4133).
    std::unique_ptr<tk::NativeTextField> _settingsPronounsField;
    std::unique_ptr<tk::NativeTextField> _settingsTzField;
    std::unique_ptr<tk::NativeTextField> _settingsBioField;

    // Join Room sheet — NSWindow hosting JoinRoomView, presented as a sheet.
    NSWindow* _joinRoomWindow;
    std::unique_ptr<tk::macos::Surface> _joinRoomSurface;
    tesseract::views::JoinRoomView* _joinRoomView; // borrowed from surface root
    std::unique_ptr<tk::NativeTextField> _joinRoomAliasField;
    uint32_t _joinRoomGen;

    // Borrowed sub-view aliases (set after building _mainAppSurface).
    tesseract::views::RoomListView* _roomListView;      // via _mainApp
    tesseract::views::RoomView* _roomView;              // via _mainApp
    tesseract::views::VerificationBanner* _verifShared; // via _mainApp
    tesseract::views::ImageViewerOverlay* _imgViewer;   // via _mainApp
    tesseract::views::VideoViewerOverlay* _vidViewer;   // via _mainApp
    tesseract::views::RoomMediaView* _roomMediaView;    // via _mainApp

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

    // GIF picker (/gif <query>) — NSPanel hosting a tk::macos::Surface.
    NSPanel* _gifPanel;
    std::unique_ptr<tk::macos::Surface> _gifPopupSurface;
    tesseract::views::GifPopup* _gifPopupWidget; // borrowed from root
    std::unique_ptr<tesseract::views::GifController> _gifController;
    std::unordered_map<std::string, std::unique_ptr<tk::Image>> _gifPreviews;
    std::unordered_set<std::string> _gifPreviewInflight;
    std::unordered_set<std::string> _gifAnimInflight;
    std::shared_ptr<bool> _gifAlive;

    // AppKit chrome.
    LoginView* _loginView;

    // Status bar: container view, sync-state label, in-flight dot.
    NSView*      _statusBarView;
    NSTextField* _statusLabel;
    InflightDotView* _inflightDotView;

    NSTimer* _animTimer;
    NSTimer* _inflightTimer;
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

    // Account manager — owns all AccountSession objects and shared media caches.
    // For the primary window this is a value (owned). For windows spawned via
    // spawn_main_window_(), _sharedAccountManager points to the primary's manager
    // and _accountManager is unused; MacShell is constructed from the pointer.
    tesseract::AccountManager  _accountManager;
    tesseract::AccountManager* _sharedAccountManager; // non-owning

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

- (MacShell*)shell
{
    return _shell.get();
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

    tesseract::AccountManager& mgr =
        _sharedAccountManager ? *_sharedAccountManager : _accountManager;
    _shell = std::make_unique<MacShell>(mgr, self);
    _shell->set_screen_lock(std::make_unique<mac::MacScreenLock>());
    _accountPickerShared = nullptr;
    window.delegate = self;
    // Load saved settings before _buildChrome wires the main app widget.
    tesseract::Settings::instance().load_from_disk(tesseract::config_dir());

    // Apply saved window geometry, or centre the default-sized window.
    {
        const auto& saved = tesseract::Settings::instance().main_window_geometry;
        if (saved.valid)
        {
            // Convert stored top-left coords to Cocoa bottom-left.
            NSArray<NSScreen*>* screens = [NSScreen screens];
            const CGFloat primaryTop =
                screens.count > 0
                    ? ([[screens firstObject] frame].origin.y +
                       [[screens firstObject] frame].size.height)
                    : 768.0;
            NSRect f = NSMakeRect(saved.x,
                                  primaryTop - saved.y - saved.h,
                                  saved.w,
                                  saved.h);
            // Validate with clamp_to_screens_: re-centre if off-screen.
            auto clamped = _shell->clamp_to_screens(saved, 1100, 768);
            if (clamped.valid)
            {
                f = NSMakeRect(clamped.x,
                               primaryTop - clamped.y - clamped.h,
                               clamped.w,
                               clamped.h);
            }
            [window setFrame:f display:NO];
        }
        else
        {
            [window center];
        }
    }

    [self _buildChrome];
    // Apply the loaded theme to all surfaces created by _buildChrome.
    _shell->apply_current_theme();

    // Re-apply when the OS switches light/dark (only in System mode).
    [NSApp addObserver:self
            forKeyPath:@"effectiveAppearance"
               options:NSKeyValueObservingOptionNew
               context:nil];

    return self;
}

// Spawned-window init: shares an existing AccountManager rather than owning one.
// Sets _sharedAccountManager before calling the designated -init so that
// the MacShell constructor receives the shared manager reference.
- (instancetype)_initWithSharedAccountManager:(tesseract::AccountManager*)mgr
{
    _sharedAccountManager = mgr;
    return [self init];
}

// Save main window geometry to Settings (debounced) on resize or move.
// Uses top-left-origin coords: y=0 at the top of the primary display.
- (void)_saveWindowGeometry
{
    if (!_shell) return;
    NSRect f = self.window.frame;
    NSArray<NSScreen*>* screens = [NSScreen screens];
    if (!screens.count) return;
    const CGFloat primaryTop =
        [[screens firstObject] frame].origin.y +
        [[screens firstObject] frame].size.height;
    auto& g = tesseract::Settings::instance().main_window_geometry;
    g.x     = static_cast<int>(f.origin.x);
    g.y     = static_cast<int>(primaryTop - f.origin.y - f.size.height);
    g.w     = static_cast<int>(f.size.width);
    g.h     = static_cast<int>(f.size.height);
    g.valid = (g.w > 0 && g.h > 0);
    _shell->save_settings_debounced();
}

- (void)windowDidEndLiveResize:(NSNotification*)notification
{
    [self _saveWindowGeometry];
}

- (void)windowDidMove:(NSNotification*)notification
{
    [self _saveWindowGeometry];
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
    // Hand this window's account bridge back to the primary, release its
    // dedicated mapping and tray ownership (multi-window), then unregister.
    _shell->on_window_closing();
    _shell->account_manager_.unregister_window(_shell.get());
    if (_shell->account_manager_.window_count() == 0)
    {
        [NSApp terminate:nil];
        return NO; // terminate is async; don't let the window close prematurely
    }
    return YES; // spawned window: allow normal close + dealloc
}

- (void)_buildChrome
{
    NSView* content = self.window.contentView;
    content.wantsLayer = YES;

    // ── Single surface hosting the full main-app widget tree ──────────
    _mainAppSurface = std::make_unique<tk::macos::Surface>(tk::Theme::light());
    // Let the animation timer repaint only the rects where animated images are
    // drawn (see _animTick) instead of the whole surface.
    _mainAppSurface->set_anim_cache(&_shell->account_manager_.anim_cache());
    // Feed pointer / wheel events into the PresenceTracker.
    _mainAppSurface->host().set_on_user_activity(
        [shell = _shell.get()] { if (shell) shell->notify_user_activity(); });

    // 30 s periodic tick for the idle-decay check.
    __weak MainWindowController* weakSelf = self;
    _presenceTickTimer =
        [NSTimer scheduledTimerWithTimeInterval:30.0
                                        repeats:YES
                                          block:^(NSTimer*) {
                                              MainWindowController* s = weakSelf;
                                              if (s && s->_shell)
                                              {
                                                  s->_shell->notify_presence_tick();
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
        _roomMediaView = _mainApp->room_media_view();
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
        _mainApp->on_quick_switch_shortcut = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (s) [s _openQuickSwitch];
        };
        _mainApp->on_message_search_shortcut = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (s) [s _openMessageSearch];
        };
        _mainApp->on_find_in_room_shortcut = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (s) [s _openFindInRoom];
        };
        _mainApp->on_history_back_shortcut = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (s) [s _navigateHistoryBack];
        };
        _mainApp->on_history_forward_shortcut = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (s) [s _navigateHistoryForward];
        };
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
                return;
            const auto items = s->_shell->build_user_menu_items_(
                [weakSelf] { if (auto c = weakSelf) [c _openSettings]; },
                [weakSelf] { if (auto c = weakSelf) [c _beginAddAccount]; },
                [weakSelf] { if (auto c = weakSelf) [c _showQRGrant]; },
                [weakSelf] { if (auto c = weakSelf) [c _logoutActiveAccount]; },
                [] { [NSApp terminate:nil]; });
            NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
            NSMutableArray<_TkMenuAction*>* actions = [NSMutableArray new];
            for (const auto& item : items)
            {
                if (item.label.empty())
                {
                    [menu addItem:[NSMenuItem separatorItem]];
                    continue;
                }
                NSString* title =
                    [NSString stringWithUTF8String:item.label.c_str()];
                _TkMenuAction* act =
                    [[_TkMenuAction alloc] initWithCallback:item.callback];
                [actions addObject:act];
                NSMenuItem* mi = [[NSMenuItem alloc]
                    initWithTitle:title
                           action:@selector(fire:)
                    keyEquivalent:@""];
                [mi setTarget:act];
                [menu addItem:mi];
            }
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
        _shell->wire_main_app_widget(_mainApp);

        _mainApp->room_list_view()->on_room_selected =
            [weakSelf](const std::string& room_id)
        {
            MainWindowController* s = weakSelf;
            if (!s)
            {
                return;
            }
            // A space is not a room: clicking one drills into it rather than
            // opening it as the active room/tab (which would put the space
            // title in the room header).
            for (const auto& r : s->_shell->rooms_)
            {
                if (r.id == room_id && r.is_space)
                {
                    s->_shell->push_space(room_id, s->_roomListView);
                    [s _refreshRoomList];
                    tesseract::ShellBase::SpaceNavFrame::enter(s->_roomListView);
                    return;
                }
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

        _mainApp->room_list_view()->on_join_room_requested = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (s)
            {
                [s _openJoinRoomSheet];
            }
        };
        _mainApp->room_list_view()->on_unjoined_room_selected =
            [weakSelf](const tesseract::RoomSummary& s)
        {
            MainWindowController* self = weakSelf;
            if (!self)
                return;
            if (!s.avatar_url.empty())
                self->_shell->ensure_media_thumbnail(s.avatar_url, 64, 64, false);
            if (self->_mainApp)
            {
                __weak MainWindowController* ws = self;
                self->_mainApp->show_room_preview(
                    s,
                    [ws](const std::string& mxc) -> const tk::Image*
                    {
                        MainWindowController* c = ws;
                        if (!c) return nullptr;
                        return c->_shell->account_manager_
                                   .thumbnail_cache().peek(mxc);
                    });
                self->_shell->request_relayout_();
            }
        };
        if (auto* rp = _mainApp->room_preview())
        {
            rp->on_avatar_needed = [weakSelf](const std::string& mxc)
            {
                MainWindowController* s = weakSelf;
                if (s)
                    s->_shell->ensure_media_thumbnail(mxc, 64, 64, false);
            };
            rp->on_join = [weakSelf, rp](const std::string& room_id)
            {
                MainWindowController* s = weakSelf;
                if (!s)
                    return;
                rp->set_state(tesseract::views::RoomPreviewView::State::Joining);
                s->_shell->join_room_command(room_id);
            };
            rp->on_dismiss = [weakSelf]
            {
                MainWindowController* s = weakSelf;
                if (s && s->_mainApp)
                    s->_mainApp->hide_room_preview();
            };
        }

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
                s->_shell->verification_flow_id());
            s->_shell->client_->start_sas(
                s->_shell->verification_flow_id());
        };
        _mainApp->verif_banner()->on_match = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (!s || !s->_shell->client_)
            {
                return;
            }
            s->_shell->client_->confirm_sas(
                s->_shell->verification_flow_id());
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
                    s->_shell->verification_flow_id());
            }
        };
        _mainApp->verif_banner()->on_cancel = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (s && s->_shell->client_)
            {
                s->_shell->client_->cancel_verification(
                    s->_shell->verification_flow_id());
            }
        };
        _mainApp->verif_banner()->on_dismiss = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (!s)
            {
                return;
            }
            s->_shell->set_verification_banner_dismissed(true);
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
        _shell->wire_main_app_viewers(
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
            if (!s->_shell->client_) return;
            auto req_id = s->_shell->begin_media_req_(0,
                [dest](std::vector<uint8_t> bytes) mutable
                {
                    if (bytes.empty()) return;
                    std::ofstream f(dest, std::ios::binary);
                    f.write(reinterpret_cast<const char*>(bytes.data()),
                            static_cast<std::streamsize>(bytes.size()));
                });
            s->_shell->client_->fetch_source_bytes_async(req_id, source_url);
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
            if (!s->_shell->client_) return;
            auto req_id = s->_shell->begin_media_req_(0,
                [dest](std::vector<uint8_t> bytes) mutable
                {
                    if (bytes.empty()) return;
                    std::ofstream f(dest, std::ios::binary);
                    f.write(reinterpret_cast<const char*>(bytes.data()),
                            static_cast<std::streamsize>(bytes.size()));
                });
            s->_shell->client_->fetch_source_bytes_async(req_id, source_json);
        };

        // RoomView shortcode lookup (avatar/image/preview wired via
        // wire_main_app_widget_).
        _mainApp->room_view()->set_shortcode_provider(
            [weakSelf](const std::string& mxc) -> std::string
            {
                MainWindowController* s = weakSelf;
                return s ? s->_shell->shortcode_for_mxc(mxc) : std::string();
            });
        // Avatar inside received mention pills: resolve user id -> member
        // avatar mxc -> cached image (kicking a fetch on miss; the row
        // repaints when the bytes arrive).
        _mainApp->room_view()->message_list()->set_mention_avatar_provider(
            [weakSelf](const std::string& user_id) -> const tk::Image*
            {
                MainWindowController* s = weakSelf;
                if (!s)
                    return nullptr;
                for (const auto& m : s->_shell->cached_room_members_)
                {
                    if (m.user_id != user_id)
                        continue;
                    if (m.avatar_url.empty())
                        return nullptr;
                    s->_shell->ensure_user_avatar(
                        m.avatar_url,
                        s->_shell->media_group_for_room(
                            s->_shell->current_room_id_));
                    return s->_shell->account_manager_.thumbnail_cache().peek(
                        m.avatar_url);
                }
                return nullptr;
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
                return shell->voice_bytes_or_fetch(
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
                if (!s->_shell->client_) return;
                auto req_id = s->_shell->begin_media_req_(0,
                    [on_ready = std::move(on_ready)](std::vector<uint8_t> bytes) mutable
                    {
                        on_ready(std::move(bytes));
                    });
                s->_shell->client_->fetch_source_bytes_async(req_id, src);
            });
        if (auto player = _mainAppSurface->host().make_audio_player())
        {
            _mainApp->room_view()->set_audio_player(std::move(player));
        }
        _shell->set_capture(_mainAppSurface->host().make_audio_capture());
        {
            __weak MainWindowController* ws = self;
            _shell->wire_voice_capture(
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
            // Build from the composer's mention draft so inline pills become
            // matrix.to links + m.mentions; fall back to the plain body.
            std::vector<tesseract::MentionSeg> draft =
                s->_roomTextArea ? s->_roomTextArea->composer_draft()
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
            if (s->_shell->send_room_message(msg.body, msg.formatted_body))
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
            if (!s || body.empty())
                return;
            s->_shell->send_reply(reply_event_id, body);
            if (s->_roomTextArea)
                s->_roomTextArea->set_text("");
            if (s->_roomView)
                s->_roomView->set_current_text({});
        };
        _mainApp->room_view()->on_send_edit =
            [weakSelf](const std::string& event_id, const std::string& new_body)
        {
            MainWindowController* s = weakSelf;
            if (!s || new_body.empty())
                return;
            s->_shell->send_edit(event_id, new_body);
            if (s->_roomTextArea)
                s->_roomTextArea->set_text("");
            if (s->_roomView)
                s->_roomView->set_current_text({});
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
            if (MainWindowController* s = weakSelf)
                s->_shell->redact_event(event_id);
        };
        _mainApp->room_view()->on_reaction_toggled =
            [weakSelf](const std::string& event_id, const std::string& key,
                       const std::string& source_mxc)
        {
            if (MainWindowController* s = weakSelf)
                s->_shell->send_reaction(event_id, key, source_mxc);
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
            if (auto* ml = s->_mainApp->room_view()->message_list())
                ml->set_hover_locked(true);
            [s showEmojiPickerAtRect:anchor];
        };
        _mainApp->room_view()->on_receipt_needed =
            [weakSelf](const std::string& eid)
        {
            if (MainWindowController* s = weakSelf)
                s->_shell->send_read_receipt(eid);
        };
        _mainApp->room_view()->message_list()->on_tile_needed =
            [weakSelf](int z, int x, int y)
        {
            MainWindowController* s = weakSelf;
            if (s)
            {
                s->_shell->ensure_tile(z, x, y);
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
            s->_shell->ensure_viewer_fullres(src_tok);
            NSView* view = (__bridge NSView*)s->_mainAppSurface->view_handle();
            [view.window makeFirstResponder:view];
        };

        // Avatar click → open the lightbox with the original avatar mxc.
        // Overrides the thumbnail-only wiring from
        // ShellBase::wire_main_app_widget_ so ensure_viewer_fullres_ fetches
        // the full-resolution bytes into viewer_fullres_; the viewer's
        // image_provider prefers that over the resized thumbnail entry.
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
            s->_shell->ensure_viewer_fullres(url);
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
            if (s->_shell->client_)
            {
                std::string src = src_tok;
                auto req_id = s->_shell->begin_media_req_(0,
                    [weakSelf](std::vector<uint8_t> bytes) mutable
                    {
                        MainWindowController* s2 = weakSelf;
                        if (!s2 || !s2->_vidViewer) return;
                        s2->_vidViewer->load_bytes(bytes.data(), bytes.size());
                    });
                s->_shell->client_->fetch_source_bytes_async(req_id, src);
            }
        };

        // Room media gallery cell clicks → the same lightboxes as the main
        // timeline. Per-shell (not wire_main_app_widget_) because opening a
        // lightbox needs to grab native keyboard focus, mirroring
        // room_view()'s on_image_clicked/on_video_clicked above exactly.
        _mainApp->room_media_view()->on_image_clicked =
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
            s->_shell->ensure_viewer_fullres(src_tok);
            NSView* view = (__bridge NSView*)s->_mainAppSurface->view_handle();
            [view.window makeFirstResponder:view];
        };
        _mainApp->room_media_view()->on_video_clicked =
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
            if (s->_shell->client_)
            {
                std::string src = src_tok;
                auto req_id = s->_shell->begin_media_req_(0,
                    [weakSelf](std::vector<uint8_t> bytes) mutable
                    {
                        MainWindowController* s2 = weakSelf;
                        if (!s2 || !s2->_vidViewer) return;
                        s2->_vidViewer->load_bytes(bytes.data(), bytes.size());
                    });
                s->_shell->client_->fetch_source_bytes_async(req_id, src);
            }
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
            if (s->_shell->client_)
            {
                auto req_id = s->_shell->begin_media_req_(0,
                    [dest](std::vector<uint8_t> bytes) mutable
                    {
                        if (bytes.empty()) return;
                        std::ofstream f(dest, std::ios::binary);
                        f.write(reinterpret_cast<const char*>(bytes.data()),
                                static_cast<std::streamsize>(bytes.size()));
                    });
                s->_shell->client_->fetch_source_bytes_async(req_id, url);
            }
        };
        _shell->setup_link_clicked_(_mainApp->room_view());
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
            s->_shell->request_forward_history();
        };
        _mainApp->room_view()->on_return_to_live = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (!s || s->_shell->current_room_id_.empty())
            {
                return;
            }
            s->_shell->return_to_live();
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
            s->_shell->begin_focused_subscription(room, ev);
            dispatch_async(
                dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                    MainWindowController* s2 = weakSelf;
                    if (s2)
                    {
                        s2->_shell->client_->subscribe_room_at(room, ev);
                    }
                });
        };
        _mainApp->room_view()->on_date_jump = [weakSelf](std::uint64_t ts_ms)
        {
            MainWindowController* s = weakSelf;
            if (s)
                s->_shell->handle_date_jump(ts_ms);
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
            [weakSelf](const std::string& body, const std::string& /*formatted*/)
        {
            MainWindowController* s = weakSelf;
            if (s)
            {
                // RoomView has no access to the native text area's mention/
                // emoticon draft, so it always passes an empty `formatted`
                // here — rebuild it the same way on_send does so thread
                // sends keep mentions and MSC2545 custom emoji instead of
                // plain shortcode text.
                std::vector<tesseract::MentionSeg> draft =
                    s->_roomTextArea ? s->_roomTextArea->composer_draft()
                                     : std::vector<tesseract::MentionSeg>{};
                tesseract::MarkdownResult msg =
                    draft.empty() ? tesseract::MarkdownResult{body, ""}
                                  : tesseract::build_mention_message(draft);
                s->_shell->on_thread_send_requested(msg.body,
                                                    msg.formatted_body);
                if (s->_roomTextArea)
                    s->_roomTextArea->set_text("");
                s->_roomView->set_current_text({});
            }
        };
        _mainApp->room_view()->on_thread_send_reply =
            [weakSelf](const std::string& reply_id,
                       const std::string& body,
                       const std::string& /*formatted*/)
        {
            MainWindowController* s = weakSelf;
            if (s)
            {
                std::vector<tesseract::MentionSeg> draft =
                    s->_roomTextArea ? s->_roomTextArea->composer_draft()
                                     : std::vector<tesseract::MentionSeg>{};
                tesseract::MarkdownResult msg =
                    draft.empty() ? tesseract::MarkdownResult{body, ""}
                                  : tesseract::build_mention_message(draft);
                s->_shell->on_thread_send_reply_requested(reply_id, msg.body,
                                                          msg.formatted_body);
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
                        // Only names/avatar_urls are cached — no avatar
                        // bytes are fetched until a mention pill or the
                        // info panel actually needs one
                        // (set_mention_avatar_provider above).
                        s2->_shell->cached_room_members_ = *members_holder;
                        s2->_shell->cached_members_room_ = room_id;
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
        _mainApp->room_view()->on_room_settings_opened =
            [weakSelf](std::string room_id)
        {
            MainWindowController* s = weakSelf;
            if (!s)
                return;
            auto* v = s->_mainApp->room_view()->room_settings_view();
            if (!v)
                return;
            if (!s->_shell->client_)
            {
                v->set_field_permissions(false, false, false);
                v->set_security_field_permissions(false, false, false, false);
                v->set_permissions_field_permissions(false);
                v->set_image_pack_field_permissions(false);
                v->set_own_power_level({});
                s->_shell->seed_room_media_section(room_id);
                s->_shell->seed_image_pack_tab(room_id, v);
                return;
            }
            v->set_field_permissions(
                s->_shell->client_->can_set_room_name(room_id),
                s->_shell->client_->can_set_room_topic(room_id),
                s->_shell->client_->can_set_room_avatar(room_id));
            v->set_security_field_permissions(
                s->_shell->client_->can_set_room_encryption(room_id),
                s->_shell->client_->can_set_room_join_rules(room_id),
                s->_shell->client_->can_set_room_guest_access(room_id),
                s->_shell->client_->can_set_room_history_visibility(room_id));
            v->set_permissions_field_permissions(
                s->_shell->client_->can_set_room_power_levels(room_id));
            v->set_permissions_state(
                s->_shell->client_->room_power_levels(room_id));
            v->set_own_power_level(
                s->_shell->client_->room_own_power_level(room_id));
            s->_shell->seed_room_media_section(room_id);
            s->_shell->fetch_room_security_state(room_id);
            s->_shell->seed_image_pack_tab(room_id, v);
        };
        _mainApp->room_view()->on_room_settings_avatar_upload_requested =
            [weakSelf](std::string room_id)
        {
            MainWindowController* s = weakSelf;
            if (!s)
                return;
            s->_shell->stage_room_settings_avatar_upload(
                room_id, s->_mainApp->room_view()->room_settings_view());
        };
        _mainApp->room_view()->room_settings_view()->on_accept =
            [weakSelf](std::string room_id,
                      tesseract::views::RoomSettingsChanges changes)
        {
            MainWindowController* s = weakSelf;
            if (!s || !s->_shell->client_)
                return;
            auto* c = s->_shell->client_;
            s->_shell->run_async_mut_(
                [weakSelf, c, room_id = std::move(room_id),
                 changes = std::move(changes)]() mutable
                {
                    auto outcome = MacShell::apply_room_settings(c, room_id, changes);
                    auto media_override = changes.media_override;
                    dispatch_async(dispatch_get_main_queue(), ^{
                        MainWindowController* s2 = weakSelf;
                        if (!s2)
                            return;
                        if (auto* v =
                                s2->_mainApp->room_view()->room_settings_view())
                            v->set_commit_result(outcome.ok, outcome.error);
                        if (outcome.ok && media_override)
                            s2->_shell->commit_room_media_preview_override(
                                room_id, media_override->has_override,
                                media_override->mode);
                    });
                });
        };
        _mainApp->room_view()->room_settings_view()->set_image_pack_provider(
            [weakSelf](const std::string& url) -> const tk::Image*
        {
            MainWindowController* s = weakSelf;
            if (!s)
                return nullptr;
            if (const auto* img = s->_shell->account_manager_.image_cache().peek(url))
                return img;
            s->_shell->ensure_media_image(url, 96, 96);
            return nullptr;
        });
        _mainApp->room_view()->room_settings_view()->on_image_pack_images_needed =
            [weakSelf](std::string pack_id)
        {
            MainWindowController* s = weakSelf;
            if (!s)
                return;
            s->_shell->handle_image_pack_images_needed(
                pack_id, s->_mainApp->room_view()->room_settings_view());
        };
        _mainApp->room_view()->room_settings_view()->on_image_pack_pending_image_added =
            [weakSelf](std::uint64_t local_id,
                      const std::vector<std::uint8_t>& bytes,
                      const std::string& mime)
        {
            MainWindowController* s = weakSelf;
            if (!s)
                return;
            s->_shell->handle_image_pack_pending_image_added(
                local_id, bytes, mime, s->_mainApp->room_view()->room_settings_view());
        };
        // Space-root settings (wrench icon on SpaceRootView): the same
        // per-room-id permission gating / accept / avatar-upload plumbing
        // above works unchanged for a space's room id — including image
        // packs, which are ordinary room state so a space can host its own
        // (only the Media tab is skipped, since it has no meaning for a
        // space).
        _mainApp->space_root()->on_settings_opened =
            [weakSelf](std::string room_id)
        {
            MainWindowController* s = weakSelf;
            if (!s)
                return;
            auto* v = s->_mainApp->space_root()->settings_view();
            if (!v)
                return;
            if (!s->_shell->client_)
            {
                v->set_field_permissions(false, false, false);
                v->set_security_field_permissions(false, false, false, false);
                v->set_permissions_field_permissions(false);
                v->set_image_pack_field_permissions(false);
                v->set_own_power_level({});
                s->_shell->seed_image_pack_tab(room_id, v);
                return;
            }
            v->set_field_permissions(
                s->_shell->client_->can_set_room_name(room_id),
                s->_shell->client_->can_set_room_topic(room_id),
                s->_shell->client_->can_set_room_avatar(room_id));
            v->set_security_field_permissions(
                s->_shell->client_->can_set_room_encryption(room_id),
                s->_shell->client_->can_set_room_join_rules(room_id),
                s->_shell->client_->can_set_room_guest_access(room_id),
                s->_shell->client_->can_set_room_history_visibility(room_id));
            v->set_permissions_field_permissions(
                s->_shell->client_->can_set_room_power_levels(room_id));
            v->set_permissions_state(
                s->_shell->client_->room_power_levels(room_id));
            v->set_own_power_level(
                s->_shell->client_->room_own_power_level(room_id));
            s->_shell->fetch_room_security_state(room_id);
            s->_shell->seed_image_pack_tab(room_id, v);
        };
        _mainApp->space_root()->on_settings_avatar_upload_requested =
            [weakSelf](std::string room_id)
        {
            MainWindowController* s = weakSelf;
            if (!s)
                return;
            s->_shell->stage_room_settings_avatar_upload(
                room_id, s->_mainApp->space_root()->settings_view());
        };
        _mainApp->space_root()->settings_view()->on_accept =
            [weakSelf](std::string room_id,
                      tesseract::views::RoomSettingsChanges changes)
        {
            MainWindowController* s = weakSelf;
            if (!s || !s->_shell->client_)
                return;
            auto* c = s->_shell->client_;
            s->_shell->run_async_mut_(
                [weakSelf, c, room_id = std::move(room_id),
                 changes = std::move(changes)]() mutable
                {
                    auto outcome = MacShell::apply_room_settings(c, room_id, changes);
                    auto media_override = changes.media_override;
                    dispatch_async(dispatch_get_main_queue(), ^{
                        MainWindowController* s2 = weakSelf;
                        if (!s2)
                            return;
                        if (auto* v =
                                s2->_mainApp->space_root()->settings_view())
                            v->set_commit_result(outcome.ok, outcome.error);
                        if (outcome.ok && media_override)
                            s2->_shell->commit_room_media_preview_override(
                                room_id, media_override->has_override,
                                media_override->mode);
                    });
                });
        };
        _mainApp->space_root()->settings_view()->set_image_pack_provider(
            [weakSelf](const std::string& url) -> const tk::Image*
        {
            MainWindowController* s = weakSelf;
            if (!s)
                return nullptr;
            if (const auto* img = s->_shell->account_manager_.image_cache().peek(url))
                return img;
            s->_shell->ensure_media_image(url, 96, 96);
            return nullptr;
        });
        _mainApp->space_root()->settings_view()->on_image_pack_images_needed =
            [weakSelf](std::string pack_id)
        {
            MainWindowController* s = weakSelf;
            if (!s)
                return;
            s->_shell->handle_image_pack_images_needed(
                pack_id, s->_mainApp->space_root()->settings_view());
        };
        _mainApp->space_root()->settings_view()->on_image_pack_pending_image_added =
            [weakSelf](std::uint64_t local_id,
                      const std::vector<std::uint8_t>& bytes,
                      const std::string& mime)
        {
            MainWindowController* s = weakSelf;
            if (!s)
                return;
            s->_shell->handle_image_pack_pending_image_added(
                local_id, bytes, mime, s->_mainApp->space_root()->settings_view());
        };
        _shell->setup_dm_callbacks();
        _mainApp->room_view()->on_ignore_user =
            [weakSelf](std::string user_id)
        {
            MainWindowController* s = weakSelf;
            if (!s || !s->_shell->client_)
                return;
            s->_shell->client_->ignore_user_async(std::move(user_id));
        };
        _mainApp->room_view()->set_repaint_requester(
            [weakSelf]
            {
                MainWindowController* s = weakSelf;
                if (s && s->_mainAppSurface)
                {
                    s->_mainAppSurface->host().request_repaint();
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
        _mainApp->space_root()->set_post_delayed(
            [weakSelf](int ms, std::function<void()> fn)
            {
                MainWindowController* s = weakSelf;
                if (s && s->_mainAppSurface)
                {
                    s->_mainAppSurface->host().post_delayed(ms, std::move(fn));
                }
            });
        _mainApp->space_root()->on_layout_changed = [weakSelf]
        {
            MainWindowController* s = weakSelf;
            if (s)
            {
                [s _relayoutChatSurface];
            }
        };
        _mainApp->space_root()->on_copy_to_clipboard = [weakSelf](std::string t)
        {
            MainWindowController* s = weakSelf;
            if (s && s->_mainAppSurface)
                s->_mainAppSurface->host().set_clipboard_text(t);
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
        _roomTextArea->set_font_role(tk::FontRole::Body);
        _roomTextArea->set_placeholder(tk::tr("Message\xe2\x80\xa6"));
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
                    c->_shell->ensure_user_avatar(mxc);
            };
            // Resolve candidate avatars from the shared avatar cache.
            _mentionPopupWidget->set_image_provider(
                [mc](const std::string& mxc) -> const tk::Image*
                {
                    MainWindowController* c = mc;
                    if (!c || !c->_shell)
                        return nullptr;
                    return c->_shell->account_manager_.thumbnail_cache().peek(mxc);
                });
            _mentionController =
                std::make_unique<tesseract::views::MentionController>(
                    _roomTextArea.get(), _shell->client_, _mentionPopupWidget,
                    std::move(hooks));
        }
        {
            // GIF picker (/gif <query>): NSPanel + GifPopup + controller, built
            // eagerly so the controller has its borrowed widget from the start.
            NSRect gf = NSMakeRect(0, 0, 220, 120);
            _gifPanel = [[NSPanel alloc]
                initWithContentRect:gf
                          styleMask:NSWindowStyleMaskNonactivatingPanel |
                                    NSWindowStyleMaskBorderless
                            backing:NSBackingStoreBuffered
                              defer:NO];
            _gifPanel.floatingPanel = YES;
            _gifPanel.hidesOnDeactivate = NO;
            _gifPanel.becomesKeyOnlyIfNeeded = YES;
            _gifPopupSurface =
                std::make_unique<tk::macos::Surface>(_mainAppSurface->theme());
            auto gw = std::make_unique<tesseract::views::GifPopup>();
            _gifPopupWidget = gw.get();
            _gifPopupSurface->set_root(std::move(gw));
            _gifPopupSurface->set_anim_cache(&_shell->account_manager_.anim_cache());
            [_gifPanel setContentView:(__bridge NSView*)
                                          _gifPopupSurface->view_handle()];
            _gifAlive = std::make_shared<bool>(true);

            __weak MainWindowController* gc = self;
            _gifPopupWidget->set_image_provider(
                [gc](const tesseract::GifResult& result) -> const tk::Image*
                {
                    MainWindowController* c = gc;
                    if (!c)
                        return nullptr;
                    MacShell* shell = c->_shell.get();
                    // The strip animates strip_url (WebP/GIF, native decode),
                    // keyed in anim_cache_. Serving a cached frame means animated
                    // content is on screen, so ensure the tick timer runs:
                    // re-shown searches take this path without re-fetching.
                    if (const tk::Image* f =
                            shell->account_manager_.anim_cache().current_frame(
                                result.strip_url))
                    {
                        [c _startAnimTickIfNeeded];
                        return f;
                    }
                    // NOTE: the static-preview fallback is returned at the *end*
                    // of this lambda, AFTER the animated re-fetch is kicked
                    // below. Returning it here would short-circuit re-animation
                    // on a re-shown search whose anim_cache_ entry was evicted
                    // while its static thumbnail lingers in _gifPreviews.
                    // Kick off static preview fetch only when not already cached.
                    if (!c->_gifPreviews.count(result.preview_url) &&
                        c->_gifPreviewInflight.insert(result.preview_url).second)
                    {
                        auto alive = c->_gifAlive;
                        auto url = result.preview_url;
                        shell->run_async_(
                            [gc, url, alive, shell]
                            {
                                // Disk-cache the preview too, symmetrically with
                                // the animated source. Otherwise a GIF whose MP4
                                // is already on disk loads its video faster than
                                // its preview downloads, leaving the cell blank
                                // until the video appears instead of the
                                // thumbnail first.
                                const std::string disk_key =
                                    shell->gif_src_disk_key(url);
                                std::vector<std::uint8_t> bytes =
                                    shell->account_manager_.media_disk_cache().load(disk_key);
                                if (!bytes.empty())
                                {
                                    shell->handle_media_ready_ui_(
                                        shell->begin_media_req_(0,
                                            [gc, url, alive, shell](std::vector<uint8_t> b) mutable
                                            {
                                                if (!*alive) return;
                                                MainWindowController* c2 = gc;
                                                if (!c2) return;
                                                c2->_gifPreviewInflight.erase(url);
                                                if (b.empty()) return;
                                                NSData* d =
                                                    [NSData dataWithBytes:b.data()
                                                                  length:b.size()];
                                                CGImageSourceRef src =
                                                    CGImageSourceCreateWithData(
                                                        (__bridge CFDataRef)d, nullptr);
                                                if (!src) return;
                                                CGImageRef cg =
                                                    CGImageSourceCreateImageAtIndex(
                                                        src, 0, nullptr);
                                                CFRelease(src);
                                                if (!cg) return;
                                                c2->_gifPreviews[url] =
                                                    tk::cg::make_image(cg);
                                                CGImageRelease(cg);
                                                if (c2->_gifPopupSurface)
                                                    c2->_gifPopupSurface->relayout();
                                            }),
                                        bytes);
                                }
                                else if (shell->client_)
                                {
                                    auto req_id = shell->begin_media_req_(0,
                                        [gc, url, alive, shell](std::vector<uint8_t> b) mutable
                                        {
                                            if (!*alive) return;
                                            MainWindowController* c2 = gc;
                                            if (!c2) return;
                                            c2->_gifPreviewInflight.erase(url);
                                            if (b.empty()) return;
                                            if (!b.empty())
                                                shell->account_manager_.media_disk_cache().store(
                                                    shell->gif_src_disk_key(url), b);
                                            NSData* d =
                                                [NSData dataWithBytes:b.data()
                                                              length:b.size()];
                                            CGImageSourceRef src =
                                                CGImageSourceCreateWithData(
                                                    (__bridge CFDataRef)d, nullptr);
                                            if (!src) return;
                                            CGImageRef cg =
                                                CGImageSourceCreateImageAtIndex(
                                                    src, 0, nullptr);
                                            CFRelease(src);
                                            if (!cg) return;
                                            c2->_gifPreviews[url] =
                                                tk::cg::make_image(cg);
                                            CGImageRelease(cg);
                                            if (c2->_gifPopupSurface)
                                                c2->_gifPopupSurface->relayout();
                                        });
                                    shell->client_->fetch_url_async(req_id, 0, url);
                                }
                                else
                                {
                                    shell->post_to_ui_(
                                        [gc, url, alive]()
                                        {
                                            if (!*alive) return;
                                            MainWindowController* c2 = gc;
                                            if (c2) c2->_gifPreviewInflight.erase(url);
                                        });
                                }
                            });
                    }
                    // Kick off the strip-display fetch (strip_url: WebP/GIF). The
                    // MP4 send form is fetched separately at send time.
                    if (c->_gifAnimInflight.insert(result.strip_url).second)
                    {
                        auto alive = c->_gifAlive;
                        auto anim_url = result.strip_url;
                        auto anim_mime = result.strip_mime;
                        // Decode entirely on worker thread; post only results.
                        // If bytes are disk-cached, do all work in run_async_.
                        // If not cached, kick an async URL fetch; the callback
                        // (UI thread) re-dispatches decode work to run_async_.
                        auto do_decode = [gc, anim_url, anim_mime, alive, shell](
                                             std::vector<std::uint8_t> bytes)
                        {
                            using CW = tesseract::views::GifPopup;
                            if (!bytes.empty() &&
                                anim_mime == "video/mp4")
                            {
                                // Decode MP4 frames off the main thread
                                // (AVAssetReader is thread-safe).
                                tk::DecodedVideoFrames dvf =
                                    tk::decode_video_frames(
                                        bytes.data(), bytes.size(),
                                        int(CW::kCellW) * 2,
                                        int(CW::kCellH) * 2);
                                // Convert BGRA → CGImage → tk::Image
                                // (CoreGraphics is thread-safe).
                                // shared_ptr so the lambda is copyable
                                // (required by std::function).
                                auto imgs = std::make_shared<
                                    std::vector<
                                        std::unique_ptr<tk::Image>>>();
                                std::vector<int> delays;
                                for (auto& f : dvf.frames)
                                {
                                    CGColorSpaceRef cs =
                                        CGColorSpaceCreateDeviceRGB();
                                    #pragma clang diagnostic push
                                    #pragma clang diagnostic ignored \
                                        "-Wdeprecated-anon-enum-enum-conversion"
                                    CGContextRef ctx =
                                        CGBitmapContextCreate(
                                            nullptr,
                                            static_cast<size_t>(f.w),
                                            static_cast<size_t>(f.h),
                                            8,
                                            static_cast<size_t>(f.w) * 4,
                                            cs,
                                            kCGBitmapByteOrder32Little |
                                                kCGImageAlphaPremultipliedFirst);
                                    #pragma clang diagnostic pop
                                    CGColorSpaceRelease(cs);
                                    if (ctx)
                                    {
                                        uint8_t* dst =
                                            static_cast<uint8_t*>(
                                                CGBitmapContextGetData(
                                                    ctx));
                                        if (dst)
                                        {
                                            std::memcpy(
                                                dst, f.bgra.data(),
                                                f.bgra.size());
                                        }
                                        CGImageRef cg =
                                            CGBitmapContextCreateImage(
                                                ctx);
                                        CGContextRelease(ctx);
                                        if (cg)
                                        {
                                            imgs->push_back(
                                                tk::cg::make_image(cg));
                                            delays.push_back(
                                                f.delay_ms);
                                            CGImageRelease(cg);
                                        }
                                    }
                                    else
                                    {
                                        CGColorSpaceRelease(cs);
                                    }
                                }
                                shell->post_to_ui_(
                                    [gc, anim_url, imgs,
                                     delays = std::move(delays),
                                     alive, shell]() mutable
                                    {
                                        if (!*alive)
                                            return;
                                        MainWindowController* c2 = gc;
                                        if (!c2)
                                            return;
                                        c2->_gifAnimInflight.erase(
                                            anim_url);
                                        if (!imgs->empty())
                                        {
                                            const std::int64_t now =
                                                static_cast<std::int64_t>(
                                                    [[NSDate date]
                                                        timeIntervalSince1970] *
                                                    1000.0);
                                            shell->account_manager_.anim_cache().store(
                                                anim_url,
                                                std::move(*imgs),
                                                std::move(delays), now);
                                            [c2 _startAnimTickIfNeeded];
                                        }
                                        if (c2->_gifPopupSurface)
                                            c2->_gifPopupSurface
                                                ->relayout();
                                    });
                            }
                            else
                            {
                                auto d = std::make_shared<
                                    MacShell::DecodedImage>(
                                    bytes.empty()
                                        ? MacShell::DecodedImage{}
                                        : shell->decode_image_(
                                              bytes,
                                              int(CW::kCellW) * 2,
                                              int(CW::kCellH) * 2));
                                shell->post_to_ui_(
                                    [gc, anim_url, d, alive, shell]()
                                    {
                                        if (!*alive)
                                            return;
                                        MainWindowController* c2 = gc;
                                        if (!c2)
                                            return;
                                        c2->_gifAnimInflight.erase(
                                            anim_url);
                                        if (!d->frames.empty())
                                        {
                                            const std::int64_t now =
                                                static_cast<std::int64_t>(
                                                    [[NSDate date]
                                                        timeIntervalSince1970] *
                                                    1000.0);
                                            shell->account_manager_.anim_cache().store(
                                                anim_url,
                                                std::move(d->frames),
                                                std::move(d->delays_ms),
                                                now);
                                            [c2 _startAnimTickIfNeeded];
                                        }
                                        else if (d->still)
                                        {
                                            c2->_gifPreviews[anim_url] =
                                                std::move(d->still);
                                        }
                                        if (c2->_gifPopupSurface)
                                            c2->_gifPopupSurface
                                                ->relayout();
                                    });
                            }
                        };
                        shell->run_async_(
                            [gc, anim_url, alive, shell,
                             do_decode = std::move(do_decode)]() mutable
                            {
                                // Source bytes: disk cache first, else kick
                                // async fetch and persist on arrival.
                                const std::string disk_key =
                                    shell->gif_src_disk_key(anim_url);
                                std::vector<std::uint8_t> bytes =
                                    shell->account_manager_.media_disk_cache().load(disk_key);
                                if (!bytes.empty())
                                {
                                    do_decode(std::move(bytes));
                                }
                                else if (shell->client_)
                                {
                                    auto req_id = shell->begin_media_req_(0,
                                        [gc, anim_url, alive, shell,
                                         do_decode = std::move(do_decode)](
                                            std::vector<uint8_t> b) mutable
                                        {
                                            // Callback is on UI thread; dispatch
                                            // the heavy decode to a worker.
                                            if (!b.empty())
                                                shell->account_manager_.media_disk_cache().store(
                                                    shell->gif_src_disk_key(anim_url), b);
                                            auto bptr =
                                                std::make_shared<std::vector<uint8_t>>(
                                                    std::move(b));
                                            shell->run_async_(
                                                [do_decode = std::move(do_decode),
                                                 bptr]() mutable
                                                {
                                                    do_decode(std::move(*bptr));
                                                });
                                        });
                                    shell->client_->fetch_url_async(
                                        req_id, 0, anim_url);
                                }
                                else
                                {
                                    shell->post_to_ui_(
                                        [gc, anim_url, alive]()
                                        {
                                            if (!*alive) return;
                                            MainWindowController* c2 = gc;
                                            if (c2)
                                                c2->_gifAnimInflight.erase(anim_url);
                                        });
                                }
                            });
                    }
                    // Static JPEG preview shown while the animation decodes (or
                    // as the permanent fallback for a non-animated result).
                    if (auto it = c->_gifPreviews.find(result.preview_url);
                        it != c->_gifPreviews.end())
                        return it->second.get();
                    return nullptr;
                });

            tesseract::views::GifController::Hooks gh;
            gh.show = [gc] { if (MainWindowController* c = gc) [c showGifPopup]; };
            gh.hide = [gc] { if (MainWindowController* c = gc) [c hideGifPopup]; };
            gh.repaint = [gc]
            {
                MainWindowController* c = gc;
                if (c && c->_gifPopupSurface && [c gifPopupVisible])
                    c->_gifPopupSurface->relayout();
            };
            gh.room_id = [gc]() -> std::string
            {
                MainWindowController* c = gc;
                return c ? c->_shell->current_room_id_ : std::string{};
            };
            gh.client = [gc]() -> tesseract::Client*
            {
                MainWindowController* c = gc;
                return c ? c->_shell->client_ : nullptr;
            };
            gh.run_async = [gc](std::function<void()> fn)
            {
                if (MainWindowController* c = gc)
                    c->_shell->run_async_(std::move(fn));
            };
            gh.post_to_ui = [gc](std::function<void()> fn)
            {
                if (MainWindowController* c = gc)
                    c->_shell->post_to_ui_(std::move(fn));
            };
            gh.post_delayed = [gc](int ms, std::function<void()> fn)
            {
                if (MainWindowController* c = gc)
                    c->_mainAppSurface->host().post_delayed(ms, std::move(fn));
            };
            gh.api_key = []() -> std::string
            { return tesseract::Settings::instance().gif_api_key; };
            gh.client_key = []() -> std::string { return "tesseract"; };
            gh.clear_composer = [gc]
            {
                MainWindowController* c = gc;
                if (!c)
                    return;
                if (c->_roomTextArea)
                    c->_roomTextArea->set_text("");
                if (c->_shell->room_view_)
                    c->_shell->room_view_->set_current_text({});
            };
            gh.get_cached_gif_bytes = [gc](const std::string& url)
                -> std::vector<std::uint8_t>
            {
                MainWindowController* c = gc;
                if (!c)
                    return {};
                // Reuse the source bytes the strip persisted to disk on fetch.
                return c->_shell->account_manager_.media_disk_cache().load(
                    c->_shell->gif_src_disk_key(url));
            };
            _gifController = std::make_unique<tesseract::views::GifController>(
                _roomTextArea.get(), _gifPopupWidget, std::move(gh));

            // Bridge ShellBase gif callbacks → this controller.
            _shell->gif_on_results_ =
                [gc](std::uint64_t id, std::vector<tesseract::GifResult> r)
            {
                MainWindowController* c = gc;
                if (c && c->_gifController)
                    c->_gifController->on_results(id, std::move(r));
            };
            _shell->gif_on_failed_ = [gc](std::uint64_t id, std::string m)
            {
                MainWindowController* c = gc;
                if (c && c->_gifController)
                    c->_gifController->on_search_failed(id, std::move(m));
            };
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
        // _activeRoomSettingsView resolves to whichever RoomSettingsView
        // instance (_mainApp->room_view()'s or the space-root one) currently
        // has a tab open — a space's General/Emojis & Stickers tabs are
        // backed by a separate RoomSettingsView instance.
        _roomSettingsNameField = _mainAppSurface->host().make_text_field();
        _roomSettingsNameField->set_on_changed(
            [weakSelf](const std::string& t)
            {
                MainWindowController* s = weakSelf;
                if (auto* v = [s _activeRoomSettingsView])
                    v->set_name_edit_text(t);
            });
        _roomSettingsNameField->set_visible(false);
        _roomSettingsTopicArea = _mainAppSurface->host().make_text_area();
        _roomSettingsTopicArea->set_on_changed(
            [weakSelf](const std::string& t)
            {
                MainWindowController* s = weakSelf;
                if (auto* v = [s _activeRoomSettingsView])
                    v->set_topic_edit_text(t);
            });
        _roomSettingsTopicArea->set_on_height_changed(
            [weakSelf](float h)
            {
                MainWindowController* s = weakSelf;
                if (!s)
                    return;
                if (auto* v = [s _activeRoomSettingsView])
                    v->set_topic_area_natural_height(h);
                [s _relayoutChatSurface];
            });
        _roomSettingsTopicArea->set_visible(false);

        _imagePackNameField = _mainAppSurface->host().make_text_field();
        _imagePackNameField->set_placeholder(tk::tr("Pack name"));
        _imagePackNameField->set_on_changed(
            [weakSelf](const std::string& t)
            {
                MainWindowController* s = weakSelf;
                if (auto* v = [s _activeRoomSettingsView])
                    v->set_image_pack_new_pack_name_text(t);
            });
        _imagePackNameField->set_visible(false);

        _imagePackShortcodeField = _mainAppSurface->host().make_text_field();
        _imagePackShortcodeField->set_compact(true);
        _imagePackShortcodeField->set_on_changed(
            [weakSelf](const std::string& t)
            {
                MainWindowController* s = weakSelf;
                if (auto* v = [s _activeRoomSettingsView])
                    v->set_image_pack_editing_shortcode_text(t);
            });
        _imagePackShortcodeField->set_on_submit(
            [weakSelf]()
            {
                MainWindowController* s = weakSelf;
                if (auto* v = [s _activeRoomSettingsView])
                    v->commit_image_pack_editing_shortcode();
            });
        _imagePackShortcodeField->set_on_focus_changed(
            [weakSelf](bool focused)
            {
                MainWindowController* s = weakSelf;
                if (!focused)
                    if (auto* v = [s _activeRoomSettingsView])
                        v->commit_image_pack_editing_shortcode();
            });
        _imagePackShortcodeField->set_visible(false);

        _imagePackRenameField = _mainAppSurface->host().make_text_field();
        _imagePackRenameField->set_compact(true);
        _imagePackRenameField->set_on_changed(
            [weakSelf](const std::string& t)
            {
                MainWindowController* s = weakSelf;
                if (auto* v = [s _activeRoomSettingsView])
                    v->set_image_pack_editing_name_text(t);
            });
        _imagePackRenameField->set_on_submit(
            [weakSelf]()
            {
                MainWindowController* s = weakSelf;
                if (auto* v = [s _activeRoomSettingsView])
                    v->commit_image_pack_editing_name();
            });
        _imagePackRenameField->set_on_focus_changed(
            [weakSelf](bool focused)
            {
                MainWindowController* s = weakSelf;
                if (!focused)
                    if (auto* v = [s _activeRoomSettingsView])
                        v->commit_image_pack_editing_name();
            });
        _imagePackRenameField->set_visible(false);

        _imagePackPasteCatcher = _mainAppSurface->host().make_text_area();
        _imagePackPasteCatcher->set_visible(false);
        _imagePackPasteCatcher->set_on_image_paste(
            [weakSelf](std::vector<std::uint8_t> bytes, std::string mime)
            {
                MainWindowController* s = weakSelf;
                if (auto* v = [s _activeRoomSettingsView])
                    v->add_image_pack_pasted_image(std::move(bytes), std::move(mime));
            });

        _roomTextArea->set_on_changed(
            [weakSelf](const std::string& s)
            {
                MainWindowController* c = weakSelf;
                if (!c)
                {
                    return;
                }
                c->_shell->handle_compose_text_changed(s);
                if (c->_roomView)
                {
                    c->_roomView->set_current_text(s);
                }

                // ── GIF picker (/gif <query>) ──────────────────────────────────
                if (c->_gifController && c->_gifController->on_text_changed(s))
                {
                    if ([c slashPopupVisible])    [c hideSlashPopup];
                    if (c->_mentionController)    c->_mentionController->hide();
                    return;
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
                                    case tk::NativeTextArea::NavKey::Left:
                                    case tk::NativeTextArea::NavKey::Right:
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
                        complete->prefix, c->_shell->cached_emoticons(), 1);
                    if (!hits.empty() && !hits.front().glyph.empty())
                    {
                        c->_roomTextArea->replace_range(
                            complete->start, complete->end, hits.front().glyph);
                    }
                    else if (!hits.empty())
                    {
                        const tk::Image* image =
                            c->_shell->account_manager_.image_cache().peek(
                                hits.front().emoticon.url);
                        c->_roomTextArea->insert_emoticon(
                            complete->start, complete->end, hits.front().shortcode,
                            hits.front().emoticon.url, image);
                    }
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
                            prefix_match->prefix, c->_shell->cached_emoticons());
                    if (!c->_shell->shortcode_current_suggestions_.empty())
                    {
                        [c hideMentionPopup];
                        c->_shell->shortcode_active_match_ = *prefix_match;
                        for (const auto& sugg :
                             c->_shell->shortcode_current_suggestions_)
                        {
                            if (!sugg.emoticon.url.empty())
                            {
                                c->_shell->ensure_media_image(
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
                                    default:
                                        // Left/Right: let the caret move.
                                        return false;
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
                if (c->_gifController && c->_gifController->on_submit())
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
                       std::string filename, tk::Point pos)
            {
                MainWindowController* c = weakSelf;
                if (!c || !c->_roomView)
                {
                    return;
                }
                if (auto* rsv = [c _activeRoomSettingsView];
                    rsv && !rsv->image_pack_list_rect().empty())
                {
                    rsv->add_image_pack_dropped_image(pos, std::move(bytes),
                                                       std::move(mime));
                    return;
                }
                MacShell* shell = c->_shell.get();
                auto outcome = tesseract::views::dispatch_file_drop(
                    *c->_roomView->compose_bar(), std::move(bytes),
                    std::move(mime), std::move(filename),
                    shell->client_->media_upload_limit(),
                    [shell](std::uint32_t gen, std::vector<std::uint8_t> b,
                            std::string m)
                    {
                        shell->extract_drop_media_(gen, std::move(b),
                                                   std::move(m));
                    });
                if (outcome == tesseract::views::FileDropOutcome::TooLarge)
                    shell->show_status_message(tk::tr("File exceeds the upload limit"));
            });
        _mainAppSurface->set_on_file_drop_error(
            [weakSelf](std::string reason)
            {
                MainWindowController* c = weakSelf;
                if (!c || !c->_shell)
                    return;
                c->_shell->show_status_message(std::move(reason));
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
                s->_shell->pending_search_text_ = q;
                s->_shell->debounce_(
                    tesseract::ShellBase::DebounceSlot::RoomSearch,
                    tesseract::views::RoomListView::kSearchDebounceMs,
                    [s] { [s _applySearchFilter]; });
            });

        // Quick switcher (⌘K) search field.
        _quickSwitchField = _mainAppSurface->host().make_text_field();
        _quickSwitchField->set_placeholder(
            tk::tr("Jump to a room, or @user to start a chat\xe2\x80\xa6"));
        _quickSwitchField->set_visible(false);
        _quickSwitchField->set_on_changed(
            [weakSelf](const std::string& q)
            {
                MainWindowController* s = weakSelf;
                if (s && s->_mainApp && s->_mainApp->quick_switcher())
                {
                    s->_mainApp->quick_switcher()->set_query(q);
                    s->_mainAppSurface->relayout();
                }
            });
        _quickSwitchField->set_on_submit(
            [weakSelf]
            {
                MainWindowController* s = weakSelf;
                if (s && s->_mainApp && s->_mainApp->quick_switcher())
                    s->_mainApp->quick_switcher()->activate_selected();
            });
        _quickSwitchField->set_on_popup_nav(
            [weakSelf](tk::NavKey nk) -> bool
            {
                MainWindowController* s = weakSelf;
                auto* qs = (s && s->_mainApp) ? s->_mainApp->quick_switcher()
                                              : nullptr;
                if (!qs || !qs->is_open())
                    return false;
                switch (nk)
                {
                case tk::NavKey::Up:
                    qs->move_selection(-1);
                    s->_mainAppSurface->relayout();
                    return true;
                case tk::NavKey::Down:
                    qs->move_selection(+1);
                    s->_mainAppSurface->relayout();
                    return true;
                case tk::NavKey::Escape:
                    [s _closeQuickSwitch];
                    return true;
                default:
                    return false;
                }
            });
        if (_mainApp->quick_switcher())
            _mainApp->quick_switcher()->on_close = [weakSelf]
            {
                MainWindowController* s = weakSelf;
                if (s)
                    [s _closeQuickSwitch];
            };

        // Message search (⌘⇧F) native field — mirrors the quick switcher.
        _messageSearchField = _mainAppSurface->host().make_text_field();
        _messageSearchField->set_placeholder(tk::tr("Search your messages\xe2\x80\xa6"));
        _messageSearchField->set_visible(false);
        _messageSearchField->set_on_changed(
            [weakSelf](const std::string& q)
            {
                MainWindowController* s = weakSelf;
                if (s && s->_mainApp && s->_mainApp->message_search())
                {
                    s->_mainApp->message_search()->set_query(q);
                    s->_mainAppSurface->relayout();
                }
            });
        _messageSearchField->set_on_submit(
            [weakSelf]
            {
                MainWindowController* s = weakSelf;
                if (s && s->_mainApp && s->_mainApp->message_search())
                    s->_mainApp->message_search()->activate_selected();
            });
        _messageSearchField->set_on_popup_nav(
            [weakSelf](tk::NavKey nk) -> bool
            {
                MainWindowController* s = weakSelf;
                auto* ms = (s && s->_mainApp) ? s->_mainApp->message_search()
                                              : nullptr;
                if (!ms || !ms->is_open())
                    return false;
                switch (nk)
                {
                case tk::NavKey::Up:
                    ms->move_selection(-1);
                    s->_mainAppSurface->relayout();
                    return true;
                case tk::NavKey::Down:
                    ms->move_selection(+1);
                    s->_mainAppSurface->relayout();
                    return true;
                case tk::NavKey::Escape:
                    [s _closeMessageSearch];
                    return true;
                default:
                    return false;
                }
            });
        if (_mainApp->message_search())
            _mainApp->message_search()->on_close = [weakSelf]
            {
                MainWindowController* s = weakSelf;
                if (s)
                    [s _closeMessageSearch];
            };

        // Forward room picker native search field.
        _forwardPickerField = _mainAppSurface->host().make_text_field();
        _forwardPickerField->set_placeholder(tk::tr("Search rooms\xe2\x80\xa6"));
        _forwardPickerField->set_visible(false);
        _forwardPickerField->set_on_changed(
            [weakSelf](const std::string& q)
            {
                MainWindowController* s = weakSelf;
                if (s && s->_mainApp && s->_mainApp->forward_picker())
                {
                    s->_mainApp->forward_picker()->set_query(q);
                    s->_mainAppSurface->relayout();
                }
            });
        _forwardPickerField->set_on_submit(
            [weakSelf]
            {
                MainWindowController* s = weakSelf;
                if (s && s->_mainApp && s->_mainApp->forward_picker())
                    s->_mainApp->forward_picker()->confirm();
            });
        _forwardPickerField->set_on_popup_nav(
            [weakSelf](tk::NavKey nk) -> bool
            {
                MainWindowController* s = weakSelf;
                auto* fp = (s && s->_mainApp) ? s->_mainApp->forward_picker()
                                               : nullptr;
                if (!fp || !fp->is_open())
                    return false;
                switch (nk)
                {
                case tk::NavKey::Up:
                    fp->move_selection(-1);
                    s->_mainAppSurface->relayout();
                    return true;
                case tk::NavKey::Down:
                    fp->move_selection(+1);
                    s->_mainAppSurface->relayout();
                    return true;
                case tk::NavKey::Escape:
                    [s _closeForwardPicker];
                    return true;
                default:
                    return false;
                }
            });
        // Per-room "find in conversation" (⌘F) native field.
        _findInRoomField = _mainAppSurface->host().make_text_field();
        _findInRoomField->set_placeholder(tk::tr("Find in conversation\xe2\x80\xa6"));
        _findInRoomField->set_visible(false);
        _findInRoomField->set_on_changed(
            [weakSelf](const std::string& q)
            {
                MainWindowController* s = weakSelf;
                auto* rv = (s && s->_mainApp) ? s->_mainApp->room_view()
                                              : nullptr;
                if (rv)
                {
                    if (auto* bar = rv->room_search_bar())
                    {
                        bar->set_query(q);
                        s->_mainAppSurface->relayout();
                    }
                }
            });
        _findInRoomField->set_on_popup_nav(
            [weakSelf](tk::NavKey nk) -> bool
            {
                MainWindowController* s = weakSelf;
                auto* rv = (s && s->_mainApp) ? s->_mainApp->room_view()
                                              : nullptr;
                if (!rv || !rv->room_search_open())
                    return false;
                switch (nk)
                {
                case tk::NavKey::Up:
                    if (rv->on_room_search_navigate)
                        rv->on_room_search_navigate(-1);
                    return true;
                case tk::NavKey::Down:
                    if (rv->on_room_search_navigate)
                        rv->on_room_search_navigate(+1);
                    return true;
                case tk::NavKey::Escape:
                    [s _closeFindInRoom];
                    return true;
                default:
                    return false;
                }
            });
        if (auto* rv = _mainApp->room_view())
        {
            if (auto* bar = rv->room_search_bar())
            {
                bar->on_close = [weakSelf]
                {
                    MainWindowController* s = weakSelf;
                    if (s)
                        [s _closeFindInRoom];
                };
            }
        }

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
                                  (s->_vidViewer && s->_vidViewer->is_open()) ||
                                  app->camera_overlay_open();
                if (viewerOpen)
                {
                    s->_roomTextArea->set_visible(false);
                    s->_topicTextArea->set_visible(false);
                    s->_roomSearchField->set_visible(false);
                    if (s->_quickSwitchField)
                        s->_quickSwitchField->set_visible(false);
                    if (s->_shell->enc_passphrase_field_)
                        s->_shell->enc_passphrase_field_->set_visible(false);
                    if (s->_shell->enc_key_field_)
                        s->_shell->enc_key_field_->set_visible(false);
                    (void)surf;
                    return;
                }
                const auto overlays = app->native_overlays();
                auto applyField = [&overlays](
                    tk::NativeOverlayId id,
                    const std::unique_ptr<tk::NativeTextField>& field)
                {
                    if (!field)
                        return;
                    const auto* entry = overlays.find(id);
                    const bool visible = entry && entry->visible;
                    field->set_visible(visible);
                    if (visible)
                        field->set_rect(entry->rect);
                };
                auto applyArea = [&overlays](
                    tk::NativeOverlayId id,
                    const std::unique_ptr<tk::NativeTextArea>& area)
                {
                    if (!area)
                        return;
                    const auto* entry = overlays.find(id);
                    const bool visible = entry && entry->visible;
                    area->set_visible(visible);
                    if (visible)
                        area->set_rect(entry->rect);
                };

                applyArea(tk::NativeOverlayId::ComposeTextArea,
                          s->_roomTextArea);
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
                if (s->_roomSettingsNameField && s->_roomSettingsTopicArea &&
                    [s _activeRoomSettingsView])
                {
                    auto* rsv = [s _activeRoomSettingsView];

                    const tk::Rect nr = rsv->name_field_rect();
                    const bool nameWasVisible = s->_roomSettingsNameFieldVisible;
                    s->_roomSettingsNameFieldVisible = !nr.empty();
                    s->_roomSettingsNameField->set_visible(!nr.empty());
                    if (!nr.empty())
                    {
                        s->_roomSettingsNameField->set_rect(nr);
                        if (!nameWasVisible)
                            s->_roomSettingsNameField->set_text(
                                rsv->name_edit_initial_text());
                    }

                    const tk::Rect tr2 = rsv->topic_edit_rect();
                    const bool topicWasVisible = s->_roomSettingsTopicArea->visible();
                    s->_roomSettingsTopicArea->set_visible(!tr2.empty());
                    if (!tr2.empty())
                    {
                        s->_roomSettingsTopicArea->set_rect(tr2);
                        if (!topicWasVisible)
                            s->_roomSettingsTopicArea->set_text(
                                rsv->topic_edit_initial_text());
                    }
                }
                if (s->_imagePackNameField && s->_imagePackShortcodeField &&
                    s->_imagePackRenameField && s->_imagePackPasteCatcher &&
                    [s _activeRoomSettingsView])
                {
                    auto* rsv = [s _activeRoomSettingsView];

                    const tk::Rect pnr = rsv->image_pack_new_pack_name_field_rect();
                    s->_imagePackNameFieldVisible = !pnr.empty();
                    s->_imagePackNameField->set_visible(!pnr.empty());
                    if (!pnr.empty())
                        s->_imagePackNameField->set_rect(pnr);
                    // The create-row field stays visible continuously, so
                    // there's no visibility-transition edge to hook a
                    // "clear the displayed text" reset off of — diff the
                    // generation counter instead (mirrors the Qt6 shell).
                    const std::uint64_t nameGen =
                        rsv->image_pack_new_pack_name_reset_generation();
                    if (nameGen != s->_imagePackNameResetGenSeen)
                    {
                        s->_imagePackNameResetGenSeen = nameGen;
                        s->_imagePackNameField->set_text("");
                    }

                    const tk::Rect scr = rsv->image_pack_shortcode_edit_rect();
                    const bool shortcodeWasVisible =
                        s->_imagePackShortcodeFieldVisible;
                    s->_imagePackShortcodeFieldVisible = !scr.empty();
                    s->_imagePackShortcodeField->set_visible(!scr.empty());
                    if (!scr.empty())
                    {
                        s->_imagePackShortcodeField->set_rect(scr);
                        if (!shortcodeWasVisible)
                            s->_imagePackShortcodeField->set_focused(true);
                    }

                    const tk::Rect renr = rsv->image_pack_name_edit_rect();
                    const bool renameWasVisible = s->_imagePackRenameFieldVisible;
                    s->_imagePackRenameFieldVisible = !renr.empty();
                    s->_imagePackRenameField->set_visible(!renr.empty());
                    if (!renr.empty())
                    {
                        s->_imagePackRenameField->set_rect(renr);
                        if (!renameWasVisible)
                        {
                            s->_imagePackRenameField->set_text(
                                rsv->image_pack_name_edit_initial_text());
                            s->_imagePackRenameField->set_focused(true);
                        }
                    }

                    const tk::Rect gr = rsv->image_pack_list_rect();
                    const bool pasteCatcherWasVisible =
                        s->_imagePackPasteCatcherVisible;
                    s->_imagePackPasteCatcherVisible = !gr.empty();
                    s->_imagePackPasteCatcher->set_visible(!gr.empty());
                    if (!gr.empty())
                    {
                        s->_imagePackPasteCatcher->set_rect({gr.x, gr.y, 1.0f, 1.0f});
                        if (!pasteCatcherWasVisible)
                            s->_imagePackPasteCatcher->set_focused(true);
                    }
                }
                applyField(tk::NativeOverlayId::RoomSearchField,
                           s->_roomSearchField);
                applyField(tk::NativeOverlayId::QuickSwitchField,
                           s->_quickSwitchField);
                applyField(tk::NativeOverlayId::MessageSearchField,
                           s->_messageSearchField);
                applyField(tk::NativeOverlayId::ForwardPickerField,
                           s->_forwardPickerField);
                applyField(tk::NativeOverlayId::FindInRoomField,
                           s->_findInRoomField);
                applyField(tk::NativeOverlayId::EncryptionPassphraseField,
                           s->_shell->enc_passphrase_field_);
                applyField(tk::NativeOverlayId::EncryptionKeyField,
                           s->_shell->enc_key_field_);
                applyField(tk::NativeOverlayId::QrGrantCheckCodeField,
                           s->_shell->qr_check_code_field_);
                (void)surf;
            });

        // Key monitor: ⌘K opens the quick switcher; Escape closes the
        // topmost overlay (switcher first, then lightboxes).
        _escapeMonitor = [NSEvent
            addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                         handler:^(NSEvent* event) {
                                             MainWindowController* s = weakSelf;
                                             if (!s)
                                                 return event;
                                             // ⌘K → open quick switcher.
                                              if ((event.modifierFlags &
                                                   NSEventModifierFlagCommand) &&
                                                  [event.charactersIgnoringModifiers
                                                      isEqualToString:@"k"])
                                              {
                                                  if (s->_mainApp)
                                                  {
                                                      tk::KeyEvent key{};
                                                      key.key = tk::Key::Character;
                                                      key.text = "k";
                                                      key.meta = true;
                                                      s->_mainApp->dispatch_key_down(key);
                                                  }
                                                  return (NSEvent*)nil;
                                              }
                                             // ⌘⇧F → open global message search.
                                             if ((event.modifierFlags &
                                                  NSEventModifierFlagCommand) &&
                                                 (event.modifierFlags &
                                                  NSEventModifierFlagShift) &&
                                                 [[event.charactersIgnoringModifiers
                                                      lowercaseString]
                                                      isEqualToString:@"f"])
                                              {
                                                  if (s->_mainApp)
                                                  {
                                                      tk::KeyEvent key{};
                                                      key.key = tk::Key::Character;
                                                      key.text = "f";
                                                      key.meta = true;
                                                      key.shift = true;
                                                      s->_mainApp->dispatch_key_down(key);
                                                  }
                                                  return (NSEvent*)nil;
                                              }
                                             // ⌘F → open per-room find in conversation.
                                             if ((event.modifierFlags &
                                                  NSEventModifierFlagCommand) &&
                                                 !(event.modifierFlags &
                                                   NSEventModifierFlagShift) &&
                                                 [[event.charactersIgnoringModifiers
                                                      lowercaseString]
                                                      isEqualToString:@"f"])
                                              {
                                                  if (s->_mainApp)
                                                  {
                                                      tk::KeyEvent key{};
                                                      key.key = tk::Key::Character;
                                                      key.text = "f";
                                                      key.meta = true;
                                                      s->_mainApp->dispatch_key_down(key);
                                                  }
                                                  return (NSEvent*)nil;
                                              }
                                             // ⌘[ → navigate room history back.
                                              if ((event.modifierFlags &
                                                   NSEventModifierFlagCommand) &&
                                                  [event.charactersIgnoringModifiers
                                                      isEqualToString:@"["])
                                              {
                                                  if (s->_mainApp)
                                                  {
                                                      tk::KeyEvent key{};
                                                      key.key = tk::Key::Character;
                                                      key.text = "[";
                                                      key.meta = true;
                                                      s->_mainApp->dispatch_key_down(key);
                                                  }
                                                  return (NSEvent*)nil;
                                              }
                                             // ⌘] → navigate room history forward.
                                              if ((event.modifierFlags &
                                                   NSEventModifierFlagCommand) &&
                                                  [event.charactersIgnoringModifiers
                                                      isEqualToString:@"]"])
                                              {
                                                  if (s->_mainApp)
                                                  {
                                                      tk::KeyEvent key{};
                                                      key.key = tk::Key::Character;
                                                      key.text = "]";
                                                      key.meta = true;
                                                      s->_mainApp->dispatch_key_down(key);
                                                  }
                                                  return (NSEvent*)nil;
                                              }
                                              if (event.keyCode == 53)
                                              { // Escape
                                                  const bool hadQuickSwitch =
                                                      s->_mainApp &&
                                                      s->_mainApp->quick_switcher() &&
                                                      s->_mainApp->quick_switcher()
                                                          ->is_open();
                                                  const bool hadMessageSearch =
                                                      s->_mainApp &&
                                                      s->_mainApp->message_search() &&
                                                      s->_mainApp->message_search()
                                                          ->is_open();
                                                  const bool hadRoomSearch =
                                                      s->_mainApp &&
                                                      s->_mainApp->room_view() &&
                                                      s->_mainApp->room_view()
                                                          ->room_search_open();
                                                  if (s->_mainApp &&
                                                      s->_mainApp
                                                          ->dispatch_key_down(
                                                              {tk::Key::Escape}))
                                                  {
                                                      if (hadQuickSwitch)
                                                          [s _closeQuickSwitch];
                                                      else if (hadMessageSearch)
                                                          [s _closeMessageSearch];
                                                      else if (hadRoomSearch)
                                                          [s _closeFindInRoom];
                                                      else if (s->_mainAppSurface)
                                                          s->_mainAppSurface
                                                              ->relayout();
                                                      if (s->_roomTextArea &&
                                                          !s->_mainApp
                                                               ->compose_text_area_rect()
                                                               .empty())
                                                          s->_roomTextArea
                                                              ->set_focused(true);
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
        _shell->set_stats_settings_view(_settingsView);
        __weak MainWindowController* ws = self;
        _settingsView->on_close = [ws]
        {
            MainWindowController* s = ws;
            if (!s)
            {
                return;
            }
            s->_shell->stop_search_stats_poll();
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
            s->_shell->begin_crypto_identity_reset();
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
                s->_shell->set_theme_preference(pref);
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
        _settingsView->on_group_unread_changed = [ws](bool enabled)
        {
            MainWindowController* s = ws;
            if (!s)
            {
                return;
            }
            tesseract::Settings::instance().group_unread_rooms = enabled;
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
        _settingsView->on_autoscroll_unread_changed = [ws](bool enabled)
        {
            MainWindowController* s = ws;
            if (!s)
            {
                return;
            }
            tesseract::Settings::instance().autoscroll_unread_rooms = enabled;
            tesseract::Settings::instance().save_to_disk(
                tesseract::config_dir());
        };
        _settingsView->on_show_membership_events_changed = [ws](bool enabled)
        {
            MainWindowController* s = ws;
            if (s) s->_shell->handle_show_membership_events_toggle(enabled);
        };
        _settingsView->on_send_presence_changed = [ws](bool enabled)
        {
            MainWindowController* s = ws;
            if (s) s->_shell->handle_send_presence_toggle(enabled);
        };
        _settingsView->on_index_messages_changed = [ws](bool enabled)
        {
            MainWindowController* s = ws;
            if (s) s->_shell->handle_index_messages_toggle(enabled);
        };
#ifdef TESSERACT_GITHUB_REPO
        _settingsView->on_check_for_updates_changed = [ws](bool enabled)
        {
            MainWindowController* s = ws;
            if (s) s->_shell->handle_check_for_updates_toggle(enabled);
        };
#endif
        _settingsView->on_media_previews_changed =
            [ws](tesseract::Settings::MediaPreviews mode)
        {
            MainWindowController* s = ws;
            if (s)
                s->_shell->apply_media_preview_config(
                    mode, tesseract::Settings::instance().invite_avatars);
        };
        _settingsView->on_invite_avatars_changed = [ws](bool enabled)
        {
            MainWindowController* s = ws;
            if (s)
                s->_shell->apply_media_preview_config(
                    tesseract::Settings::instance().media_previews, enabled);
        };
        // Populate capture-device combos in the Media section.
        {
            auto& host = _mainAppSurface->host();
            _settingsView->set_audio_input_devices(host.enumerate_audio_inputs());
            _settingsView->set_audio_output_devices(host.enumerate_audio_outputs());
            _settingsView->set_camera_devices(host.enumerate_cameras());
            _settingsView->set_selected_audio_input(
                tesseract::Settings::instance().audio_input_device_id);
            _settingsView->set_selected_audio_output(
                tesseract::Settings::instance().audio_output_device_id);
            _settingsView->set_selected_camera(
                tesseract::Settings::instance().camera_device_id);
            _settingsView->on_audio_input_changed = [ws](std::string id)
            {
                MainWindowController* s = ws;
                if (!s) return;
                tesseract::Settings::instance().audio_input_device_id =
                    std::move(id);
                tesseract::Settings::instance().save_to_disk(
                    tesseract::config_dir());
            };
            _settingsView->on_audio_output_changed = [ws](std::string id)
            {
                MainWindowController* s = ws;
                if (!s) return;
                tesseract::Settings::instance().audio_output_device_id =
                    std::move(id);
                tesseract::Settings::instance().save_to_disk(
                    tesseract::config_dir());
            };
            _settingsView->on_camera_changed = [ws](std::string id)
            {
                MainWindowController* s = ws;
                if (!s) return;
                tesseract::Settings::instance().camera_device_id =
                    std::move(id);
                tesseract::Settings::instance().save_to_disk(
                    tesseract::config_dir());
            };
        }
        _settingsView->on_tab_changed = [ws] {
            MainWindowController* s = ws;
            if (s) s->_settingsSurface->relayout();
        };
        _settingsView->on_clear_caches = [ws]
        {
            MainWindowController* s = ws;
            if (!s || !s->_shell) return;
            s->_shell->clear_all_caches(
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
    [self _buildJoinRoomSheet];

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
    if (_shell)
    {
        // unregister_window was already called in windowShouldClose: so we
        // do not repeat it here.  broadcast_rebuild_tray_ is still needed to
        // refresh any remaining windows' tray menus after this shell is freed.
        _shell->broadcast_rebuild_tray_();
    }

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
    if (_inflightTimer)
    {
        [_inflightTimer invalidate];
        _inflightTimer = nil;
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
            _shell->apply_current_theme();
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
    // Multi-window: only the primary (non-pinned) window tears down the SHARED
    // accounts' background sync (its destruction == app shutdown). A secondary
    // (pinned) window closing must leave every account syncing for the survivors;
    // it still drains its own per-window pools below.
    if (!_shell->is_pinned_window())
    {
        for (auto& acc : _shell->account_manager_.accounts())
        {
            if (acc->sync_started)
                acc->client->stop_sync();
        }
    }
    _shell->drain_pools();
    ;
}

- (void)openMatrixLink:(NSString*)uri
{
    if (uri && _shell)
    {
        _shell->open_matrix_link([uri UTF8String]);
    }
}

- (void)navigateToUnread
{
    if (_shell)
    {
        _shell->navigate_tray_unread();
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
                             if (auto* f = s->_shell->account_manager_.anim_cache().current_frame(
                                     cache_key))
                             {
                                 [s _startAnimTickIfNeeded];
                                 return f;
                             }
                             {
                                 if (const auto* img =
                                         s->_shell->account_manager_.image_cache().peek(cache_key))
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
            s->_shell->send_reaction(ev, std::string(glyph.UTF8String ?: ""), {});
            if (auto* ml = s->_mainApp->room_view()->message_list())
                ml->set_hover_locked(false);
            [weakPanel close];
            return;
        }
        if (!s->_roomTextArea)
            return;
        s->_roomTextArea->insert_at_cursor(std::string(glyph.UTF8String ?: ""));
        if (s->_roomView)
            s->_roomView->set_current_text(s->_roomTextArea->text());
        s->_roomTextArea->set_focused(true);
    };
    panel.onEmoticonSelect = ^(const tesseract::ImagePackImage& img) {
        MainWindowController* s = weakSelf;
        if (!s || img.url.empty())
            return;
        // Reaction mode (parallel to onSelect above): send an MSC4027
        // custom-image reaction.
        if (!s->_pendingReactionEventId.empty())
        {
            std::string ev = std::move(s->_pendingReactionEventId);
            s->_pendingReactionEventId.clear();
            s->_shell->send_reaction(ev, {}, img.url);
            if (auto* ml = s->_mainApp->room_view()->message_list())
                ml->set_hover_locked(false);
            [weakPanel close];
            return;
        }
        // Compose mode: insert an inline emoticon pill.
        if (!s->_roomTextArea)
            return;
        const tk::Image* image =
            s->_shell->account_manager_.anim_cache().current_frame(img.url);
        if (!image)
            image = s->_shell->account_manager_.image_cache().peek(img.url);
        int pos = s->_roomTextArea->cursor_byte_pos();
        s->_roomTextArea->insert_emoticon(pos, pos, img.shortcode, img.url, image);
        if (s->_roomView)
            s->_roomView->set_current_text(s->_roomTextArea->text());
        s->_roomTextArea->set_focused(true);
    };
    panel.onDismiss = ^{
        MainWindowController* s = weakSelf;
        if (!s)
            return;
        s->_pendingReactionEventId.clear();
        if (auto* ml = s->_mainApp->room_view()->message_list())
            ml->set_hover_locked(false);
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
            if (!s.glyph.empty())
            {
                c->_roomTextArea->replace_range(
                    c->_shell->shortcode_active_match_.start,
                    c->_shell->shortcode_active_match_.end, s.glyph);
            }
            else
            {
                const tk::Image* image =
                    c->_shell->account_manager_.image_cache().peek(s.emoticon.url);
                c->_roomTextArea->insert_emoticon(
                    c->_shell->shortcode_active_match_.start,
                    c->_shell->shortcode_active_match_.end, s.shortcode,
                    s.emoticon.url, image);
            }
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
                return c->_shell->account_manager_.image_cache().peek(url);
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
                // /selfie — open camera overlay instead of sending.
                if (s.name == "selfie")
                {
                    c->_roomTextArea->set_text("");
                    if (c->_roomView)
                        c->_roomView->set_current_text({});
                    if (c->_shell->main_app_)
                    {
#ifdef TESSERACT_CALLS_ENABLED
                        c->_shell->main_app_->is_call_active =
                            [c] { return c->_shell->active_call() != nullptr; };
#endif
                        c->_shell->main_app_->on_selfie_captured =
                            [c](std::vector<std::uint8_t> bgra,
                                std::uint32_t w, std::uint32_t h)
                            {
                                CGColorSpaceRef cs =
                                    CGColorSpaceCreateDeviceRGB();
                                CGDataProviderRef dp =
                                    CGDataProviderCreateWithData(
                                        nullptr, bgra.data(), bgra.size(),
                                        nullptr);
                                CGImageRef cgImg = CGImageCreate(
                                    w, h, 8, 32,
                                    static_cast<std::size_t>(w) * 4, cs,
                                    static_cast<CGBitmapInfo>(
                                        kCGImageAlphaPremultipliedFirst) |
                                        kCGBitmapByteOrder32Little,
                                    dp, nullptr, false,
                                    kCGRenderingIntentDefault);
                                CGDataProviderRelease(dp);
                                CGColorSpaceRelease(cs);
                                NSBitmapImageRep* rep =
                                    cgImg
                                    ? [[NSBitmapImageRep alloc]
                                           initWithCGImage:cgImg]
                                    : nullptr;
                                if (cgImg) CGImageRelease(cgImg);
                                if (rep)
                                {
                                    NSDictionary* props = @{
                                        NSImageCompressionFactor: @0.9f
                                    };
                                    NSData* jpeg = [rep
                                        representationUsingType:NSBitmapImageFileTypeJPEG
                                        properties:props];
                                    if (jpeg && c->_shell->main_app_ &&
                                        c->_shell->main_app_->room_view()->compose_bar())
                                    {
                                        const auto* d = static_cast<const std::uint8_t*>(
                                            jpeg.bytes);
                                        c->_shell->main_app_->room_view()
                                            ->compose_bar()->set_pending_image(
                                                std::vector<std::uint8_t>(d, d + jpeg.length),
                                                "image/jpeg", "selfie.jpg");
                                    }
                                }
                            };
                        c->_shell->main_app_->open_camera_overlay();
                    }
                    return;
                }
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

- (BOOL)gifPopupVisible
{
    return _gifPanel && _gifPanel.isVisible;
}

- (void)repaintGifPopupAnimRegions
{
    if (_gifPopupSurface && [self gifPopupVisible])
        _gifPopupSurface->update_anim_regions();
}

- (void)repaintSettingsAnimRegions
{
    // Settings' "Emojis & Stickers" tab hosts its own top-level surface,
    // separate from _mainAppSurface, so it needs its own animation-tick
    // invalidation or animated stickers there only advance on
    // mouse-move-driven repaints.
    if (_settingsSurface &&
        !((__bridge NSView*)_settingsSurface->view_handle()).hidden)
    {
        _settingsSurface->update_anim_regions();
    }
}

- (void)showGifPopup
{
    if (!_gifPanel || !_mainAppSurface || !_gifPopupWidget || !_roomTextArea)
    {
        return;
    }
    // Full-width strip spanning the compose bar, floating just above it (like
    // the attachment preview band). content_size() drives only the height and
    // the empty/status check; the width comes from the compose bar.
    NSView* hostView = (__bridge NSView*)_mainAppSurface->view_handle();
    const tk::Rect cb = _roomView ? _roomView->compose_bar_rect() : tk::Rect{};
    const tk::Size sz = _gifPopupWidget->content_size(cb.w);
    if (cb.w <= 0.0f || sz.h <= 0.0f)
    {
        [self hideGifPopup];
        return;
    }
    const CGFloat w = cb.w;
    const CGFloat h = sz.h;
    [_gifPanel setContentSize:NSMakeSize(w, h)];
    if (_gifPopupSurface)
    {
        _gifPopupSurface->relayout();
    }

    // Anchor on the compose bar's top-left (surface coords); the panel's bottom
    // sits just above that edge.
    NSPoint localPt = NSMakePoint(cb.x, cb.y);
    NSPoint windowPt = [hostView convertPoint:localPt toView:nil];
    NSPoint screenPt = [hostView.window convertPointToScreen:windowPt];
    NSRect sf = _gifPanel.screen ? _gifPanel.screen.visibleFrame
                                 : [NSScreen mainScreen].visibleFrame;
    CGFloat x = screenPt.x;
    CGFloat y = screenPt.y + 4;
    x = std::clamp(x, sf.origin.x, sf.origin.x + sf.size.width - w);
    y = std::clamp(y, sf.origin.y, sf.origin.y + sf.size.height - h);
    [_gifPanel setFrameOrigin:NSMakePoint(x, y)];
    [_gifPanel orderFront:nil];

    __weak MainWindowController* weakSelf = self;
    _roomTextArea->set_on_popup_nav(
        [weakSelf](tk::NativeTextArea::NavKey nk) -> bool
        {
            MainWindowController* c = weakSelf;
            return c && c->_gifController && c->_gifController->on_nav(nk);
        });
}

- (void)hideGifPopup
{
    [_gifPanel orderOut:nil];
    if (_roomTextArea)
    {
        _roomTextArea->set_on_popup_nav(nullptr);
    }
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
                             if (auto* f = s->_shell->account_manager_.anim_cache().current_frame(
                                     cache_key))
                             {
                                 [s _startAnimTickIfNeeded];
                                 return f;
                             }
                             {
                                 if (const auto* img =
                                         s->_shell->account_manager_.image_cache().peek(cache_key))
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
            s->_shell->send_reaction(ev, std::string(glyph.UTF8String ?: ""), {});
            [weakPanel close];
            return;
        }
        if (!s->_roomTextArea)
            return;
        s->_roomTextArea->insert_at_cursor(std::string(glyph.UTF8String ?: ""));
        if (s->_roomView)
            s->_roomView->set_current_text(s->_roomTextArea->text());
        s->_roomTextArea->set_focused(true);
    };
    panel.onEmoticonSelect = ^(const tesseract::ImagePackImage& img) {
        MainWindowController* s = weakSelf;
        if (!s || img.url.empty())
            return;
        if (!s->_pendingReactionEventId.empty())
        {
            std::string ev = std::move(s->_pendingReactionEventId);
            s->_pendingReactionEventId.clear();
            s->_shell->send_reaction(ev, {}, img.url);
            [weakPanel close];
            return;
        }
        if (!s->_roomTextArea)
            return;
        const tk::Image* image =
            s->_shell->account_manager_.anim_cache().current_frame(img.url);
        if (!image)
            image = s->_shell->account_manager_.image_cache().peek(img.url);
        int pos = s->_roomTextArea->cursor_byte_pos();
        s->_roomTextArea->insert_emoticon(pos, pos, img.shortcode, img.url, image);
        if (s->_roomView)
            s->_roomView->set_current_text(s->_roomTextArea->text());
        s->_roomTextArea->set_focused(true);
    };
    panel.onDismiss = ^{
        MainWindowController* s = weakSelf;
        if (!s)
            return;
        s->_pendingReactionEventId.clear();
        if (auto* ml = s->_mainApp->room_view()->message_list())
            ml->set_hover_locked(false);
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

    // Secondary (spawned) window: the shared AccountManager is already populated
    // and syncing, and set_initial_account() pinned the account to display. Bind
    // the UI to it without touching disk, restoring, or re-adding accounts.
    if (_shell->is_secondary_window_startup())
    {
        [self _switchActiveAccount:_shell->active_account_->user_id];
        _shell->ensure_settings_controller();
        return;
    }

    // Migrate + restore every stored account (shared loop in ShellBase). macOS
    // has no in-app notifier or UnifiedPush connector, so install_account_*
    // are no-ops on MacShell.
    auto restore = _shell->restore_all_accounts();

    std::string restore_error = restore.restore_error;
    bool any_restore_failed = restore.any_restore_failed;

    if (!restore.any_accounts)
    {
        _shell->pending_login_temp_dir_.clear();
        _shell->pending_login_client_ = std::make_unique<tesseract::Client>();
        [_loginView setClient:_shell->pending_login_client_.get()];
        __weak MainWindowController* weakSelf = self;
        _loginView.onBeginOAuth = ^{
            MainWindowController* s = weakSelf;
            if (!s)
            {
                return;
            }
            s->_shell->arm_pending_login_();
        };
        [_loginView setMode:tesseract::views::LoginView::Mode::Initial];
        [_loginView reset];
        // _mainAppSurface is already hidden from _buildChrome; login overlay is shown.
        _loginView.hidden = NO;
        if (any_restore_failed)
        {
            NSString* body = [NSString stringWithUTF8String:restore_error.c_str()];
            __weak MainWindowController* weakSelf = self;
            [_loginView showRestoreError:body
                           retryCallback:^{ [weakSelf beginLogin]; }];
        }
        return;
    }

    [self _switchActiveAccount:restore.active_uid];
    _shell->ensure_settings_controller();
}

// Native (AppKit) binding for settings_controller_. Invoked from
// MacShell::bind_settings_controller_ at the tail of
// ShellBase::ensure_settings_controller_, which constructs the controller with
// the three standard callbacks. This method installs the macOS dialog hooks and
// wires the settings view + name field.
- (void)_bindSettingsControllerNative
{
    __weak MainWindowController* ws = self;

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
    _settingsNameField->set_text(_shell->display_name());
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
            s->_shell->set_avatar_url(mxc);
            if (s->_shell->active_account_)
            {
                s->_shell->active_account_->avatar_url =
                    s->_shell->avatar_url();
            }
            s->_settingsView->set_avatar_url(mxc);
            if (s->_settingsSurface) s->_settingsSurface->relayout();
            [s _populateUserStrip];
        };

        _settingsView->set_user_pack_image_provider(
            [ws](const std::string& url) -> const tk::Image*
        {
            MainWindowController* s = ws;
            if (!s)
                return nullptr;
            if (const auto* img = s->_shell->account_manager_.image_cache().peek(url))
                return img;
            s->_shell->ensure_media_image(url, 96, 96);
            return nullptr;
        });
        _settingsView->on_user_pack_pending_image_added =
            [ws](std::uint64_t local_id, const std::vector<std::uint8_t>& bytes,
                const std::string& mime)
        {
            MainWindowController* s = ws;
            if (!s) return;
            s->_shell->handle_user_pack_pending_image_added(
                local_id, bytes, mime, s->_settingsView->user_pack_editor());
        };
    }

    // Captured by value in each on_submit lambda; ws is __weak so safe across
    // surface teardown.
    auto& host = _settingsSurface->host();

    _settingsPronounsField = host.make_text_field();
    _settingsPronounsField->set_placeholder(tk::tr("Pronouns"));
    _settingsPronounsField->set_visible(false);
    _settingsPronounsField->set_on_submit(
        [ws]
        {
            MainWindowController* s = ws;
            if (!s) return;
            const std::string text = s->_settingsPronounsField->text();
            static constexpr char kKey[] = "io.fsky.nyx.pronouns";
            std::string value_json;
            if (text.empty())
                value_json = "null";
            else
                value_json = "[{\"summary\":" + json_quote(text) + ",\"language\":\"en\"}]";
            if (s->_settingsView)
                s->_settingsView->set_profile_field_busy(kKey, true);
            s->_shell->handle_profile_field_change(kKey, value_json);
            if (s->_settingsSurface) s->_settingsSurface->relayout();
        });

    _settingsTzField = host.make_text_field();
    _settingsTzField->set_placeholder(tk::tr("Timezone (e.g. Europe/London)"));
    _settingsTzField->set_visible(false);
    _settingsTzField->set_on_submit(
        [ws]
        {
            MainWindowController* s = ws;
            if (!s) return;
            const std::string text = s->_settingsTzField->text();
            static constexpr char kKey[] = "us.cloke.msc4175.tz";
            std::string value_json = text.empty() ? "null" : json_quote(text);
            if (s->_settingsView)
                s->_settingsView->set_profile_field_busy(kKey, true);
            s->_shell->handle_profile_field_change(kKey, value_json);
            if (s->_settingsSurface) s->_settingsSurface->relayout();
        });

    _settingsBioField = host.make_text_field();
    _settingsBioField->set_placeholder(tk::tr("Short biography"));
    _settingsBioField->set_visible(false);
    _settingsBioField->set_on_submit(
        [ws]
        {
            MainWindowController* s = ws;
            if (!s) return;
            const std::string text = s->_settingsBioField->text();
            static constexpr char kKey[] = "gay.fomx.biography";
            std::string value_json;
            if (text.empty())
                value_json = "null";
            else
                value_json = "{\"m.text\":[{\"body\":" + json_quote(text) + "}]}";
            if (s->_settingsView)
                s->_settingsView->set_profile_field_busy(kKey, true);
            s->_shell->handle_profile_field_change(kKey, value_json);
            if (s->_settingsSurface) s->_settingsSurface->relayout();
        });

    _settingsSurface->set_on_layout(
        [ws]
        {
            MainWindowController* s = ws;
            if (!s || !s->_settingsView) return;
            if (s->_settingsNameField)
            {
                const tk::Rect r = s->_settingsView->name_field_rect();
                s->_settingsNameField->set_visible(!r.empty());
                if (!r.empty())
                    s->_settingsNameField->set_rect(r);
            }
            if (s->_settingsPronounsField)
            {
                const tk::Rect r = s->_settingsView->pronouns_field_rect();
                s->_settingsPronounsField->set_visible(!r.empty());
                if (!r.empty())
                    s->_settingsPronounsField->set_rect(r);
            }
            if (s->_settingsTzField)
            {
                const tk::Rect r = s->_settingsView->tz_field_rect();
                s->_settingsTzField->set_visible(!r.empty());
                if (!r.empty())
                    s->_settingsTzField->set_rect(r);
            }
            if (s->_settingsBioField)
            {
                const tk::Rect r = s->_settingsView->bio_field_rect();
                s->_settingsBioField->set_visible(!r.empty());
                if (!r.empty())
                    s->_settingsBioField->set_rect(r);
            }
        });
}

- (void)loginViewDidSucceed:(LoginView*)view
{
    if (!_shell->pending_login_client_)
    {
        return;
    }

    // The LoginView holds a raw alias to pending_login_client_; clear it before
    // finalize_login_ resets the client.
    [_loginView setClient:nullptr];

    // Agnostic add-account core (see ShellBase::finalize_login_).
    const auto fin = _shell->finalize_login();

    // Reject if this account is already signed in.
    if (fin.rejected_duplicate)
    {
        _shell->pending_login_is_add_account_ = false;
        const auto& accs = _shell->account_manager_.accounts();
        int returnIdx = _shell->add_account_return_idx_;
        _shell->add_account_return_idx_ = -1;
        if (returnIdx >= 0 && returnIdx < (int)accs.size())
        {
            [self _switchActiveAccount:accs[returnIdx]->user_id];
        }
        return;
    }

    if (!fin.ok)
    {
        return;
    }

    _shell->pending_login_is_add_account_ = false;
    _shell->add_account_return_idx_ = -1;

    [self _switchActiveAccount:fin.user_id];
    _shell->ensure_settings_controller();
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
    const auto& accs = _shell->account_manager_.accounts();
    int returnIdx = _shell->add_account_return_idx_;
    _shell->add_account_return_idx_ = -1;
    if (returnIdx >= 0 && returnIdx < (int)accs.size())
    {
        [self _switchActiveAccount:accs[returnIdx]->user_id];
        // The account may already be active (cancelled before completing OAuth),
        // so switch_account() returns false and _refreshAccountUIAfterSwitch is
        // skipped. Always hide the login view explicitly.
        [self _refreshAccountUIAfterSwitch];
    }
}

- (void)_switchActiveAccount:(const std::string&)user_id
{
    // Platform-agnostic bookkeeping (unsubscribe previous room, clear
    // per-account state, swap active_account_ / aliases / identity, compute
    // pending restores, swap rooms_/invites_ snapshots, persist the index)
    // lives in ShellBase. Returns false (no-op) when the account isn't found
    // or is already active with a bound client.
    if (!_shell->switch_account(user_id))
    {
        return;
    }
    [self _refreshAccountUIAfterSwitch];
}

- (void)_refreshAccountUIAfterSwitch
{
    [self _refreshRoomList];

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

    if (_mainApp)
    {
        _mainApp->show_verif_banner(false);
        _mainAppSurface->relayout();
    }

    _shell->handle_verification_state_ui_(
        _shell->active_account_ && !_shell->active_account_->unverified);

    // Exactly one window owns the single app-wide tray icon (multi-window).
    if (!_tray && _shell->account_manager_.claim_tray_owner(_shell.get()))
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
                    // If the unread room is popped out, raise that window
                    // instead of toggling/raising the main window.
                    if (strong->_shell->focus_tray_unread_popout())
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
                        strong->_shell->navigate_tray_unread();
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
            _tray->set_unread(_shell->tray_unread(), _shell->tray_highlight());
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
    // Record the current active account's position in the accounts list so
    // Cancel can return to it. -1 means no active account.
    const auto& accs = _shell->account_manager_.accounts();
    _shell->add_account_return_idx_ = -1;
    if (_shell->active_account_)
    {
        for (int i = 0; i < static_cast<int>(accs.size()); ++i)
        {
            if (accs[i]->user_id == _shell->active_account_->user_id)
            {
                _shell->add_account_return_idx_ = i;
                break;
            }
        }
    }
    _shell->pending_login_is_add_account_ = true;
    _shell->pending_login_temp_dir_.clear();
    _shell->pending_login_client_ = std::make_unique<tesseract::Client>();
    [_loginView setClient:_shell->pending_login_client_.get()];
    __weak MainWindowController* weakSelf = self;
    _loginView.onBeginOAuth = ^{
        MainWindowController* s = weakSelf;
        if (!s)
        {
            return;
        }
        s->_shell->arm_pending_login_();
    };
    [_loginView setMode:tesseract::views::LoginView::Mode::AddAccount];
    [_loginView reset];
    ((__bridge NSView*)_mainAppSurface->view_handle()).hidden = YES;
    _loginView.hidden = NO;
}

- (void)_buildJoinRoomSheet
{
    NSRect frame = NSMakeRect(0, 0,
                              tesseract::views::JoinRoomView::kPreferredW,
                              tesseract::views::JoinRoomView::kPreferredH);
    _joinRoomWindow =
        [[NSWindow alloc] initWithContentRect:frame
                                    styleMask:NSWindowStyleMaskTitled |
                                              NSWindowStyleMaskClosable
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    _joinRoomWindow.title = TkTr("Join a Room");
    _joinRoomWindow.releasedWhenClosed = NO;

    _joinRoomSurface = std::make_unique<tk::macos::Surface>(tk::Theme::light());
    auto jrv = std::make_unique<tesseract::views::JoinRoomView>();
    _joinRoomView = jrv.get();
    _joinRoomGen = 0;

    __weak MainWindowController* ws = self;

    _joinRoomView->set_avatar_provider(
        [ws](const std::string& mxc) -> const tk::Image*
        {
            MainWindowController* s = ws;
            if (!s)
                return nullptr;
            return s->_shell->account_manager_.thumbnail_cache().peek(mxc);
        });

    _joinRoomView->on_lookup_requested =
        [ws](const std::string& alias)
    {
        MainWindowController* s = ws;
        if (!s || !s->_shell->client_ || alias.empty())
            return;
        s->_joinRoomView->set_state(
            tesseract::views::JoinRoomView::State::Loading);
        if (s->_joinRoomSurface)
            s->_joinRoomSurface->relayout();
        uint32_t gen = ++s->_joinRoomGen;
        auto snap = s->_shell->client_;
        s->_shell->run_async_(
            [ws, alias, gen, snap]
            {
                tesseract::RoomSummary summary = snap->get_room_summary(alias);
                MainWindowController* ctrl = ws;
                if (!ctrl)
                    return;
                ctrl->_shell->post_to_ui_(
                    [ws, summary = std::move(summary), gen]
                    {
                        MainWindowController* s = ws;
                        if (!s || !s->_joinRoomView || s->_joinRoomGen != gen)
                            return;
                        if (summary.ok())
                            s->_joinRoomView->set_preview(summary);
                        else
                            s->_joinRoomView->set_error("Room not found.");
                        if (s->_joinRoomSurface)
                            s->_joinRoomSurface->relayout();
                    });
            });
    };

    _joinRoomView->on_join_requested =
        [ws](const std::string& room_id_or_alias)
    {
        MainWindowController* s = ws;
        if (!s || !s->_shell->client_ || room_id_or_alias.empty())
            return;
        s->_joinRoomView->set_state(
            tesseract::views::JoinRoomView::State::Joining);
        if (s->_joinRoomSurface)
            s->_joinRoomSurface->relayout();
        uint32_t gen = ++s->_joinRoomGen;
        auto snap = s->_shell->client_;
        s->_shell->run_async_(
            [ws, room_id_or_alias, gen, snap]
            {
                std::string canonical_id = snap->join_room(room_id_or_alias);
                MainWindowController* ctrl = ws;
                if (!ctrl)
                    return;
                ctrl->_shell->post_to_ui_(
                    [ws, canonical_id, gen]
                    {
                        MainWindowController* s = ws;
                        if (!s || !s->_joinRoomView || s->_joinRoomGen != gen)
                            return;
                        if (!canonical_id.empty())
                        {
                            if (s->_joinRoomWindow)
                                [s.window endSheet:s->_joinRoomWindow];
                            s->_shell->tab_navigate_room(canonical_id);
                        }
                        else
                        {
                            s->_joinRoomView->set_error("Join failed.");
                            if (s->_joinRoomSurface)
                                s->_joinRoomSurface->relayout();
                        }
                    });
            });
    };

    _joinRoomView->on_cancel = [ws]
    {
        MainWindowController* s = ws;
        if (s && s->_joinRoomWindow)
            [s.window endSheet:s->_joinRoomWindow];
    };

    _joinRoomView->on_link_clicked = [ws](std::string url)
    {
        MainWindowController* s = ws;
        if (!s) return;
        if (tesseract::Client::parse_matrix_link(url).kind !=
            tesseract::Client::MatrixLink::Kind::Unknown)
            s->_shell->open_matrix_link(url);
        else
            tesseract::Client::open_in_browser(url);
    };
    {
        auto hovered = std::make_shared<bool>(false);
        _joinRoomView->on_link_hovered = [hovered, ws](std::string url)
        {
            MainWindowController* s = ws;
            if (!s || !s->_joinRoomSurface) return;
            if (!url.empty() && !*hovered)
            {
                *hovered = true;
                NSView* jrv = (__bridge NSView*)s->_joinRoomSurface->view_handle();
                [jrv addCursorRect:jrv.bounds cursor:[NSCursor pointingHandCursor]];
                [[NSCursor pointingHandCursor] push];
            }
            else if (url.empty() && *hovered)
            {
                *hovered = false;
                [NSCursor pop];
            }
        };
    }

    _joinRoomSurface->set_root(std::move(jrv));
    _joinRoomSurface->set_theme(tk::Theme::light());

    _joinRoomAliasField = _joinRoomSurface->host().make_text_field();
    _joinRoomAliasField->set_placeholder("#room:server.org");
    _joinRoomAliasField->set_on_changed(
        [ws](const std::string& text)
        {
            MainWindowController* s = ws;
            if (s && s->_joinRoomView)
                s->_joinRoomView->set_alias_text(text);
        });
    _joinRoomSurface->set_on_layout(
        [ws]
        {
            MainWindowController* s = ws;
            if (!s || !s->_joinRoomView || !s->_joinRoomAliasField)
                return;
            s->_joinRoomAliasField->set_rect(
                s->_joinRoomView->alias_field_rect());
            s->_joinRoomAliasField->set_visible(
                s->_joinRoomView->alias_field_visible());
        });

    NSView* surfaceView =
        (__bridge NSView*)_joinRoomSurface->view_handle();
    [_joinRoomWindow setContentView:surfaceView];
}

- (void)_openJoinRoomSheet
{
    [self _openJoinRoomSheetWithPrefill:nil];
}

- (void)_openJoinRoomSheetWithPrefill:(NSString*)prefill
{
    if (!_joinRoomWindow)
        return;
    ++_joinRoomGen;
    if (_joinRoomView)
    {
        _joinRoomView->set_state(tesseract::views::JoinRoomView::State::Idle);
        _joinRoomView->set_alias_text("");
    }
    if (_joinRoomAliasField)
        _joinRoomAliasField->set_text("");
    if (_joinRoomSurface)
        _joinRoomSurface->relayout();

    [self.window beginSheet:_joinRoomWindow completionHandler:nil];

    if (prefill.length > 0)
    {
        std::string s([prefill UTF8String]);
        if (_joinRoomView)
            _joinRoomView->set_alias_text(s);
        if (_joinRoomAliasField)
            _joinRoomAliasField->set_text(s);
    }

    if (_joinRoomAliasField)
        _joinRoomAliasField->set_focused(true);
}

- (void)_openSettings
{
    if (!_settingsView || !_settingsSurface)
    {
        return;
    }
    __weak MainWindowController* ws = self;
    _settingsView->set_account_info(
        _shell->display_name(), _shell->user_id(), _shell->avatar_url());
    _settingsView->set_image_provider(
        [ws](const std::string& mxc) -> const tk::Image*
        {
            MainWindowController* s = ws;
            if (!s)
            {
                return nullptr;
            }
            return s->_shell->account_manager_.thumbnail_cache().peek(mxc);
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
    _settingsView->set_group_unread_pref(
        tesseract::Settings::instance().group_unread_rooms);
    _settingsView->set_inactive_period_pref(
        tesseract::Settings::instance().inactive_room_threshold_days);
    _settingsView->set_autoscroll_unread_pref(
        tesseract::Settings::instance().autoscroll_unread_rooms);
    _settingsView->set_show_membership_events_pref(
        tesseract::Settings::instance().show_room_join_leave_events);
    _settingsView->set_send_presence_pref(
        tesseract::Settings::instance().send_presence);
    _settingsView->set_index_messages_pref(
        tesseract::Settings::instance().index_messages_for_search);
#ifdef TESSERACT_GITHUB_REPO
    _settingsView->set_check_for_updates_pref(
        tesseract::Settings::instance().check_for_updates);
#endif
    _settingsView->set_media_previews_pref(
        tesseract::Settings::instance().media_previews);
    _settingsView->set_invite_avatars_pref(
        tesseract::Settings::instance().invite_avatars);
    _settingsSurface->relayout();

    _shell->compute_cache_sizes(
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
    _shell->start_search_stats_poll();
}

- (void)_populateUserStrip
{
    if (!_mainApp)
    {
        return;
    }
    _mainApp->user_info()->set_display_name(_shell->display_name());
    _mainApp->user_info()->set_user_id(_shell->user_id());
    _mainApp->user_info()->set_avatar_url(_shell->avatar_url());
    if (!_shell->avatar_url().empty())
    {
        _shell->ensure_user_avatar(_shell->avatar_url());
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
        _roomListView->set_search_text(_shell->pending_search_text_);
    }
}

- (void)_navigateHistoryBack
{
    if (_shell)
        _shell->navigate_history_back();
}

- (void)_navigateHistoryForward
{
    if (_shell)
        _shell->navigate_history_forward();
}

- (void)_openQuickSwitch
{
    if (!_mainApp || !_mainApp->quick_switcher())
        return;
    // The ⌘K monitor is application-local, so it fires while a pop-out room
    // window is key. Bring the main window forward (the switcher lives here)
    // so it is visible and its search field can take focus.
    if (![self.window isKeyWindow])
        [self.window makeKeyAndOrderFront:nil];
    _mainApp->show_quick_switch(true);
    if (_mainAppSurface)
        _mainAppSurface->relayout();
    if (_quickSwitchField)
    {
        _quickSwitchField->set_text("");
        _quickSwitchField->set_focused(true);
    }
}

- (void)_closeQuickSwitch
{
    if (_mainApp)
        _mainApp->show_quick_switch(false);
    if (_quickSwitchField)
        _quickSwitchField->set_visible(false);
    if (_mainAppSurface)
        _mainAppSurface->relayout();
}

- (void)_openMessageSearch
{
    if (!_mainApp || !_mainApp->message_search())
        return;
    // The ⌘⇧F monitor is application-local, so it can fire while a pop-out
    // room window is key. Bring the main window forward (search lives here).
    if (![self.window isKeyWindow])
        [self.window makeKeyAndOrderFront:nil];
    _mainApp->show_message_search(true);
    if (_mainAppSurface)
        _mainAppSurface->relayout();
    if (_messageSearchField)
    {
        _messageSearchField->set_text("");
        _messageSearchField->set_focused(true);
    }
}

- (void)_closeMessageSearch
{
    if (_mainApp)
        _mainApp->show_message_search(false);
    if (_messageSearchField)
        _messageSearchField->set_visible(false);
    if (_mainAppSurface)
        _mainAppSurface->relayout();
}

- (void)_closeForwardPicker
{
    if (_mainApp && _mainApp->forward_picker())
        _mainApp->forward_picker()->close();
}

- (void)_focusForwardPickerField
{
    if (_forwardPickerField)
    {
        _forwardPickerField->set_text("");
        _forwardPickerField->set_focused(true);
    }
}

- (void)_hideForwardPickerField
{
    if (_forwardPickerField)
        _forwardPickerField->set_visible(false);
}

- (void)_openFindInRoom
{
    auto* rv = _mainApp ? _mainApp->room_view() : nullptr;
    if (!rv || !rv->has_room())
        return;
    if (![self.window isKeyWindow])
        [self.window makeKeyAndOrderFront:nil];
    rv->open_room_search();
    if (_mainAppSurface)
        _mainAppSurface->relayout();
    if (_findInRoomField)
    {
        _findInRoomField->set_text("");
        _findInRoomField->set_focused(true);
    }
}

- (void)_closeFindInRoom
{
    auto* rv = _mainApp ? _mainApp->room_view() : nullptr;
    if (rv)
        rv->close_room_search();
    if (_findInRoomField)
        _findInRoomField->set_visible(false);
    if (_mainAppSurface)
        _mainAppSurface->relayout();
}

- (void)_onRoomScrollDebounce
{
    if (!_roomListView || !_shell->active_account_)
    {
        return;
    }
    auto ids  = _roomListView->visible_room_ids();
    auto sess = _shell->active_account_;
    _shell->run_async_mut_([sess, ids = std::move(ids)]() mutable
    {
        if (sess && sess->client)
        {
            sess->client->stop_background_backfill();
            sess->client->start_background_backfill(ids);
        }
    });
}

- (void)_onSpaceBack
{
    _shell->pop_space(_roomListView);
    if (_mainApp)
        _mainApp->hide_room_preview();
    if (_mainApp)
        _mainApp->hide_space_root();
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

        _accountPickerSurface = std::make_unique<tk::macos::Surface>(
            _mainAppSurface ? _mainAppSurface->theme() : tk::Theme::light());
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
            [s->_accountPickerPopover close];
            s->_shell->on_account_picker_select(uid);
        };
        _accountPickerShared->set_image_provider(
            [weakSelf](const std::string& mxc) -> const tk::Image*
            {
                MainWindowController* s = weakSelf;
                if (!s)
                {
                    return nullptr;
                }
                return s->_shell->account_manager_.thumbnail_cache().peek(mxc);
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
    for (const auto& s : _shell->account_manager_.accounts())
    {
        tesseract::views::AccountEntry e;
        e.user_id = s->user_id;
        e.display_name = s->display_name;
        e.avatar_url = s->avatar_url;
        e.active = (_shell->active_account_ &&
                    s->user_id == _shell->active_account_->user_id);
        entries.push_back(std::move(e));
        if (!s->avatar_url.empty())
        {
            _shell->ensure_user_avatar(s->avatar_url);
        }
    }
    _accountPickerShared->set_entries(std::move(entries));

    CGFloat rowH = 48.0f;
    NSSize sz =
        NSMakeSize(220, rowH * (CGFloat)_shell->account_manager_.accounts().size());
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
    // Platform-agnostic teardown (unsubscribe the room, up_connector/presence
    // logout, client_->logout() + failure surface, stop_sync, clear account
    // state, tray refresh, index update, and — when other accounts remain — the
    // switch to a survivor via switch_active_account_impl_ +
    // refresh_account_ui_after_switch_) lives in ShellBase.
    const auto result = _shell->logout_active_account();
    if (!result.logged_out)
    {
        return;
    }

    if (!result.has_remaining)
    {
        // No accounts left → native empty-surface cleanup + login view.
        [self _refreshRoomList];
        [self _refreshInviteList];
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
            if (!s)
            {
                return;
            }
            s->_shell->arm_pending_login_();
        };
        [_loginView setMode:tesseract::views::LoginView::Mode::Initial];
        [_loginView reset];
        ((__bridge NSView*)_mainAppSurface->view_handle()).hidden = YES;
        _loginView.hidden = NO;
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
        _shell->notify_window_active(true);
    }
}

- (void)windowDidResignKey:(NSNotification*)notification
{
    (void)notification;
    if (_shell)
    {
        _shell->notify_window_active(false);
    }
}

- (void)windowDidDeminiaturize:(NSNotification*)notification
{
    (void)notification;
    [self _startAnimTickIfNeeded];
}

- (void)windowDidChangeOcclusionState:(NSNotification*)notification
{
    (void)notification;
    if (self.window.occlusionState & NSWindowOcclusionStateVisible)
        [self _startAnimTickIfNeeded];
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
    // Require winVisible: a hidden window can retain isKeyWindow in edge cases
    // (animation in progress, sheets), so don't suppress when not on screen.
    if (winVisible && winFocused && _shell->active_account_ &&
        _shell->active_account_->user_id == userId &&
        _shell->current_room_id_ == roomId)
    {
        return;
    }

    // Bounce dock whenever app is not focused — covers both visible-but-background
    // and fully hidden windows. requestUserAttention is a no-op when the app is
    // already frontmost, so this is safe.
    if (!winFocused)
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
        uid && _shell->active_account_ &&
        _shell->active_account_->user_id ==
            std::string(uid.UTF8String ?: "");
    if (self.window.isVisible && self.window.isKeyWindow && activeAccount && rid &&
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
        [self _switchActiveAccount:target_uid];
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

// handleMessageInserted/Updated/Removed and handleTimelineReset were removed:
// the MacShell C++ overrides that forwarded to them are gone, and the shared
// ShellBase handlers now drive _roomView (via room_view_) and relayout directly.

// updateRoomsForUserId: was the old ObjC EventBridge hook. EventHandlerBase now
// calls ShellBase::push_rooms_() directly, which invokes on_rooms_updated_()
// → _refreshRoomList + restore-room logic. No ObjC forwarding needed.

- (void)handleVerificationState:(BOOL)isVerified
{
    if (!_mainApp || !_mainAppSurface)
    {
        return;
    }
    // Only prompt when there is actually an identity to verify against. On a
    // fresh/only device our own login-time bootstrap holds the cross-signing
    // keys, so "verify this device" is a dead end — check_encryption_setup_
    // drives the Fresh setup overlay instead.
    if (!isVerified && !_shell->verification_banner_dismissed()
        && _shell->foreign_cross_signing_identity())
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
            _shell->client_->start_sas(_shell->verification_flow_id());
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

- (tesseract::views::RoomSettingsView*)_activeRoomSettingsView
{
    if (_mainApp && _mainApp->room_view()->room_settings_view()->is_open())
        return _mainApp->room_view()->room_settings_view();
    if (_mainApp)
        return _mainApp->space_root()->settings_view();
    return nullptr;
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
    _statusLabel.stringValue = TkTr("Not logged in");
    _statusLabel.font = [NSFont systemFontOfSize:11];
    _statusLabel.textColor = NSColor.secondaryLabelColor;
    _statusLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    _statusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    [_statusLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                         forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_statusBarView addSubview:_statusLabel];

    const CGFloat dotViewSz = static_cast<CGFloat>(tk::kInflightViewSize);
    _inflightDotView = [[InflightDotView alloc]
        initWithFrame:NSMakeRect(0, 0, dotViewSz, dotViewSz)];
    _inflightDotView.translatesAutoresizingMaskIntoConstraints = NO;
    [_statusBarView addSubview:_inflightDotView];

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

        [_inflightDotView.widthAnchor constraintEqualToConstant:dotViewSz],
        [_inflightDotView.heightAnchor constraintEqualToConstant:dotViewSz],
        [_inflightDotView.trailingAnchor
            constraintEqualToAnchor:_statusBarView.trailingAnchor constant:-4],
        [_inflightDotView.centerYAnchor
            constraintEqualToAnchor:_statusBarView.centerYAnchor],

        [_statusLabel.leadingAnchor
            constraintEqualToAnchor:_statusBarView.leadingAnchor constant:8],
        [_statusLabel.centerYAnchor
            constraintEqualToAnchor:_statusBarView.centerYAnchor],
        [_statusLabel.trailingAnchor
            constraintEqualToAnchor:_inflightDotView.leadingAnchor
                           constant:-4],
    ]];

    if (_shell)
        _shell->init_pool_callbacks();
    [self _onInflightChanged];
}

- (void)_setStatusLabelText:(NSString*)text
{
    if (!_statusLabel)
        return;
    // Status messages may carry markdown-style "[label](url)" hyperlinks
    // (see app/status_links.h). Plain messages keep the exact legacy
    // non-selectable NSTextField behavior; linked messages use the
    // attributed-string trick (selectable + allowsEditingTextAttributes)
    // so AppKit renders NSLinkAttributeName clickable and opens the
    // default browser itself.
    const std::string raw = std::string([text UTF8String] ?: "");
    // Opt-in gate (server/error text → plain) lives in ShellBase; fall back to
    // plain when the shell isn't attached yet.
    const auto segs =
        _shell ? _shell->parse_status(raw)
               : std::vector<tesseract::StatusSegment>{{raw, std::string{}}};
    if (!tesseract::status_has_links(segs))
    {
        _statusLabel.selectable = NO;
        _statusLabel.allowsEditingTextAttributes = NO;
        _statusLabel.stringValue = text;
        return;
    }

    NSMutableParagraphStyle* para = [[NSMutableParagraphStyle alloc] init];
    para.lineBreakMode = NSLineBreakByTruncatingTail;
    NSFont* font = [NSFont systemFontOfSize:11];
    NSMutableAttributedString* att =
        [[NSMutableAttributedString alloc] init];
    for (const auto& seg : segs)
    {
        NSString* run = @(seg.text.c_str());
        if (!run.length)
            continue;
        NSMutableDictionary* attrs = [@{
            NSFontAttributeName : font,
            NSParagraphStyleAttributeName : para,
            NSForegroundColorAttributeName : NSColor.secondaryLabelColor,
        } mutableCopy];
        if (!seg.url.empty())
        {
            if (NSURL* url = [NSURL URLWithString:@(seg.url.c_str())])
                attrs[NSLinkAttributeName] = url;
        }
        [att appendAttributedString:[[NSAttributedString alloc]
                                        initWithString:run
                                            attributes:attrs]];
    }
    _statusLabel.selectable = YES;
    _statusLabel.allowsEditingTextAttributes = YES;
    _statusLabel.attributedStringValue = att;
}

- (void)_refreshSyncStatus
{
    if (!_statusLabel || !_shell)
        return;
    using RLS = tesseract::RoomListState;
    using BS = tesseract::BackupState;
    const auto ss = _shell->sync_status_info();

    const bool room_busy    = (ss.room_list_state == RLS::Init ||
                                ss.room_list_state == RLS::SettingUp);
    const bool reconnecting = (ss.room_list_state == RLS::Recovering);
    const bool keys_busy    = (ss.backup_state == BS::Downloading);

    NSString* text;
    if (room_busy)
        text = @"Syncing rooms…";
    else if (reconnecting)
        text = @"Reconnecting…";
    else if (keys_busy)
        text = [NSString stringWithFormat:@"Downloading encryption keys (%llu)…",
                         (unsigned long long)ss.imported_keys];
    else if (ss.has_override)
        return; // persistent status override active; don't overwrite with "Connected"
    else
        text = @"Connected";

    [self _setStatusLabelText:text];
    _shell->set_sync_progress_shown(room_busy || reconnecting || keys_busy);
}

- (void)_onInflightChanged
{
    if (!_inflightDotView || !_shell)
        return;
    const auto info = _shell->inflight_info();
    _inflightDotView.dotColor      = info.dot_color;
    _inflightDotView.inflightCount = info.count;
    _inflightDotView.spinPhase     = info.spin_phase;
    [_inflightDotView setNeedsDisplay:YES];
    NSString* first = (info.count == 1)
        ? @"1 request in flight"
        : [NSString stringWithFormat:@"%u requests in flight", info.count];
    NSString* tip = [NSString stringWithFormat:
        @"%@\nmedia: %zu loading · fetch: %zu queued · send: %zu queued",
        first, info.media_pending, info.pool_pending, info.mut_pool_pending];
#ifndef NDEBUG
    if (!info.urls.empty()) {
        tip = [tip stringByAppendingFormat:@"\n── requests ──\n%@",
               [NSString stringWithUTF8String:info.urls.c_str()]];
    }
#endif
    _inflightDotView.toolTip = tip;
}

- (void)_onRoomListStateChanged
{
    [self _refreshSyncStatus];
    [self _onInflightChanged];

    // Once Running, attempt the deferred room restore (we waited for Running
    // to avoid subscribing to a room during initial sync, which triggers the
    // imbl promote_front data race in matrix-sdk-ui).
    using RLS = tesseract::RoomListState;
    if (_shell->sync_status_info().room_list_state == RLS::Running &&
        _shell->current_room_id_.empty())
    {
        _shell->maybe_restore_rooms();
    }
}

- (void)_onServerInfoReady
{
    if (_settingsView)
        _settingsView->set_server_info(_shell->server_info_ref());
    if (_mainApp && _mainApp->room_view())
        _mainApp->room_view()->header()->set_jump_to_date_enabled(
            _shell->server_info_ref().supports_msc3030);
    if (_mainAppSurface)
        _mainAppSurface->relayout();
}

- (void)_onOwnExtendedProfileReady
{
    if (_settingsView)
        _settingsView->set_extended_profile(_shell->own_extended_profile());
    if (_settingsPronounsField)
        _settingsPronounsField->set_text(_shell->own_extended_profile().pronouns);
    if (_settingsTzField)
        _settingsTzField->set_text(_shell->own_extended_profile().tz);
    if (_settingsBioField)
        _settingsBioField->set_text(_shell->own_extended_profile().biography);
    if (_settingsSurface)
        _settingsSurface->relayout();
}

- (void)_onProfileFieldResult:(const std::string&)key
                           ok:(bool)ok
                        error:(const std::string&)error
{
    if (!_settingsView) return;
    _settingsView->set_profile_field_busy(key, false);
    if (!ok)
        _settingsView->set_profile_field_error(key, error);
    if (_settingsSurface)
        _settingsSurface->relayout();
}

- (void)_updateTrayUnread:(bool)hasUnread highlight:(bool)hasHighlight
{
    if (_tray)
    {
        _tray->set_unread(hasUnread, hasHighlight);
    }
}

- (void)_rebuildTrayMenu
{
    if (!_tray || !_tray->is_available())
        return;

    auto items = _shell->build_tray_items_();
    _tray->rebuild_menu(std::move(items));
}

- (void)_refreshRoomList
{
    _shell->refresh_room_list();
}

- (void)_refreshInviteList
{
    if (_roomListView)
    {
        _roomListView->set_invites(_shell->invites_ptr());
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
    if (const auto* r = _shell->room_by_id(roomId); r && r->is_space)
    {
        _shell->push_space(roomId, _roomListView);
        [self _refreshRoomList];
        tesseract::ShellBase::SpaceNavFrame::enter(_roomListView);
        return;
    }
    [self hideSlashPopup];
    [self hideShortcodePopup];
    _shell->handle_compose_room_leaving();
    // (No unsubscribe-on-leave here: ShellBase::prune_warm_subscriptions_ owns
    // timeline lifecycle via the warm-subscription LRU.)
    _shell->current_room_id_ = roomId;
    _shell->clear_focused_state(roomId);
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
                                         c->_shell->mark_room_read();
                                     }
                                 }];
    _shell->clear_reply_details();
    _shell->persist_room_layout_pref();
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

    // Subscribe (mut pool) + initial history (shared pool). The split keeps the
    // network paginate off the single mut thread so the next switch's reset is
    // never blocked. See ShellBase::start_room_subscription_.
    std::vector<std::string> visibleIds =
        _roomListView ? _roomListView->visible_room_ids()
                      : std::vector<std::string>{};
    _shell->start_room_subscription(_shell->current_room_id_,
                                    std::move(visibleIds));
}

- (void)requestMoreHistoryForRoom:(std::string)roomId
{
    _shell->request_more_history(roomId);
}

- (void)handlePaginateResultForRoom:(std::string)roomId
                      reached_start:(BOOL)reached
{
    _shell->handle_paginate_result(roomId, (bool)reached);
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
        thumb ? _shell->account_manager_.thumbnail_cache()
              : _shell->account_manager_.image_cache();
    if (bytes.empty() || still_cache.contains(key) ||
        _shell->account_manager_.anim_cache().has(key))
    {
        return;
    }
    auto d = _shell->decode_image_(bytes, 0, 0);
    if (!d.frames.empty())
    {
        const std::int64_t now = static_cast<std::int64_t>(
            [[NSDate date] timeIntervalSince1970] * 1000.0);
        _shell->account_manager_.anim_cache().store(
            key, std::move(d.frames), std::move(d.delays_ms), now);
        [self _startAnimTickIfNeeded];
    }
    else if (d.still)
    {
        still_cache.store(key, std::move(d.still));
    }
}

- (void)_startAnimTickIfNeeded
{
    if (_animTimer || _shell->account_manager_.anim_cache().empty())
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
        _shell->tick_anim();
}

- (void)_stopAnimTick
{
    [_animTimer invalidate];
    _animTimer = nil;
}

- (void)_startInflightTickIfNeeded
{
    if (_inflightTimer)
        return;
    __weak MainWindowController* weakSelf = self;
    _inflightTimer =
        [NSTimer scheduledTimerWithTimeInterval:0.016
                                        repeats:YES
                                          block:^(NSTimer* t) {
                                              [weakSelf _inflightTick:t];
                                          }];
    [[NSRunLoop currentRunLoop] addTimer:_inflightTimer
                                 forMode:NSRunLoopCommonModes];
}

- (void)_stopInflightTick
{
    [_inflightTimer invalidate];
    _inflightTimer = nil;
}

- (void)_inflightTick:(NSTimer*)timer
{
    if (_shell)
        _shell->tick_inflight();
}

- (void)_repaintInflightSpinner
{
    if (_inflightDotView && _shell && _shell->inflight_needs_anim())
    {
        _inflightDotView.spinPhase = _shell->inflight_info().spin_phase;
        [_inflightDotView setNeedsDisplay:YES];
    }
}

- (void)_ensureStickerImageAsync:(std::string)url
{
    _shell->ensure_picker_image(url, true);
}

- (void)_ensureEmojiImageAsync:(std::string)url
{
    _shell->ensure_picker_image(url, false);
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
                             if (auto* f = s->_shell->account_manager_.anim_cache().current_frame(
                                     cache_key))
                             {
                                 [s _startAnimTickIfNeeded];
                                 return f;
                             }
                             {
                                 if (const auto* img =
                                         s->_shell->account_manager_.image_cache().peek(cache_key))
                                     return img;
                             }
                             [s _ensureStickerImageAsync:cache_key];
                             return nullptr;
                         }];

    __weak StickerPickerPanel* weakPanel = panel;
    panel.onSelected = ^(NSString* url, NSString* body, NSString* infoJson) {
        MainWindowController* s = weakSelf;
        if (!s)
            return;
        s->_shell->send_sticker(body.UTF8String ?: "", url.UTF8String ?: "",
                                infoJson.UTF8String ?: "{}");
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
                             if (auto* f = s->_shell->account_manager_.anim_cache().current_frame(
                                     cache_key))
                             {
                                 [s _startAnimTickIfNeeded];
                                 return f;
                             }
                             {
                                 if (const auto* img =
                                         s->_shell->account_manager_.image_cache().peek(cache_key))
                                     return img;
                             }
                             [s _ensureStickerImageAsync:cache_key];
                             return nullptr;
                         }];

    __weak StickerPickerPanel* weakPanel = panel;
    panel.onSelected = ^(NSString* url, NSString* body, NSString* infoJson) {
        MainWindowController* s = weakSelf;
        if (!s)
            return;
        s->_shell->send_sticker(body.UTF8String ?: "", url.UTF8String ?: "",
                                infoJson.UTF8String ?: "{}");
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

- (void)_showQRGrant
{
    _shell->start_qr_grant_overlay_public();
}

@end
