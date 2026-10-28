#pragma once

// macOS main window: the ShellBase subclass that owns the window state and logic.
// Included only from .mm files (ObjC types are fine here).

#import <AppKit/AppKit.h>

#import "AuxWindowController.h"
#import "MainWindowController.h"
#import <UserNotifications/UserNotifications.h>
#import "MacNowPlaying.h"
#import "MacOSTrayIcon.h"
#import "MacSpotlightSearch.h"

#include "views/shortcut_registry.h"
#include <tesseract/client.h>
#include <tesseract/event_handler.h>
#include <tesseract/image_pack.h>
#include <tesseract/session_store.h>
#include <tesseract/prefs.h>
#include <tesseract/paths.h>
#include <tesseract/settings.h>
#include <tesseract/visual.h>
#include "app/DeferredTeardown.h"
#include "views/SlashCommands.h"
#include "app/status_links.h"
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
#include "app/ScreenshotFixture.h"
#endif
#include "tk/anim_image_cache.h"
#include "tk/canvas_cg.h"
#include "tk/inflight_dot.h"
#include "tk/status_icons.h"
#include "tk/svg.h"
#include "platform/video_decode.h"
#include "tk/emoji_support.h"
#include "tk/host.h"
#include "tk/host_macos.h"
#include "app/image_send.h"
#include <tesseract/image_meta.h>
#include "app/jxl_decode.h"
#include "tk/image_sniff.h"
#include "tk/theme.h"
#include "views/BrandView.h"
#include "views/MainAppWidget.h"
#include "views/media_drop.h"
#include "views/MediaViewerOverlay.h"
#include "views/media_viewer_items.h"
#include "views/SettingsView.h"
#include "views/ShortcodeEngine.h"
#include "views/ShortcodePopup.h"
#include "views/MentionController.h"
#include "views/GifController.h"
#include "views/GifPopup.h"
#include "views/MentionPopup.h"
#include "views/SlashCommandController.h"
#include "views/SlashCommandPopup.h"
#include <tesseract/mentions.h>
#include <tesseract/account_session.h>
#include "app/AccountManager.h"
#include "app/ShellBase.h"
#include "app/shell_helpers.h"
#include "app/EventHandlerBase.h"
#include "views/settings/SettingsController.h"
#include "views/AccountPicker.h"
#include <algorithm>
#include <cmath>
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

@class InflightDotView;

// Controller internals shared with MainWindow.mm: the delegate conformances (so
// MainWindow can hand the controller out as a delegate) and the designated
// init for spawned (secondary) windows that share an existing AccountManager
// instead of owning their own.
namespace tesseract::macos { class MainWindow; }
@interface MainWindowController () <
    UNUserNotificationCenterDelegate, NSWindowDelegate>
- (instancetype)_initWithSharedAccountManager:(tesseract::AccountManager*)mgr;
// The C++ window this controller forwards to (used when spawning a window).
- (tesseract::macos::MainWindow*)mainWindow;
@end

// ─────────────────────────────────────────────────────────────────────────
//  _TkMenuAction — thin ObjC object that bridges a C++ callback to an
//  NSMenuItem (or gesture-recognizer) action. Created per-item; the target
//  must be kept alive by the caller, since AppKit does not retain it.
// ─────────────────────────────────────────────────────────────────────────
@interface _TkMenuAction : NSObject
- (instancetype)initWithCallback:(std::function<void()>)cb;
- (void)fire:(id)sender;
@end

namespace tesseract::macos
{

class MainWindow final : public tesseract::ShellBase
{
public:
    // ctrl_ is a weak back-pointer: MainWindow is owned by its
    // MainWindowController (via _window).
    explicit MainWindow(tesseract::AccountManager& account_manager,
                        MainWindowController* ctrl)
        : ShellBase(account_manager), ctrl_(ctrl)
    {
        account_manager_.register_window(this);
        broadcast_rebuild_tray_();
    }
    // Invalidates every NSTimer this window owns (their blocks capture `this`)
    // and removes the key-event monitor and notification observers.
    ~MainWindow() override;

    // ── Called by MainWindowController (all one-line forwards) ─────────────
    // Loads settings, restores the window geometry, builds the chrome and
    // applies the loaded theme. Call once, after the NSWindow exists and the
    // controller is its delegate.
    void initialize();
    // Observes backing-scale changes of the window and system colour (accent)
    // changes; the observers are removed in the destructor.
    void observe_system_changes();
    // Forwarded from the controller's KVO on NSApp.effectiveAppearance.
    void on_os_appearance_changed();
    // Drops the popup, settings and account-picker surfaces before the main
    // surface is destroyed, so they never outlive the host they anchor to.
    void release_surfaces();
    void stop_sync();

    // NSWindowDelegate.
    void save_window_geometry();
    void window_did_resize();
    bool window_should_close(NSWindow* sender);
    void window_did_become_key();
    void window_did_resign_key();
    void window_did_miniaturize();
    void window_did_deminiaturize();
    void window_did_change_occlusion_state();

    // The public -beginLogin.
    void begin_login();

    // UNUserNotificationCenterDelegate.
    void will_present_notification(
        UNNotification* notification,
        void (^completionHandler)(UNNotificationPresentationOptions));
    void did_receive_notification_response(UNNotificationResponse* response,
                                           void (^completionHandler)(void));

    // AppDelegate API.
    void dispatch_launch_action(tesseract::LaunchAction action,
                                std::string room_id)
    {
        dispatch_launch_action_(action, std::move(room_id));
    }
    void navigate_to_unread() { navigate_tray_unread_(); }
    void activate_spotlight_result(NSString* identifier);
    void show_emoji_picker();

    // Edit-menu Copy and the application-menu-bar actions.
    bool can_copy_selection() const;
    void copy_selection();
    void menu_open_settings();
    void menu_add_room();
    void menu_show_room_info();
    void menu_find_in_conversation();
    void menu_search_all_messages();
    void menu_go_back();
    void menu_go_forward();
    void menu_open_quick_switcher();
    // +1 steps forward through the recent rooms, -1 backward.
    void menu_cycle_recent_rooms(int direction);
    void menu_show_keyboard_shortcuts();
    bool validate_menu_action(SEL action) const;
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    void apply_screenshot_theme(bool dark);
    // Unhides the main view with the fixture seeded; false if assets are missing.
    bool prepare_screenshot_view();
    void refresh_for_screenshot();
    void start_screenshot_director(tesseract::screenshot::ScreenshotHost& host,
                                   std::string prefix)
    {
        start_screenshot_director_(host, std::move(prefix));
    }
#endif

protected:
    void post_to_ui_(std::function<void()> fn) override;
    void post_to_ui_after_(int ms, std::function<void()> fn) override;
    void request_relayout_() override;
    void request_repaint_() override;
    void after_rooms_updated_ui_() override;
    void apply_window_title_ui_(const std::string& title) override;
    void on_space_children_cache_ready_ui_() override;
    void on_space_unjoined_summaries_ready_ui_(const std::string&) override;
    void on_tray_unread_changed_(bool has_unread,
                                 bool has_highlight) override;
    void on_dock_badge_changed_(uint64_t count) override;
    void ingest_tile_media_(const tk::CacheKey& key,
                            std::vector<std::uint8_t> bytes) override;
    void decode_video_first_frame_(
        std::vector<std::uint8_t> bytes,
        std::function<void(std::vector<std::uint8_t>)> done) override;
    void cache_rgba_image_(const tk::CacheKey& key, int w, int h,
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
    void update_main_anim_regions_() override;
    void start_inflight_tick_() override;
    void stop_inflight_tick_() override;
    void repaint_inflight_spinner_() override;

    // handle_timeline_reset_ui_ and handle_message_{inserted,updated,removed}_ui_
    // are NOT overridden here: the shared ShellBase implementations drive the
    // same room_view_ this shell owns (via request_relayout_)
    // and dispatch to secondary windows. They also carry guards this shell used
    // to drop (in-thread reply exclusion, scroll/focus restore on reset), so the
    // base path is strictly more correct.
    void handle_sync_error_ui_(std::string context, std::string user_id,
                               std::string description,
                               bool soft_logout) override;
    void on_room_list_state_ui_() override;
    void on_inflight_ui_() override;
    void on_low_power_mode_ui_(bool active) override;
    bool tray_available_() const override;
    void open_app_settings_ui_() override;
    void close_app_settings_ui_() override;
    void rebind_login_client_ui_() override;
    // makeKeyAndOrderFront: also brings back a window orderOut:'d to the tray.
    void raise_main_window_ui_() override { raise_and_activate_(); }
    void on_show_status_message_ui_(const std::string& msg) override;
    void on_restore_status_ui_() override;
    void on_startup_restore_progress_ui_(const std::string& status) override;

    tk::ThemeMode os_color_scheme_() const override;
    std::optional<tk::Color> os_accent_color_() const override;
    void after_theme_applied_(const tk::Theme& t) override;
    void for_each_shell_surface_(
        const std::function<void(tk::SurfaceBase&)>& fn) override;
    tesseract::RoomWindowBase*
    create_secondary_room_window_(const std::string& room_id) override;
    void raise_and_activate_() override;
    void set_window_fullscreen_(bool on) override;
    void rebuild_tray_() override;
    bool spawn_relaunch_(const std::vector<std::string>& args) override;
    void quit_app_() override;
    bool is_ctrl_held_() const override;
    bool ui_ready_() const override { return ctrl_ != nil; }
    void focus_forward_picker_field_() override;
    void hide_forward_picker_field_() override;
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
    std::unique_ptr<tk::AudioPlayback> make_call_audio_output_() override
    {
        return tk::make_audio_playback_macos();
    }
    tesseract::CallWindowBase* create_call_window_() override;
    tesseract::AuxWindowBase* create_aux_window_(std::string title,
                                                 std::unique_ptr<tk::Widget> root,
                                                 int width, int height) override
    {
        return tesseract::make_mac_aux_window(title, std::move(root), width, height,
                                              current_theme_);
    }

    // Room-selection hooks (ShellBase::on_room_selected_).
    void after_room_set_() override;

    // Tab management hooks.
    void navigate_to_room_(const std::string& room_id) override
    {
        tab_navigate_room(room_id);
    }

    // Image decoding and native pickers.
    DecodedImage decode_image_(const std::vector<uint8_t>& bytes, int max_w,
                               int max_h) override;
    bool decode_image_streamed_(
        const std::vector<uint8_t>& bytes, int max_w, int max_h,
        const std::function<void(std::unique_ptr<tk::Image>, int)>& on_first_frame,
        const std::function<void(int, std::unique_ptr<tk::Image>, int)>& on_frame)
        override;
    bool decode_image_streamed_windowed_(
        const std::vector<uint8_t>& bytes, int max_w, int max_h,
        const std::function<void(std::unique_ptr<tk::Image>, int,
                                 std::shared_ptr<tk::AnimDecodeSession>,
                                 std::size_t)>& on_first_frame,
        const std::function<void(int, std::unique_ptr<tk::Image>, int)>& on_frame)
        override;
    void pick_image_file_(
        std::function<void(std::vector<uint8_t>, std::string)> cb) override;

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

    // Platform probe called by ShellBase::extract_drop_media_ for video/audio:
    // extracts thumbnail, dimensions and duration on a background thread and
    // posts the result back via post_to_ui_. When `target` is non-null the
    // result is posted to that compose bar (a pop-out window's), guarded by
    // `target_alive`; otherwise to the main compose bar.
    void probe_drop_video_(std::uint32_t pending_gen,
                           std::vector<std::uint8_t> bytes,
                           tesseract::views::ComposeBar* target,
                           std::shared_ptr<bool> target_alive) override;
    void probe_drop_audio_(std::uint32_t pending_gen,
                           std::vector<std::uint8_t> bytes,
                           tesseract::views::ComposeBar* target,
                           std::shared_ptr<bool> target_alive) override;


    // Overlays opened from menu items / shortcuts: the open/close pairs live
    // in ShellBase.
    void bring_main_window_forward_for_overlay_() override;

private:
    // Builds the main surface, the widget tree and its wiring, the status bar
    // and the branding splash.
    void build_chrome_();
    // Local NSEvent monitor for the shortcuts the canvas must see before a
    // focused native text view does (see the definition).
    void install_key_monitor_();
    bool app_ui_ready_() const;
    bool has_current_room_() const;

    // Tray, Now Playing, Spotlight, dock attention. Each of the three app-wide
    // integrations is owned by exactly one window (claimed through
    // AccountManager) and created after login.
    void start_tray_if_needed();
    void start_now_playing_if_needed();
    void start_spotlight_search_if_needed();
    // Immediate (non-debounced) Spotlight reindex.
    void reindex_spotlight_now();
    // Dock bounce; cancelled when the window becomes key.
    void request_user_attention();
    bool is_main_window_focused_() const override
    {
        return ctrl_ && ctrl_.window.isKeyWindow;
    }
    void request_notification_attention_() override { request_user_attention(); }
    void deliver_notification_(const std::string& user_id,
                               const tesseract::Notification& n) override;
    void cancel_user_attention();

    // Status bar (bottom strip: sync label, low-power glyph, inflight dot).
    void build_status_bar(NSView* content);
    void set_status_label_text(NSString* text);

    // Login flow.
    void on_signed_out_ui_(const LogoutResult& result) override;
    // Branding splash shown before the session check completes; added above the
    // main app view and below the status bar (call after build_status_bar).
    void build_branding(NSView* content);
    void hide_branding();
    void hide_login_view();
    bool login_view_visible() const;

    // Native dialogs behind the settings controller's key export/import and the
    // history export (see ShellBase).
    bool has_save_file_dialog_() const override;
    void prompt_passphrase_(std::string title,
                            std::function<void(std::string)> cb) override;
    void pick_save_file_(std::string title, std::string suggested_name,
                         std::function<void(std::string)> cb) override;
    void pick_open_file_(std::string title,
                         std::function<void(std::string)> cb) override;
    void pick_folder_(std::string title,
                      std::function<void(std::string)> cb) override;
    void show_message_dialog_(std::string title, std::string text,
                              bool is_error) override;

    // Right-click on the main surface: user-strip menu or sticker context menu.
    void handle_main_right_click(tk::Point p);
    // Settings surface.
    void hide_settings_surface();
    // 30 s repeating tick for the presence idle-decay check.
    void start_presence_tick_timer();
    // The composer's text area (self-owned by ComposeBar; borrowed).
    tk::TextArea* room_text_area() const { return room_text_area_; }

    // The window's backing scale factor changed (moved between displays).
    void on_backing_scale_changed_();
    // Coalescing debounce for the Spotlight reindex.
    void schedule_spotlight_reindex_();
    // Sticker context menu (right-click on a sticker in the message list).
    void show_sticker_context_menu_(NSPoint screenPt);
    void save_context_sticker_();
    // Login view. Built on first need (ShellBase::ensure_login_view_ through
    // create_login_surface_ui_, called from begin_login/begin_add_account/
    // logout_active_account_ui) and torn down once no longer needed
    // (show_main_content_ calls teardown_login_view_) rather than kept alive
    // (native controls, decoded icon, full widget tree) for the whole app
    // session. See ui/shared/app/DeferredTeardown.h for why teardown is
    // deferred rather than synchronous. show_main_content_ is also the single
    // choke point for "login is no longer showing, main app is".
    void show_main_content_() override;
    std::unique_ptr<tk::SurfaceBase> create_login_surface_ui_() override;
    void unmount_login_surface_ui_() override;
    void show_login_page_ui_() override;
    // The login page's native view.
    NSView* login_native_view_() const
    {
        return (__bridge NSView*)static_cast<tk::macos::Surface*>(
            login_surface_.get())->view_handle();
    }
    void start_platform_services_ui_() override;
    bool main_window_foreground_for_tray_() const override;
    std::vector<std::uint8_t> encode_selfie_jpeg_(const std::uint8_t* bgra,
                                                  std::uint32_t w,
                                                  std::uint32_t h) override;
    std::unique_ptr<tk::SurfaceBase> create_settings_surface_ui_() override;
    void unmount_settings_surface_ui_() override;
    tk::SurfaceBase* main_surface_() override;
    void grab_main_surface_focus_() override;
    // The settings page's native view.
    NSView* settings_native_view_() const
    {
        return (__bridge NSView*)static_cast<tk::macos::Surface*>(
            settings_surface_.get())->view_handle();
    }
    void hide_main_window_for_tray_() override;
    void show_main_window_for_tray_(const std::string& token) override;
    bool notifications_set_up_ = false;
    // Status bar.
    void set_status_bar_hidden_(bool hidden);
    void show_sync_status_text_(const std::string& text) override;
    void refresh_low_power_icon_();

    // Weak: the controller owns this window (via _window), so a strong pointer
    // would be a retain cycle. Used only for what needs the NSWindowController
    // itself (the NSWindow, delegate wiring, startedHidden).
    __weak MainWindowController* ctrl_;

    // Branding splash shown before the session check completes.
    std::unique_ptr<tk::macos::Surface> branding_surface_;
    tesseract::views::BrandView* branding_view_ = nullptr; // borrowed from branding_surface_

    // Status bar: container view, sync-state label, in-flight dot.
    NSView* status_bar_view_ = nil;
    NSLayoutConstraint* status_bar_height_constraint_ = nil;
    NSTextField* status_label_ = nil;
    InflightDotView* inflight_dot_view_ = nil;
    // Subtle low-power (battery) glyph, hidden by default.
    NSImageView* low_power_label_ = nil;
    // Click target for status_label_ (gesture recognizers do not retain theirs).
    _TkMenuAction* status_click_action_ = nil;

    // Compose text area (self-owned by ComposeBar; borrowed here).
    tk::TextArea* room_text_area_ = nullptr;
    // Wrap a decoded GIF-strip frame in the CoreGraphics image type.
    std::unique_ptr<tk::Image> make_image_from_bgra_(
        int w, int h, const std::uint8_t* bgra) override;
    bool make_image_from_bgra_on_worker_() const override { return true; }

    // Right-click context menu sticker state.
    std::string ctx_sticker_mxc_url_;
    std::string ctx_sticker_body_;
    std::string ctx_sticker_info_json_;
    // Click target for the sticker menu item (NSMenuItem does not retain it).
    _TkMenuAction* sticker_save_action_ = nil;

    // Settings surface — full-window sibling of the main app view and the login
    // view. The view itself is borrowed from the surface's root and tracked by
    // ShellBase::stats_settings_view_ (non-null exactly while it exists).

    NSTimer* anim_timer_ = nil;
    NSTimer* inflight_timer_ = nil;
    NSTimer* presence_tick_timer_ = nil;

    // System-tray icon (menu-bar status item). Null until created after login.
    // When non-null and `is_available()`, closing the window hides it instead
    // of terminating the app.
    std::unique_ptr<MacOSTrayIcon> tray_;

    // "Now Playing" (MPNowPlayingInfoCenter/MPRemoteCommandCenter) and
    // Spotlight search (CSSearchableIndex) integrations — macOS analogs of
    // Linux's MPRIS/GNOME-Shell-search-provider/KRunner adapters. Each is
    // one app-wide singleton, created after login (see
    // start_now_playing_if_needed / start_spotlight_search_if_needed).
    std::unique_ptr<MacNowPlaying> now_playing_;
    std::unique_ptr<MacSpotlightSearch> spotlight_search_;
    // Coalescing debounce for spotlight_search_->reindex() — see
    // schedule_spotlight_reindex_.
    NSTimer* spotlight_reindex_timer_ = nil;
    // Safety net: contact-roster changes have no dedicated push hook into
    // MainWindow (unlike room-list changes via on_rooms_updated_), so this
    // periodically nudges a reindex to bound staleness.
    NSTimer* spotlight_periodic_timer_ = nil;

    // Local key-event monitor token (install_key_monitor_); removed in the destructor.
    id escape_monitor_ = nil;

    // Dock-bounce token; 0 = no request in flight.
    NSInteger attention_request_token_ = 0;

    // NSNotificationCenter tokens (see observe_system_changes).
    id backing_properties_observer_ = nil;
    id system_colors_observer_ = nil;
    // Guards on_os_appearance_changed() against reacting to our own
    // NSApp.appearance writeback in after_theme_applied_() (see there).
    // Mutable: toggled inside the const os_color_scheme_() query.
    mutable bool setting_own_appearance_ = false;

    // The surface hosting the full main-app widget tree. Declared last so it is
    // destroyed first: every other surface, controller and callback that anchors
    // to its host is gone by then.
    std::unique_ptr<tk::macos::Surface> main_app_surface_;
};

} // namespace tesseract::macos
