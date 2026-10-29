#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <ole2.h>
// GdiplusTypes.h calls unqualified min()/max(); with NOMINMAX the Win32
// macros are gone, so std::min/std::max must be brought into scope (a bare
// <algorithm> is not enough — the names there are std-qualified) before it.
#include <algorithm>
using std::max;
using std::min;
#include <gdiplus.h>

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/event_handler.h>
#include <tesseract/launch_args.h>
#include <tesseract/session_store.h>
#include <tesseract/visual.h>

#include "app/AccountManager.h"
#include "app/ShellBase.h"
#include "app/EventHandlerBase.h"
#include "CustomTitleBar.h"
#include "tk/anim_image_cache.h"
#include "tk/canvas.h"
#include "tk/host.h"
#include "tk/host_win32.h"
#include "tk/key_chord.h"
#include "views/format.h"
#include "views/BrandView.h"
#include "views/MainAppWidget.h"
#include "views/media_viewer_items.h"
#include "views/SettingsView.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Custom window messages
// WM_APP + 1 was WM_TESSERACT_MESSAGE_INSERTED; Windows now routes through the
// shared ShellBase timeline/message handlers (no per-message marshalling msg).
// WM_APP + 2/3/5/6 and 12/14/16/21 were per-event messages (rooms, sync error,
// reconnect, auth error, image packs, media bytes, account prefs, room-list
// state); all of those now arrive through EventHandlerBase and post_to_ui_.
// WM_APP + 4 was WM_TESSERACT_TIMELINE_RESET; see the +1 note above.
// WM_APP + 8 was the recover-done message; the inline key-entry banner that
// consumed it was removed (key recovery now lives in the encryption-setup
// overlay).
// WM_APP + 9 was WM_TESSERACT_MESSAGE_UPDATED; see the +1 note above.
// WM_APP + 11 was WM_TESSERACT_MESSAGE_REMOVED; see the +1 note above.
// WM_APP + 13 was WM_TESSERACT_STICKER_BYTES; the sticker picker now uses
// the shared ShellBase async image-cache path (ensure_picker_image_).
// WM_APP + 15 was WM_TESSERACT_SUBSCRIBE_DONE (removed with the shared
// start_room_subscription_ consolidation); left as a gap to avoid renumbering.
// WM_APP + 17 was WM_TESSERACT_NOTIFY (removed with the shared notification
// policy in ShellBase); left as a gap to avoid renumbering.
// WM_APP + 18 = WM_TESSERACT_NOTIFY_CLICK, defined in Win32Notifier.h
// WM_APP + 19 was WM_TESSERACT_VIDEO_BYTES (removed; media now flows through begin_media_req_)
// WM_APP + 20 is taken by Win32TrayIcon on its hidden helper HWND.
constexpr UINT WM_TESSERACT_POST_TO_UI = WM_APP + 22;
// WM_APP + 25/26 were WM_TESSERACT_JOIN_ROOM_LOOKUP_DONE/_DONE (removed with
// the shared AddRoomView/lookup_room_command_/join_room_command_
// consolidation); left as a gap to avoid renumbering.
// WM_APP + 27 was WM_TESSERACT_FILE_BYTES (removed; file saves now use begin_media_req_)

namespace win32
{

class Win32Notifier; // defined in Win32Notifier.h, included by MainWindow.cpp
class Win32TrayIcon; // defined in Win32TrayIcon.h, included by MainWindow.cpp
class Win32Taskbar;


// ---------------------------------------------------------------------------

class MainWindow : public tesseract::ShellBase
{
public:
    static bool register_class(HINSTANCE hInst);
    // "TesseractMainWnd" plus the --profile suffix: a second launch finds
    // the running instance of its own profile with FindWindowW(class_name()).
    // Only valid after tesseract::set_profile() (prepare_launch) has run.
    static const wchar_t* class_name();
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    // start_hidden: true when launched via the OS autostart mechanism
    // (--autostart). The caller passes SW_HIDE to create() in that case;
    // start_login()'s async restore completion reads this member: stays
    // hidden on a successful silent restore, or force-shows the window
    // (ShowWindow(hwnd_, SW_SHOW)) if there's no saved session to restore.
    explicit MainWindow(tesseract::AccountManager& account_manager, HINSTANCE hInst,
                        Win32Taskbar& taskbar, bool start_hidden = false,
                        tesseract::LaunchAction launch_action =
                            tesseract::LaunchAction::None,
                        std::string launch_room_id = {}
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
                        , std::filesystem::path screenshot_dir = {}
#endif
                        );
    ~MainWindow();

    bool create(int nCmdShow);
    Win32Taskbar& taskbar() { return taskbar_; }

    // Called by the main message loop before TranslateMessage/DispatchMessage.
    // Routes application accelerators (e.g. Ctrl+K → quick switcher) to the
    // window as commands even when a native edit control holds focus.
    // Returns true when the message was consumed.
    bool pre_translate_message(MSG* msg);

    // MediaKind is inherited from tesseract::ShellBase.
    // RoomAvatar → tk_avatars_, relayout main_app_surface_
    // UserAvatar → tk_avatars_, invalidate main_app_surface_
    // MediaImage → anim_cache_/tk_images_[url], invalidate main_app_surface_

    // ── EventHandlerBase UI-thread hook overrides (Win32) ────────────────────
    // handle_timeline_reset_ui_ and handle_message_{inserted,updated,removed}_ui_
    // are NOT overridden here: the shared ShellBase implementations drive the
    // same room_view_ this shell owns and dispatch to secondary windows. They
    // also carry guards this shell used to drop (in-thread reply exclusion,
    // scroll/focus restore on reset), so the base path is strictly more correct.
    void handle_sync_error_ui_(std::string context, std::string user_id,
                               std::string description,
                               bool soft_logout) override;
    void on_room_list_state_ui_() override;
    void on_inflight_ui_() override;
    void on_low_power_mode_ui_(bool active) override;
    bool tray_available_() const override;
    void open_app_settings_ui_() override { open_settings_(); }
    void close_app_settings_ui_() override { close_settings_(); }
    void rebind_login_client_ui_() override;
    void on_show_status_message_ui_(const std::string& msg) override;
    void on_restore_status_ui_() override;
    void on_startup_restore_progress_ui_(const std::string& status) override;

    // Returns the user-chosen path, or L"" if cancelled.
    // Also called by RoomWindow for save dialogs in popout windows.
    std::wstring show_save_dialog_(const std::wstring& suggested,
                                   const wchar_t* filter);
    // An OPENFILENAMEW filter ("Label\0pattern\0...\0") from UTF-8 labels,
    // so the labels can be translated.
    static std::wstring file_filter(
        std::initializer_list<std::pair<std::string, const wchar_t*>> entries);
    // OPENFILENAMEW filter for a MediaSaveSpec (its type group + "All files").
    static std::wstring save_filter_for(const tesseract::views::MediaSaveSpec& spec);
    bool has_save_file_dialog_() const override;
    void prompt_passphrase_(std::string title,
                            std::function<void(std::string)> cb) override;
    void pick_media_save_path_(const tesseract::views::MediaSaveSpec& spec,
                               std::function<void(std::string)> cb) override;
    void pick_save_file_(std::string title, std::string suggested_name,
                         std::function<void(std::string)> cb) override;
    void pick_open_file_(std::string title,
                         std::function<void(std::string)> cb) override;
    void pick_folder_(std::string title,
                      std::function<void(std::string)> cb) override;
    void show_message_dialog_(std::string title, std::string text,
                              bool is_error) override;

public:
    /// Split a status-bar string into text and link runs, honouring the opt-in
    /// of the message being shown (see ShellBase::show_status_message_). Used by
    /// the status bar's window procedure.
    std::vector<tesseract::StatusSegment> status_segments(const std::string& msg) const
    {
        return parse_status_message_(msg);
    }

private:
    void on_create(HWND hwnd);
    void on_destroy();
    void on_size(int w, int h);
    // Whether hwnd_ should be treated as the active/foreground window for
    // tray-click purposes. Backed by tray_owner_considered_active_, which
    // WM_ACTIVATE keeps up to date (see its handler): losing activation to
    // Explorer's own shell chrome (taskbar, tray flyouts — unavoidable when
    // reaching for the notify icon at all) doesn't count as leaving the app,
    // only losing it to a genuinely different application does.
    bool is_tray_owner_effectively_active_() const;
    bool main_window_foreground_for_tray_() const override;
    std::vector<std::uint8_t> encode_selfie_jpeg_(const std::uint8_t* bgra,
                                                  std::uint32_t w,
                                                  std::uint32_t h) override;
    std::unique_ptr<tk::Image>
    decode_avatar_(const std::vector<std::uint8_t>& bytes) override;
    void hide_main_window_for_tray_() override;
    void show_main_window_for_tray_(const std::string& token) override;
    static bool is_explorer_foreground_(HWND fg);
    void start_login();
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    void start_screenshot_mode_();
    bool save_screenshot_(const wchar_t* filename);
    // ScreenshotHost adapter; nested so it can reach the private hooks.
    class Win32ScreenshotHost;
#endif
    void show_login_page_ui_() override;
    void show_main_content_() override;
    // Launch-action hooks for ShellBase::dispatch_launch_action_.
    void raise_main_window_ui_() override;
    void bring_main_window_forward_for_overlay_() override;
    void restore_focus_after_overlay_() override;
    void open_settings_();
    void close_settings_();
    // Push ShellBase::compose_window_title_()'s string to the OS window title
    // (and thus the custom title bar, which reads it via GetWindowTextW).
    // SetWindowTextW alone doesn't repaint the custom-drawn title bar (see
    // CustomTitleBar.h), so this also invalidates the strip.
    void apply_window_title_ui_(const std::string& title) override;
    bool is_main_window_focused_() const override;
    void request_notification_attention_() override;
    void navigate_to_room(const std::string& room_id);

    void focus_forward_picker_field_() override;
    void hide_forward_picker_field_() override;

    // ensure_room_avatar_, ensure_user_avatar_, and ensure_media_image_ are
    // inherited from tesseract::ShellBase. ensure_reply_details_ now lives
    // on RoomPane (main_room_pane_).

    void show_sync_status_text_(const std::string& text) override;
    std::unique_ptr<tk::SurfaceBase> create_login_surface_ui_() override;
    std::unique_ptr<tk::SurfaceBase> create_settings_surface_ui_() override;
    tk::SurfaceBase* main_surface_() override;
    void grab_main_surface_focus_() override;
    void on_pre_paint_() override;
    // The login page's native surface (its window handle).
    tk::win32::Surface* login_native_() const
    {
        return static_cast<tk::win32::Surface*>(login_surface_.get());
    }
    // The settings page's native surface (its window handle).
    tk::win32::Surface* settings_native_() const
    {
        return static_cast<tk::win32::Surface*>(settings_surface_.get());
    }
    void start_platform_services_ui_() override;
    // last_room_list_state_, last_backup_state_, last_imported_keys_, and
    // sync_progress_shown_ are inherited from tesseract::ShellBase.
    // Room and message rendering are owned by the shared widget tree —
    // see RoomListView / MessageListView. The legacy DRAWITEM hooks
    // are gone.

    void apply_default_font(HWND);                // SegoeUI / SegoeUI Variable
    void on_system_theme_changed();               // re-apply DWM + invalidate
    void paint_main_background(HDC, const RECT&); // compose card etc.
    void show_user_context_menu_(int screen_x, int screen_y);

    float dip_scale() const
    {
        const float dpi = static_cast<float>(GetDpiForWindow(hwnd_));
        return dpi > 0.f ? dpi / 96.f : 1.f;
    }
    LONG dip_to_phys(float dip) const
    {
        return static_cast<LONG>(std::round(dip * dip_scale()));
    }

    HINSTANCE hInst_;
    Win32Taskbar& taskbar_;
    HWND hwnd_ = nullptr;
    CustomTitleBar title_bar_;
    // Tracks WM_ACTIVATE so the custom title bar can dim its text/icon to
    // match Win11's inactive-caption convention.
    bool is_active_ = true;
    // Media-viewer OS full-screen: caption strip suppressed, window borderless
    // over the monitor. fs_saved_placement_ restores the prior geometry.
    bool fs_active_ = false;
    WINDOWPLACEMENT fs_saved_placement_{};
    std::unique_ptr<tk::win32::Surface> branding_surface_;
    tesseract::views::BrandView* branding_view_ = nullptr; // borrowed, owned by branding_surface_
    bool branding_visible_ = true;
    bool login_visible_ = false;

    // Single surface hosting the full MainAppWidget tree.
    // main_app_ / room_view_ live in ShellBase (assigned in setup).
    std::unique_ptr<tk::win32::Surface> main_app_surface_;

    // Settings surface — full-window sibling of main_app_surface_ and the login surface.
    // "settings page is showing" state lives in ShellBase::app_settings_open_.
    // The name/pronouns/timezone/bio fields are self-owned by AccountSection
    // — see AccountSection::name_field()/pronouns_editor()/tz_field()/
    // bio_field() — so no shell-side member is needed for them.

    // Borrowed from main_app_->room_view()->compose_bar()->text_area() —
    // see ComposeBar::text_area(). Quick switcher's, message search's,
    // forward picker's, find-in-room's, room-info topic-edit, Room Settings
    // name/topic, and image-pack name/shortcode/rename/paste-catcher fields
    // are all self-owned too — see QuickSwitcher::search_field() /
    // MessageSearchView::search_field() / ForwardRoomPicker::search_field() /
    // RoomSearchBar::search_field() / RoomInfoPanel::topic_field() /
    // RoomSettingsView::name_field()/topic_field() /
    // ImagePackEditorView::new_pack_name_field()/shortcode_field()/
    // pack_name_field()/paste_catcher().
    tk::TextArea* room_text_area_ = nullptr;

    // ── GIF picker (/gif <query>) ─────────────────────────────────────────
    std::unique_ptr<tk::Image> make_image_from_bgra_(
        int w, int h, const std::uint8_t* bgra) override;
    HWND hStatus_ = nullptr;
    HWND hStatusTip_ = nullptr;
    std::wstring inflight_tip_text_;
    bool low_power_active_win_ = false;

    // Multi-account state: account_manager_ (ref), active_account_, client_,
    // event_handler_, per_account_rooms_, pending_login_client_,
    // pending_login_temp_dir_, pending_login_is_add_account_,
    // add_account_return_uid_ are inherited from tesseract::ShellBase.


    std::unique_ptr<Win32TrayIcon> tray_;
    bool quitting_ = false;
    // Whether hwnd_ should still be treated as "the active app" for tray-
    // click purposes. Set true on WM_ACTIVATE(active); on WM_ACTIVATE
    // (inactive), only cleared if whatever took the foreground is NOT part
    // of Explorer's own shell chrome (taskbar, tray flyouts) — reaching for
    // the notify icon at all necessarily starts by activating that chrome,
    // which shouldn't count as "the user switched to another app".
    bool tray_owner_considered_active_ = true;
    // True when constructed with start_hidden=true (--autostart) and no
    // saved session has yet forced the window visible. See start_login().
    bool start_hidden_ = false;
    // Set by spawn_main_window_ on a heap-allocated secondary window, which
    // deletes itself in WM_NCDESTROY. Never set on the primary.
    bool delete_on_destroy_ = false;
    std::unordered_map<std::string, std::string> recent_taskbar_avatar_rooms_;
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    std::filesystem::path screenshot_dir_;
    std::unique_ptr<tesseract::screenshot::ScreenshotHost> screenshot_host_;
    // The director never has two steps pending, so one timer id suffices.
    std::function<void()> screenshot_step_;
#endif
    // rooms_, current_room_id_, pending_restore_room_, space_stack_
    // are inherited from tesseract::ShellBase.

    // pagination_ and kPaginationBatch are inherited from tesseract::ShellBase.

    ULONG_PTR gdiplus_token_ = 0;

    // tk_avatars_, tk_images_, anim_cache_, voice_prefetched_,
    // video_thumb_in_flight_, reply_details_requested_, media_fetches_in_flight_,
    // sticker_fetches_in_flight_ are inherited from tesseract::ShellBase.

    /// WM_TIMER handler — advances every entry whose `next_advance_ms`
    /// has passed and triggers a single repaint of the message surface +
    /// sticker picker when at least one frame changed.
    void on_anim_tick();

    // run_async_, pool_ are inherited from tesseract::ShellBase.

    static constexpr UINT_PTR kAnimTimerId = 0xA01u;
    static constexpr UINT_PTR kInflightTimerId = 0xA02u;
    static constexpr UINT_PTR kPresenceTickTimerId = 8;
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    static constexpr UINT_PTR kScreenshotStepTimerId = 9;
#endif
    static constexpr UINT kAnimTimerMs = 16; // ~60 fps
    bool anim_timer_running_ = false;
    // Last on_anim_tick() time and whether a catch-up tick is already queued
    // by the pre-paint hook (see the hook in MainWindow.cpp).
    std::int64_t last_anim_tick_ms_ = 0;
    bool anim_catchup_posted_ = false;
    bool inflight_timer_running_ = false;

    // ShellBase virtual hooks (Win32 implementations).
    bool is_main_window_visible_() const override
    {
        return hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
    }
    void navigate_to_room_(const std::string& room_id) override
    {
        navigate_to_room(room_id);
    }
    void apply_native_theme_(const tk::Theme& t) override;
    void for_each_shell_surface_(
        const std::function<void(tk::SurfaceBase&)>& fn) override;
    void apply_fonts_ui_() override;
    tk::ThemeMode os_color_scheme_() const override;
    std::optional<tk::Color> os_accent_color_() const override;
    std::optional<tk::HighContrastColors> os_high_contrast_() const override;
    void post_to_ui_(std::function<void()> fn) override;
    void post_to_ui_after_(int ms, std::function<void()> fn) override;
    void request_relayout_() override;
    void request_repaint_() override;
    void after_rooms_updated_ui_() override;
    void on_recent_room_visited_(const tesseract::RoomInfo& room) override;
    void on_space_children_cache_ready_ui_() override;
    void on_space_unjoined_summaries_ready_ui_(const std::string&) override;
    void on_tray_unread_changed_(bool has_unread,
                                 bool has_highlight) override;
    void on_upload_progress_ui_(std::uint64_t request_id,
                                std::uint64_t current,
                                std::uint64_t total) override;
    void on_upload_finished_ui_(std::uint64_t request_id, bool ok) override;

    // Tab management hooks.
    void on_tab_state_changed_ui_() override;
    tesseract::RoomWindowBase*
    create_secondary_room_window_(const std::string& room_id) override;
    void raise_and_activate_() override;
    void activate_for_notification_(const std::string& token) override;
    void set_window_fullscreen_(bool on) override;
    void rebuild_tray_() override;
    bool spawn_relaunch_(const std::vector<std::string>& args) override;
    void quit_app_() override;
    bool is_ctrl_held_() const override;
    void spawn_main_window_(
        std::shared_ptr<tesseract::AccountSession> account) override;
    std::unique_ptr<tesseract::IEventHandler>
    make_account_bridge_(const std::string& uid) override;
    void install_account_notifier_(
        tesseract::AccountSession& session) override;
    std::unique_ptr<tk::AudioPlayback> make_call_audio_output_() override;
    tesseract::CallWindowBase* create_call_window_() override;
    tesseract::AuxWindowBase* create_aux_window_(std::string title,
                                                 std::unique_ptr<tk::Widget> root,
                                                 int width, int height) override;
    bool accept_media_bytes_(const tk::CacheKey& key, MediaKind kind,
                             const std::vector<std::uint8_t>& bytes) override;
    void ingest_tile_media_(const tk::CacheKey& key,
                            std::vector<std::uint8_t> bytes) override;
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
    std::int64_t monotonic_ms_() override;
    void start_anim_tick_() override;
    void stop_anim_tick_() override;
    void update_main_anim_regions_() override;
    void start_inflight_tick_() override;
    void stop_inflight_tick_() override;
    void repaint_inflight_spinner_() override;
    void decode_video_first_frame_(
        std::vector<std::uint8_t> bytes,
        std::function<void(std::vector<std::uint8_t>)> done) override;
    void cache_rgba_image_(const tk::CacheKey& key, int w, int h,
                           std::vector<uint8_t> rgba) override;
    std::vector<tk::Rect> get_screen_work_areas_() const override;

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

    // WM_COMMAND ids IDC_SHORTCUT_BASE + i are the global shortcuts from
    // views/shortcut_registry.h; accel_events_[i] is the KeyEvent the i-th
    // forwards to main_app_->dispatch_key_down.
    static constexpr int IDC_SHORTCUT_BASE = 130;
    static constexpr std::size_t kMaxShortcuts = 64;
    std::vector<tk::KeyEvent> accel_events_;
    // A registry chord as an accelerator (fVirt + key); false when it has
    // no accelerator form on the active keyboard layout.
    static bool accel_from_chord_(const tk::KeyChord& chord, ACCEL& out);

    // Accelerator tables built from the registry by rebuild_accelerators_
    // (on create, and again when the keyboard layout changes), applied by
    // pre_translate_message so the shortcuts fire even while the compose /
    // search edit controls have focus. accel_: every global shortcut, for
    // this window; accel_app_: Application-scoped ones only, for pop-outs.
    void rebuild_accelerators_();
    void destroy_accelerators_();
    HACCEL accel_ = nullptr;
    HACCEL accel_app_ = nullptr;
    HKL accel_layout_ = nullptr; // layout the tables were built for
};

} // namespace win32
